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
#define WS_PUSH_PERIOD_MS       100        // ~10 fps
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
static uint8_t        s_payload[WS_FRAME_LEN];
static httpd_ws_frame_t s_ws_frame;  // static, NOT stack-local

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
            uint8_t bye = WS_FRAME_TYPE_TAKEOVER;
            httpd_ws_frame_t f = {
                .final = true, .type = HTTPD_WS_TYPE_BINARY,
                .payload = &bye, .len = 1,
            };
            httpd_handle_t h = s_server ? s_server : req->handle;
            esp_err_t terr = httpd_ws_send_frame_async(h, s_ws_fd, &f);
            if (terr != ESP_OK)
                ESP_LOGD(TAG, "takeover notice to fd=%d failed: %s (closing anyway)",
                         s_ws_fd, esp_err_to_name(terr));
        }
        httpd_sess_trigger_close(s_server ? s_server : req->handle, s_ws_fd);
    }
    s_ws_fd = fd;
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
    s_ws_frame.final   = true;
    s_ws_frame.type    = HTTPD_WS_TYPE_BINARY;
    s_ws_frame.payload = s_payload;
    s_ws_frame.len     = WS_FRAME_LEN;
    s_payload[0] = WS_FRAME_TYPE_SPECTRUM;

    const int   N     = DSP_FFT_SIZE;
    const int   half  = N / 2;
    const float scale = 255.0f / (WS_DB_MAX - WS_DB_MIN);

    TickType_t last   = xTaskGetTickCount();
    uint32_t   sent   = 0;
    TickType_t fps_at = last;
    int        fail_streak    = 0;   // consecutive async-send failures
    TickType_t fail_streak_at = 0;   // tick of the first failure in the streak

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
        vTaskDelayUntil(&last, pdMS_TO_TICKS(WS_PUSH_PERIOD_MS));

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

        esp_err_t err = httpd_ws_send_frame_async(s_server, s_ws_fd, &s_ws_frame);
        if (err != ESP_OK) {
            TickType_t now_fail = xTaskGetTickCount();
            if (++fail_streak == 1) fail_streak_at = now_fail;
            uint32_t streak_ms = (uint32_t)(now_fail - fail_streak_at) * portTICK_PERIOD_MS;
            if (fail_streak < MAX_FAIL_STREAK && streak_ms < (uint32_t)MAX_FAIL_STREAK_MS) {
                // Transient: skip this frame, keep the session, retry next tick.
                continue;
            }
            ESP_LOGW(TAG, "async send failed fd=%d: %s (%d in a row, %ums); closing session",
                     s_ws_fd, esp_err_to_name(err), fail_streak, (unsigned)streak_ms);
            // Close the socket so its LWIP slot is freed (see takeover note above).
            int dead = s_ws_fd;
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
            ESP_LOGI(TAG, "tx %.1f fps", fps);
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
