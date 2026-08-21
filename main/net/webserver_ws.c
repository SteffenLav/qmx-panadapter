#include "webserver_ws.h"

#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"   // setsockopt / SO_SNDTIMEO on the WS fd

#include "cat.h"
#include "dsp.h"

#define WS_FRAME_TYPE_SPECTRUM  0x01
// Sent to a client that is about to be displaced by a newer one, so it can stop
// reconnecting and say what happened instead of flapping. There is only ever ONE
// live spectrum client - see the takeover note in ws_uri_handler().
#define WS_FRAME_TYPE_TAKEOVER  0x02
// ~10 fps. NOTE (2026-07-14): halving this to 5 fps was tried as a fix for a
// browser-stream stutter + a core-0-saturation reboot, on the theory that the
// WS TX path (httpd_ws_send_frame_async → LWIP/esp_hosted, core 0) dominated
// core 0. MEASURED WRONG: at 5 fps core-0 idle was unchanged (~12%). The real
// core-0 load is the audio→FFT→render→LVGL-rotation pipeline, which is active
// whenever the QMX streams audio (idle0 56%→12% coincides with USB-audio
// connect, NOT with a browser connecting). The WS rate is a minor contributor;
// reverted to 10 fps so the browser view isn't needlessly choppy. The core-0
// saturation (with core 1 ~94% idle) is the thing to fix — by rebalancing to
// core 1 / cutting the rotation cost — not here.
#define WS_PUSH_PERIOD_MS       100        // ~10 fps - the FLOOR, i.e. fastest
// Adaptive rate (#232). The period is raised when the link repeatedly cannot
// take a frame and walked back down when it can, between these bounds.
// 250 ms = 4 fps is as slow as this is allowed to get. Lower was tried at 400
// and is not worth it: the extra 1.5 fps of headroom buys little on a link this
// size, and a panadapter at 2.5 fps stops reading as a live display.
#define WS_PERIOD_MAX_MS        250
// Additive both directions, deliberately asymmetric: concede 20 ms at a time,
// take back 5 ms at a time. Full range is therefore ~1.5 s down and ~6 s back,
// slow enough that neither transition is visible as a jump.
#define WS_PERIOD_BACKOFF_MS    20
#define WS_PERIOD_RECOVER_MS    5
#define WS_HEADER_LEN           2
#define WS_PAYLOAD_LEN          DSP_FFT_SIZE
#define WS_FRAME_LEN            (WS_HEADER_LEN + WS_PAYLOAD_LEN)

// Quantization range: -130 dBm (q=0) .. -30 dBm (q=255). ~0.39 dB/step.
// The quantisation window for the spectrum bytes. The FLOOR matters more than it
// looks: everything below it arrives as byte 0, and the browser's waterfall tracks a
// per-bin noise floor out of these bytes exactly as render_waterfall.c does out of
// floats. At -130 a QUARTER of the band was pinned at 0 on a quiet 20 m (measured
// 2026-08-11: 24.7 % of bins at exactly 0, 44.7 % below byte 10), because -130 dBm
// is where this receiver's own noise floor sits - see DSP_DB_CALIBRATION_OFFSET. A
// floor tracker cannot find a floor it cannot see, so the browser's black level had
// nothing to gate against and the waterfall came out as blue speckle where the Tab5
// showed black.
//
// -150 puts 20 dB of headroom under the noise. The cost is resolution, 0.39 -> 0.47
// dB per count, which is invisible next to a 24 dB contrast span - and the EMA the
// browser applies before colouring interpolates between counts anyway.
//
// ⚠ WIRE FORMAT. The browser derives its own counts-per-dB from these two numbers
// (WS_DB_MIN/WS_DB_MAX in index.html) - change one side and you must change the
// other, or every dB figure in the browser is silently wrong.
#define WS_DB_MIN   (-150.0f)
#define WS_DB_MAX   (-30.0f)

// Must match IF_OFFSET_HZ inside main/ui/ui.c (QMX dial sits at +12 kHz baseband).
#define WS_IF_OFFSET_HZ 12000

static const char *TAG = "ws";

// Shared state between the URI handler and the push task. The URI handler
// captures (server, fd) on handshake and returns immediately so the httpd
// worker is free to serve /api/status while a WS session is active.
//
// Async sends MUST use static (non-stack) frame storage -- the v0.8.x lesson
// was that httpd_ws_send_frame_async with a stack-local frame struct silently
// drops frames and returns ESP_OK.
static httpd_handle_t volatile s_server   = NULL;
static int            volatile s_ws_fd    = -1;
static volatile bool           s_session_active = false;

static float          s_spec[DSP_FFT_SIZE];

// WS wire buffer: the RFC 6455 header is built directly IN FRONT of the payload
// so header and body leave as ONE contiguous send. A split between them is one of
// the two ways the stream can desynchronise (see ws_send_all below).
// Layout: [0..3] = 4-byte header for a 126-length frame, [4..] = our payload.
#define WS_WIRE_HDR_LEN 4
static uint8_t        s_txbuf[WS_WIRE_HDR_LEN + WS_FRAME_LEN];
static uint8_t *const s_payload = s_txbuf + WS_WIRE_HDR_LEN;

// Frames that needed more than one send() to go out, i.e. instances of the
// partial write that used to silently corrupt the stream. Reported on the fps
// line. This is the ONLY evidence that the fix is doing something: before it,
// these were indistinguishable from healthy sends (IDF returned ESP_OK), which
// is the same trap #189 documents for the silent USB patches.
static uint32_t       s_partial_writes = 0;
// #217 (Samuel W7STF): he reports occasional 3-6 s PSD stalls after the #193
// fix and is careful to say it may be his PC. It might well be - but nobody
// could tell, because every number that would settle it lived in a periodic
// LOG LINE the operator never sees. These are reported in /api/status so a
// stall becomes attributable instead of arguable: if a stall lines up with a
// teardown the device caused it, and if these do not move during one, it is
// the network or the browser.
static uint32_t       s_sessions   = 0;   // WS sessions accepted
static uint32_t       s_takeovers  = 0;   // a new client displacing a stale one
static uint32_t       s_closes     = 0;   // sessions ended device-side

static TaskHandle_t   s_push_task = NULL;

// When a network transfer (QRZ/eQSL upload, ADIF/diag download) is in flight,
// the push task suspends the ~10 fps spectrum stream so the transfer gets the
// WiFi TX path to itself. The stream is bandwidth-heavy; sharing the single
// SDIO->C6 link with an outbound TLS connect stalls both (the connect times
// out, the WS sends back up with EAGAIN). Paused = behave like idle.
static volatile bool s_ws_paused = false;

void webserver_ws_set_paused(bool paused)
{
    s_ws_paused = paused;
}

// ---------------------------------------------------------------------------
// Sending a WS frame WITHOUT the partial-write bug in IDF's own helper.
//
// ⛔ Do NOT go back to httpd_ws_send_frame_async() for the spectrum stream.
// Verified by reading the PINNED IDF v5.4.4 source
// (components/esp_http_server/src/httpd_ws.c), not inferred: it sends the header
// and the payload with two BARE calls to sess->send_fn() and checks only "< 0".
// httpd_default_send() is a single send() that RETURNS A BYTE COUNT, and a short
// count is not negative - so a PARTIAL TCP WRITE IS REPORTED TO US AS ESP_OK.
// The ordinary HTTP path does not have this bug: it uses httpd_send_all(), which
// loops until the buffer is drained. The WS path simply never got that loop.
//
// This bites here harder than it would anywhere else, because ws_uri_handler()
// deliberately sets SO_SNDTIMEO to 400 ms (so a stuck send cannot freeze the
// single httpd worker - the 2026-07-14 fix). A send timeout on a socket that has
// already queued some bytes is EXACTLY how a short count arises, so on a
// congested link the partial write is not a rare edge case, it is the expected
// outcome. The freeze fix and this bug are the same line of code.
//
// The consequence is not a dropped frame, it is a CORRUPT STREAM. The browser has
// been told to expect WS_FRAME_LEN payload bytes; it gets fewer, so it consumes
// the NEXT frame's header as the tail of this one and every frame afterwards is
// misparsed. The browser then sees a protocol violation and closes the socket -
// which is why the symptom is "the panadapter freezes for a few seconds and then
// comes back", not "one glitchy frame".
//
// MEASURED on a 9.6 h capture of v1.8.6-7-g7f387ab, before this fix:
//   * 545 session teardowns, median 14.4 s apart
//   * each costing the browser a median 2.2 s (p90 4.5 s) blackout while it
//     reconnected - matching Samuel W7STF's "several seconds later it begins to
//     animate again" (#177196)
//   * feed activity enriched in the 2 s before a teardown: RBN 8.4x, DX cluster
//     8.6x, TLS handshake 5.7x, SDIO recovery 4.6x, against an 8.1 % base rate
// Socket exhaustion was TESTED AND FALSIFIED as the cause (0 accept/ENFILE
// errors), so do not go looking there again.
// ---------------------------------------------------------------------------
typedef enum {
    WS_TX_OK = 0,    // the whole frame reached the socket
    WS_TX_SKIPPED,   // nothing was sent; frame abandoned cleanly, stream still in sync
    WS_TX_DEAD,      // session gone, or the frame could not be completed
} ws_tx_result_t;

// Send every byte or report why not. The invariant this exists to keep: once ANY
// byte of a frame is on the wire we are committed - abandoning it mid-frame is
// what desynchronises the stream, so the only honest outcomes from that point are
// "finished it" or "closed the session".
// out_sends / out_ms report HOW HARD the frame was to push, for the adaptive
// rate in ws_push_task(). Both may be NULL. A frame that needed more than one
// socket write means the TCP send buffer was full - i.e. the link could not
// take it in one go - which is the most direct congestion signal available
// here, and it was already being counted for the #193 healer.
static ws_tx_result_t ws_send_all(httpd_handle_t hd, int fd,
                                  const uint8_t *buf, size_t len,
                                  uint32_t budget_ms,
                                  int *out_sends, uint32_t *out_ms)
{
    size_t     off   = 0;
    int        sends = 0;
    TickType_t start = xTaskGetTickCount();

    while (off < len) {
        int r = httpd_socket_send(hd, fd, (const char *)buf + off, len - off, 0);
        if (r > 0) {
            off += (size_t)r;
            sends++;
            continue;
        }
        // The session has already been removed from httpd's table. This is not a
        // transient condition and never becomes one, so waiting on it is pure
        // added blackout - close now. (Same reading as httpd_ws_send_frame_async
        // returning ESP_ERR_INVALID_ARG, which is what we used to burn 15
        // attempts on.)
        if (r == HTTPD_SOCK_ERR_INVALID) return WS_TX_DEAD;
        if (r == HTTPD_SOCK_ERR_FAIL)    return WS_TX_DEAD;

        // HTTPD_SOCK_ERR_TIMEOUT (EAGAIN: the TCP send buffer is full).
        // Nothing sent yet => the stream is still perfectly in sync, so dropping
        // this frame is free and the session survives the congestion.
        if (off == 0) return WS_TX_SKIPPED;

        // Committed mid-frame. Keep trying inside a bounded budget rather than
        // leaving the browser parsing garbage.
        if ((uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS) >= budget_ms)
            return WS_TX_DEAD;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (sends > 1) s_partial_writes++;   // would have been silent corruption before
    if (out_sends) *out_sends = sends;
    if (out_ms)    *out_ms = (uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
    return WS_TX_OK;
}

// Build an unmasked binary frame header in front of `payload_len` bytes that are
// already sitting at buf + WS_WIRE_HDR_LEN, then send the lot in one go.
static ws_tx_result_t ws_send_binary(httpd_handle_t hd, int fd,
                                     uint8_t *buf, size_t payload_len,
                                     uint32_t budget_ms,
                                     int *out_sends, uint32_t *out_ms)
{
    // ⛔ RFC 6455 §5.2 requires the MINIMAL length encoding: the 7-bit field for
    // 0..125, and the 126 + 16-bit form ONLY for 126..65535. The old code used
    // the 16-bit form unconditionally under the comment "126 is valid for
    // anything <= 65535" - which is wrong, and it mattered for exactly one
    // frame: the 1-byte TAKEOVER notice.
    //
    // That notice exists so a displaced browser can stand down instead of
    // retrying, and the ping-pong it prevents is documented right where it is
    // sent (340 takeovers in one session). But the notice itself was malformed,
    // so a strict client - i.e. every browser - failed the connection rather
    // than reading it, reconnected, and took the slot straight back. The cure
    // was re-creating the disease.
    //
    // Measured on the bench 2026-08-20 with two tabs open: 19 sessions and
    // **16 takeovers** in about ten seconds, each costing the displaced tab a
    // reconnect. That is very likely the residual "PSD stalls for 3-6 s" report
    // (#217, Samuel W7STF) whenever a second browser or phone is left open.
    //
    // The header is still built IN FRONT of the payload so the two can never be
    // split across sends (the #193 rule); a 2-byte header simply starts two
    // bytes later in the same buffer.
    uint8_t *frame;
    size_t   hdr;
    if (payload_len <= 125) {
        hdr   = 2;
        frame = buf + WS_WIRE_HDR_LEN - hdr;
        frame[1] = (uint8_t)payload_len;
    } else {
        hdr   = 4;
        frame = buf;
        frame[1] = 126;
        frame[2] = (uint8_t)((payload_len >> 8) & 0xFF);
        frame[3] = (uint8_t)(payload_len & 0xFF);
    }
    frame[0] = 0x80 | HTTPD_WS_TYPE_BINARY;    // FIN + opcode 0x2
    return ws_send_all(hd, fd, frame, hdr + payload_len, budget_ms, out_sends, out_ms);
}

// httpd worker has 1 task, so this URI handler runs in the only worker.
// We must return ESP_OK quickly so subsequent /api/status requests can be served.
static esp_err_t ws_uri_handler(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        // Incoming WS frame (CLOSE/PING/etc). We do not subscribe to control
        // frames (handle_ws_control_frames=false) so this is mostly unreachable.
        return ESP_OK;
    }

    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        ESP_LOGE(TAG, "httpd_req_to_sockfd failed");
        return ESP_FAIL;
    }

    // Single-client, but LAST CONNECTION WINS rather than refusing. An ungraceful
    // client disconnect (tab reload, Wi-Fi blip, device reboot) can leave a
    // half-open socket that keeps "accepting" async sends, so the old session
    // never frees via the send-failure path - refusing the reconnect then strands
    // the browser on "reconnecting" forever while HTTP still works. Taking over
    // the slot makes reconnects always succeed; the stale fd is closed explicitly
    // (NOT just abandoned) so its LWIP socket is freed - otherwise repeated
    // freeze/reconnect cycles leak sockets until accept() fails with ENFILE
    // (LWIP_MAX_SOCKETS exhausted) and the server stops accepting connections.
    if (s_session_active && s_ws_fd != fd && s_ws_fd >= 0) {
        ESP_LOGW(TAG, "New client fd=%d takes over stale fd=%d (closing stale)", fd, s_ws_fd);
        s_takeovers++;
        // TELL THE DISPLACED CLIENT WHY, before closing it. Without this the old
        // browser only sees its socket close, retries after 2 s, takes the slot
        // back, and the two ping-pong forever - 340 takeovers in one session while
        // both showed nothing but "reconnecting", which reads as a network fault and
        // sent an hour of debugging in the wrong direction. A browser that knows it
        // was displaced can stand down and say so.
        //
        // One byte, sent synchronously to the OTHER fd before the close is queued. A
        // browser that predates this frame type ignores any frame whose first byte
        // is not WS_FRAME_TYPE_SPECTRUM, so it is backward compatible; the failure
        // mode if the stale socket is genuinely dead is that this send fails and the
        // close proceeds exactly as before.
        {
            uint8_t bye[WS_WIRE_HDR_LEN + 1];
            bye[WS_WIRE_HDR_LEN] = WS_FRAME_TYPE_TAKEOVER;
            httpd_handle_t h = s_server ? s_server : req->handle;
            // Same all-or-nothing send as the stream: a half-written notice would
            // desynchronise the very socket we are about to close, and the stale
            // browser would report a protocol error instead of the reason.
            if (ws_send_binary(h, s_ws_fd, bye, 1, 200, NULL, NULL) != WS_TX_OK)
                ESP_LOGD(TAG, "takeover notice to fd=%d did not go out (closing anyway)",
                         s_ws_fd);
        }
        httpd_sess_trigger_close(s_server ? s_server : req->handle, s_ws_fd);
    }
    s_ws_fd = fd;
    s_sessions++;
    s_session_active = true;

    // Bound how long a single WS send may block. On a weak/congested link
    // (this board's esp_hosted WiFi, worse at low RSSI) a send into a full TCP
    // window was serial-captured blocking ~3 s ("2 in a row, 6000ms"); because
    // httpd is single-threaded, that froze the WHOLE web server — even
    // /api/status — for seconds (the "big time freeze", 2026-07-14). A short
    // SO_SNDTIMEO makes a stuck send fail fast with EAGAIN so the push task's
    // fail-streak logic tears the session down and the httpd worker is freed in
    // <0.5 s instead of blocking; the browser then reconnects cleanly. This
    // bounds the freeze DURATION; it can't speed up a weak link (that's the
    // -77 dBm signal), so brief blips/reconnects still happen when it congests.
    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };  // 400 ms
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    ESP_LOGI(TAG, "Client connected, fd=%d -- push task takes over", fd);
    return ESP_OK;  // <-- frees httpd worker immediately
}

// Push task: sleeps when no session, runs at ~10 fps when one is open.
static void ws_push_task(void *arg)
{
    (void)arg;
    s_payload[0] = WS_FRAME_TYPE_SPECTRUM;

    const int   N     = DSP_FFT_SIZE;
    const int   half  = N / 2;
    const float scale = 255.0f / (WS_DB_MAX - WS_DB_MIN);

    TickType_t last   = xTaskGetTickCount();
    uint32_t   sent   = 0;
    TickType_t fps_at = last;
    int        fail_streak    = 0;   // consecutive async-send failures
    TickType_t fail_streak_at = 0;   // tick of the first failure in the streak
    int        period_ms      = WS_PUSH_PERIOD_MS;  // adaptive, see below
    int        hard_streak    = 0;   // consecutive frames the link struggled with

    // Tolerate a short burst of transient send failures (EAGAIN: the TCP send
    // buffer is momentarily full, common when the C6 link is briefly congested)
    // before tearing the session down. Killing on the first failure made the
    // browser "freeze then need a manual reconnect"; skipping the frame and
    // retrying rides through the hiccup. Only a sustained failure (dead socket)
    // closes the session.
    //
    // MAX_FAIL_STREAK alone assumes each failed attempt costs ~100ms (the
    // nominal push period) - field-observed wrong: with a stale-but-not-yet-
    // reset socket (e.g. the browser's own network dropped without a clean
    // TCP close), httpd_ws_send_frame_async() itself can block for several
    // seconds per call, so 15 failures took ~33s wall-clock instead of the
    // assumed ~1.5s - a 30+ second frozen page before the existing recovery
    // even kicked in. MAX_FAIL_STREAK_MS is a wall-clock backstop: force-close
    // once a streak has been open this long, regardless of how many discrete
    // attempts that represents.
    const int  MAX_FAIL_STREAK    = 15;     // ~1.5 s at 100 ms/frame, the fast path
    const int  MAX_FAIL_STREAK_MS = 5000;   // hard ceiling regardless of attempt count

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(period_ms));

        // Keep the socket fresh WHILE PAUSED too. The refresh below only runs
        // after a successful send, so a stream paused for an upload stops being
        // refreshed for the whole transfer and drifts back to the bottom of the
        // LRU pile - which is exactly the eviction this is meant to prevent,
        // reappearing during the one operation that already stresses the link.
        if (s_session_active && s_server && s_ws_fd >= 0 && s_ws_paused)
            httpd_sess_update_lru_counter(s_server, s_ws_fd);

        if (!s_session_active || s_server == NULL || s_ws_fd < 0 || s_ws_paused) {
            // Idle (or paused for a transfer): reset fps counter so we do not
            // log stale stats on reconnect, and yield the link to the transfer.
            sent = 0;
            fps_at = xTaskGetTickCount();
            fail_streak = 0;   // don't carry a pre-pause streak into resume
            fail_streak_at = 0;
            continue;
        }

        // Select spectrum source. When zoom-FFT is active the DSP has already
        // mixed the zoom center down to DC; only an fftshift is needed and the
        // browser gets higher-resolution bins. When inactive (or not ready yet),
        // fall back to the base 1024-bin spectrum with a full IF shift.
        int decim = dsp_get_zoom_decim();
        const float *spec_data = NULL;
        int if_shift = 0;

        if (decim > 1) {
            spec_data = dsp_get_zoom_spectrum();  // NULL if accumulation still filling
        }
        if (spec_data == NULL) {
            decim = 1;
            if (dsp_get_spectrum(s_spec) != ESP_OK) continue;
            spec_data = s_spec;
            // CW mode: add CW pitch on top of base 12 kHz IF so the browser
            // centers on the audio tone, matching the Tab5 display.
            int total_if_hz = WS_IF_OFFSET_HZ;
            const char *mode = cat_get_mode_str();
            if (mode && strcmp(mode, "CW") == 0) {
                total_if_hz += cat_get_cw_offset_hz();
            }
            if_shift = (total_if_hz * N + DSP_SAMPLE_RATE_HZ / 2) / DSP_SAMPLE_RATE_HZ;
        }

        // byte[1] carries the decimation factor so the browser can apply
        // residual zoom instead of full zoom when rendering zoom-FFT data.
        s_payload[1] = (uint8_t)decim;

        // fftshift (+ IF shift for base spectrum path)
        for (int i = 0; i < N; i++) {
            int bin = (i < half) ? (i + half) : (i - half);
            if (if_shift) bin = ((bin + if_shift) % N + N) % N;
            float db = spec_data[bin];
            if (db < WS_DB_MIN) db = WS_DB_MIN;
            else if (db > WS_DB_MAX) db = WS_DB_MAX;
            int q = (int)((db - WS_DB_MIN) * scale + 0.5f);
            s_payload[WS_HEADER_LEN + i] = (uint8_t)q;
        }

        // Budget for finishing a frame we are already committed to. Deliberately
        // longer than one push period: dropping the session costs the browser a
        // ~2 s reconnect, so riding out a congestion burst is much cheaper than
        // being strict here.
        int      frame_sends = 0;
        uint32_t frame_ms    = 0;
        ws_tx_result_t tx = ws_send_binary(s_server, s_ws_fd, s_txbuf, WS_FRAME_LEN,
                                           1500, &frame_sends, &frame_ms);

        // ---- adaptive rate ------------------------------------------------
        // This stream is the single biggest consumer of a small link, and at a
        // fixed 10 fps it WINS: measured on the bench, the spectrum kept its
        // 9.7-10.2 fps while a page load beside it crawled at 2.7 KB/s and the
        // browser's own polls timed out. 1026 bytes x 10 fps = 10.0 KB/s of a
        // link measured at ~12.7 KB/s, so everything else divides what is left.
        //
        // A frame that needed more than one socket write, or that took longer
        // than a push period to get out, means the send buffer was full - the
        // link could not take it. That is the moment to yield, and it is a
        // signal we already compute (see ws_send_all).
        //
        // Backing off is deliberately FAST and recovery deliberately SLOW: the
        // cost of being too quick to speed up is starving the very transfer we
        // just made room for, whereas the cost of recovering slowly is a few
        // seconds of a slightly lower frame rate that nobody can see. A healthy
        // link never leaves WS_PERIOD_MIN_MS, so this is invisible until it is
        // needed.
        // ⚠ GENTLY, and only on SUSTAINED difficulty. The first version backed
        // off x1.5 on a single hard frame and crawled back 20 ms at a time,
        // which swings 10 fps -> 3 fps and back within a couple of seconds. The
        // operator saw exactly that: "spectrum is running - but very erratic".
        // A panadapter is judged on looking smooth, so a controller that lurches
        // is worse than one that is slightly too fast: an occasional hard frame
        // is normal on this link and must not move the rate at all.
        //
        // Additive both ways, and two hard frames IN A ROW before conceding
        // anything. That turns the response into a slow glide the eye does not
        // catch, while still yielding within a second or two of real congestion.
        if (tx == WS_TX_OK) {
            // ⛔ KEEP THIS SOCKET OUT OF THE LRU BIN. Without it httpd closes the
            // spectrum stream every few seconds and the browser shows
            // "reconnecting" - measured session lifetimes of 5.8-14.1 s on the
            // bench, with NO device-side close logged anywhere, which is what
            // sent this hunting through the network for a fault that was here.
            //
            // Verified by reading the PINNED IDF v5.4.4 source rather than
            // inferred: components/esp_http_server/src/httpd_sess.c updates
            // session->lru_counter in exactly ONE place, at the end of
            // httpd_sess_process(), i.e. when a request is RECEIVED on that
            // socket. httpd_sess_update_lru_counter() is a public helper that
            // the component itself never calls - it exists for cases like ours.
            // httpd_socket_send(), which this stream uses, does not touch it.
            //
            // Our socket only ever SENDS and never receives (control frames are
            // not subscribed), so its counter is frozen at the handshake while
            // every other request bumps the global one. It is therefore ALWAYS
            // the least-recently-used session, and with max_open_sockets=10 and
            // lru_purge_enable=true it is the one httpd evicts whenever the
            // table fills. The browser's own /api/status and /api/decodes polls
            // are enough to fill it, so the stream was being killed by the page
            // that was watching it.
            httpd_sess_update_lru_counter(s_server, s_ws_fd);

            bool hard = (frame_sends > 1) || (frame_ms >= (uint32_t)period_ms);
            if (hard) {
                if (++hard_streak >= 2) {
                    period_ms += WS_PERIOD_BACKOFF_MS;
                    if (period_ms > WS_PERIOD_MAX_MS) period_ms = WS_PERIOD_MAX_MS;
                    hard_streak = 0;
                }
            } else {
                hard_streak = 0;
                if (period_ms > WS_PUSH_PERIOD_MS) {
                    period_ms -= WS_PERIOD_RECOVER_MS;
                    if (period_ms < WS_PUSH_PERIOD_MS) period_ms = WS_PUSH_PERIOD_MS;
                }
            }
        }

        if (tx == WS_TX_SKIPPED) {
            // Nothing went out and the stream is still in sync. Count it, so a
            // link that is congested for a sustained period still ends the
            // session rather than showing a frozen page forever.
            TickType_t now_fail = xTaskGetTickCount();
            if (++fail_streak == 1) fail_streak_at = now_fail;
            uint32_t streak_ms = (uint32_t)(now_fail - fail_streak_at) * portTICK_PERIOD_MS;
            if (fail_streak < MAX_FAIL_STREAK && streak_ms < (uint32_t)MAX_FAIL_STREAK_MS)
                continue;
            ESP_LOGW(TAG, "send blocked fd=%d (%d in a row, %ums); closing session",
                     s_ws_fd, fail_streak, (unsigned)streak_ms);
            tx = WS_TX_DEAD;
        }

        if (tx == WS_TX_DEAD) {
            // Close the socket so its LWIP slot is freed (see takeover note above).
            int dead = s_ws_fd;
            s_closes++;
            s_session_active = false;
            s_ws_fd = -1;
            fail_streak = 0;
            fail_streak_at = 0;
            if (s_server && dead >= 0) httpd_sess_trigger_close(s_server, dead);
            continue;
        }
        fail_streak = 0;
        fail_streak_at = 0;

        sent++;
        TickType_t now = xTaskGetTickCount();
        if ((now - fps_at) >= pdMS_TO_TICKS(5000)) {
            float fps = (float)sent * 1000.0f / (float)pdTICKS_TO_MS(now - fps_at);
            // `partial` counts frames that needed more than one send() to go out.
            // Every one of those was, before this fix, a silently corrupted stream
            // and therefore a browser-side disconnect a moment later. A non-zero
            // count here is the fix working, NOT a fault.
            // `target` is the adaptive rate the link is currently allowing. If
            // it sits below 10 fps the stream is deliberately yielding, which
            // is the fix working rather than a fault - and it makes "the web UI
            // felt slow" attributable instead of arguable.
            ESP_LOGI(TAG, "tx %.1f fps (target %.1f, partial writes healed: %u)",
                     fps, 1000.0f / (float)period_ms, (unsigned)s_partial_writes);
            sent = 0;
            fps_at = now;
        }
    }
}

esp_err_t webserver_ws_start(httpd_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    if (s_server) return ESP_OK;

    s_server         = server;
    s_session_active = false;
    s_ws_fd          = -1;

    static const httpd_uri_t uri_ws = {
        .uri                      = "/ws",
        .method                   = HTTP_GET,
        .handler                  = ws_uri_handler,
        .user_ctx                 = NULL,
        .is_websocket             = true,
        .handle_ws_control_frames = false,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /ws: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    if (s_push_task == NULL) {
        // Priority kept below fft_task's 4 (dsp.c) - fft_task is the audio ring
        // buffer's sole consumer for both the panadapter spectrum and FT8
        // capture. At priority 5 this task could preempt it every 100 ms
        // whenever a browser tab is open, the same hazard class that
        // regressed FT8 decode yield via cw_audio_task (see CLAUDE.md).
        BaseType_t ok = xTaskCreate(ws_push_task, "ws_push", 4096, NULL, 3, &s_push_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "xTaskCreate ws_push failed");
            s_server = NULL;
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "WS /ws ready (push task, target %d ms period)", WS_PUSH_PERIOD_MS);
    return ESP_OK;
}

void webserver_ws_stop(void)
{
    if (!s_server) return;
    s_session_active = false;
    s_ws_fd = -1;
    s_server = NULL;
    // Leave the push task running idle -- it will pick up the next start cleanly.
    ESP_LOGI(TAG, "WS stopped");
}

// #217: the WS health counters, for /api/status. Read-only snapshot; a torn
// read is harmless here because these only ever increase and are for eyeballing
// against a reported stall, not for control flow.
void webserver_ws_stats(uint32_t *sessions, uint32_t *takeovers,
                        uint32_t *closes, uint32_t *partial)
{
    if (sessions)  *sessions  = s_sessions;
    if (takeovers) *takeovers = s_takeovers;
    if (closes)    *closes    = s_closes;
    if (partial)   *partial   = s_partial_writes;
}
