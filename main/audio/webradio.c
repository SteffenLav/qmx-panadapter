// Web-radio player. See webradio.h for the ownership rules and why this
// borrows the speaker instead of opening a channel.
//
// ⛔ SHAPE: A READER TASK, A RING BUFFER, AND A DECODE TASK - and the first
// cut of this file got that wrong deliberately, which is why it is written
// down at length.
//
// The first version used ONE task for fetch, decode and write, and its header
// comment argued for it: "the codec write blocks and paces the loop for free,
// so a second task would add a buffer, a semaphore and a second stack to
// solve a problem this does not have." It broke up continuously, and the
// argument was backwards. In one task, EVERY WAIT FOR THE NETWORK IS A GAP AT
// THE SPEAKER: while the loop sat in read_full() waiting for its next 12.3 KB
// - 768 ms of audio at 128 kbps - the only thing feeding the codec was the
// I2S DMA ring, which is dma_desc_num 12 x dma_frame_num 320 = 80 ms.
//
// ⛔ CPU WAS NOT THE CAUSE, AND ONE FIX ALREADY MISSED BECAUSE OF THAT. The
// first diagnosis blamed a memmove-per-byte in the resync path. That WAS a
// real fault and it is fixed (ea93df76), but it was not what the operator
// heard: a task BLOCKED on a socket is not busy, so cpu_stats could never see
// this. Measured on field 2026-11-01, idle1 while playing (61-85%) is
// indistinguishable from idle1 while stopped (54-89%).
//
// ⭐ IT WAS SETTLED BY COMPARISON, NOT BY THEORY. dmatking/esp32-p4-webradio
// v2.0.1 runs the same Helix decoder on the same Tab5 and sounds perfect; the
// operator had it cable-flashed on the field bench the same day. Reading its
// source (main/stream.c, main/mp3_decoder.c) the difference is one thing:
//   - a reader task that reads 2048 B at a time, and
//   - a 64 KB FreeRTOS stream buffer - FOUR SECONDS at 128 kbps - between it
//     and a decode task that pulls with a 100 ms timeout.
// Its I2S DMA ring is the IDF default 6 x 240 = 30 ms, SHALLOWER than ours,
// so the DMA depth was never the difference. Four seconds of decoupling was.
//
// ⚠ Two parts of its design we CANNOT copy. It calls
// audio_set_sample_rate(fi.samprate) and reopens the codec at the stream's
// rate; ours is opened once at boot at 48 kHz and cannot be reopened
// (webradio.h says why), so the resampler below stays. And its reader drops
// an ICY block that straddles two reads; the state machine below carries it
// instead, because dropping it loses audio, not just a title.
#include "webradio.h"

#include "rx_audio.h"
#include "dsp/dsp.h"              /* DSP_SAMPLE_RATE_HZ - the channel's rate   */

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"   /* xStreamBufferCreateWithCaps - PSRAM */

#include "mp3dec.h"               /* chmorgan/esp-libhelix-mp3 */
#include "mp3common.h"            /* MP3DecInfo - public, and it holds the 8 */
#include "util/mem_where.h"       /* LP SRAM is not L2. T133/T134 */

#include <stdlib.h>
#include <string.h>

static const char *TAG = "webradio";

/* ---- state, written by the tasks and read by LVGL -----------------------
 *
 * Plain variables behind no lock, and that is a decision rather than an
 * oversight: every reader is a 1 Hz repaint that can tolerate a torn string
 * for one frame, and the alternative - a mutex taken on the audio path - buys
 * a repaint nothing can see at the cost of a lock in the decode loop. The
 * strings are only ever written whole, by one writer. */
static volatile webradio_state_t s_state  = WEBRADIO_STOPPED;
static volatile bool             s_paused = false;
static volatile bool             s_stop_req = false;
static TaskHandle_t              s_task   = NULL;   /* decoder */
static TaskHandle_t              s_net    = NULL;   /* reader  */
static volatile bool             s_net_done = true;

static char s_url[192]  = "";
static char s_name[48]  = "";
static char s_title[96] = "";
static char s_err[80]   = "";

/* ---- buffers ------------------------------------------------------------
 *
 * ⛔ ALL IN PSRAM. Internal RAM on this board runs within ~30 KB of empty
 * (#65) and none of this is DMA - including the ring, which is why it is
 * created with xStreamBufferCreateWithCaps() and NOT xStreamBufferCreate(),
 * whose buffer would come out of the internal heap.
 *
 * RING_BYTES is the jitter buffer and the whole point of this file's second
 * version: 64 KB = 4.0 s at 128 kbps, the same figure the reference uses.
 * IN_BYTES only has to hold a frame or two for the decoder; the largest MP3
 * frame is MAINBUF_SIZE. */
#define RING_BYTES  (64 * 1024)
#define NET_CHUNK   2048                       /* one socket read - 128 ms   */
#define IN_BYTES    (MAINBUF_SIZE * 4)
#define PCM_SAMPLES (MAX_NGRAN * MAX_NSAMP * MAX_NCHAN)   /* one frame, 2304 */
/* The resampler expands when the stream is slower than the channel: 48/8 kHz
 * is the worst legal MPEG2.5 case, and a truncated write is a click, so this
 * is sized for it rather than clipped. */
#define OUT_FRAMES  (PCM_SAMPLES * 3)

static uint8_t             *s_in;
static int16_t             *s_pcm;
static int16_t             *s_out;
static StreamBufferHandle_t s_ring;

/* ---- ICY metadata -------------------------------------------------------
 *
 * The server interleaves a metadata block every `icy-metaint` bytes of audio:
 * one length byte (in 16-byte units) then that many bytes of text, usually
 * "StreamTitle=" followed by a quoted string. It is how the programme or
 * track name reaches the screen, and it is the only source for it - the
 * directory knows the station, never what is on it right now.
 *
 * ⛔ THIS RUNS IN THE READER, NOT THE DECODER. The ring must carry audio and
 * nothing else, or the decoder has to resync over every title the server
 * sends. */
static int s_metaint = 0;          /* 0 = the server sends none */
static int s_until_meta = 0;
static int s_meta_need = 0;        /* >0 while a block is being assembled */
static int s_meta_have = 0;
static char *s_meta;               /* 4096 B, PSRAM - a block caps at 4080 */

static void icy_take_title(const char *block)
{
    const char *p = strstr(block, "StreamTitle=");
    if (!p) return;
    p += 12;
    char q = *p;
    if (q != '\'' && q != '"') return;
    p++;
    const char *e = strchr(p, q);
    if (!e) return;
    int n = (int)(e - p);
    if (n < 0) return;
    if (n >= (int)sizeof(s_title)) n = (int)sizeof(s_title) - 1;
    /* Whole write, never a partial one - the repaint has no lock. */
    char tmp[sizeof(s_title)];
    memcpy(tmp, p, (size_t)n);
    tmp[n] = '\0';
    snprintf(s_title, sizeof(s_title), "%s", tmp);
    ESP_LOGI(TAG, "ICY title: '%s'", s_title);
}

/* ---- resampling ---------------------------------------------------------
 *
 * ⛔ THE CHANNEL IS 48 kHz AND CANNOT BE REOPENED (webradio.h says why), so a
 * 44.1 kHz stream has to be converted here. Linear interpolation between
 * neighbouring samples.
 *
 * ⭐ THE PHASE CARRIES A SAMPLE, NOT JUST A FRACTION. The first cut ended
 * each block with `s_phase -= src_frames; if (s_phase < 0) s_phase = 0;` -
 * and after a 1152-sample block at 44.1 kHz the true phase IS negative
 * (-0.8), because the next output sample falls BETWEEN the last input sample
 * of this block and the first of the next. Clamping it to zero threw that
 * away and restarted at src[0]: a slip of up to one sample 38 times a second,
 * on top of a 0.07% rate error. So the last input frame is carried in s_tail
 * and index -1 means "that sample".
 *
 * ⚠ IT IS LINEAR, and that is all it is. There is no anti-imaging filter, so
 * this adds high-frequency junk a proper resampler would not. For a speaker on
 * a workbench playing a 128 kbps stream that is inaudible against the codec's
 * own artefacts; it is written down because somebody will one day wonder why
 * this is not a polyphase filter, and the answer is cost, not oversight.
 *
 * Returns frames written to `dst`. */
static float   s_phase = 0.0f;
static int16_t s_tail[2] = { 0, 0 };

static int resample_to_channel(const int16_t *src, int src_frames, int src_rate,
                               int16_t *dst, int dst_cap_frames)
{
    if (src_frames <= 0) return 0;
    if (src_rate == DSP_SAMPLE_RATE_HZ) {
        int n = src_frames < dst_cap_frames ? src_frames : dst_cap_frames;
        memcpy(dst, src, (size_t)n * 2 * sizeof(int16_t));
        /* Keep the carry current even on the pass-through, or a stream that
         * changes rate mid-play interpolates from a sample that is minutes
         * old. One click, but it costs two stores to not have it. */
        s_tail[0] = src[(src_frames - 1) * 2];
        s_tail[1] = src[(src_frames - 1) * 2 + 1];
        s_phase = 0.0f;
        return n;
    }
    const float step = (float)src_rate / (float)DSP_SAMPLE_RATE_HZ;
    int o = 0;
    while (o < dst_cap_frames) {
        /* s_phase lives in [-1, src_frames): -1 addresses the carried frame. */
        int i = (s_phase < 0.0f) ? -1 : (int)s_phase;
        if (i >= src_frames - 1) break;
        float f = s_phase - (float)i;
        for (int c = 0; c < 2; c++) {
            int a = (i < 0) ? s_tail[c] : src[i * 2 + c];
            int b = src[(i + 1) * 2 + c];
            dst[o * 2 + c] = (int16_t)(a + (int)((b - a) * f));
        }
        o++;
        s_phase += step;
    }
    s_phase -= (float)src_frames;
    s_tail[0] = src[(src_frames - 1) * 2];
    s_tail[1] = src[(src_frames - 1) * 2 + 1];
    return o;
}

static void set_err(const char *msg)
{
    snprintf(s_err, sizeof(s_err), "%s", msg ? msg : "");
    s_state = WEBRADIO_ERROR;
    ESP_LOGW(TAG, "%s", s_err);
}

/* =========================================================================
 * THE READER. Socket -> ICY de-interleave -> ring. Never touches the codec.
 * ========================================================================= */

/* Push audio into the ring, splitting out ICY blocks as they arrive. `len` is
 * raw body bytes. A block that straddles two reads is assembled in s_meta. */
static void feed_ring(const uint8_t *d, int len)
{
    while (len > 0 && !s_stop_req) {
        if (s_meta_need > 0) {                      /* mid-block */
            int n = s_meta_need - s_meta_have;
            if (n > len) n = len;
            memcpy(s_meta + s_meta_have, d, (size_t)n);
            s_meta_have += n; d += n; len -= n;
            if (s_meta_have == s_meta_need) {
                s_meta[s_meta_need] = '\0';
                icy_take_title(s_meta);
                s_meta_need = s_meta_have = 0;
                s_until_meta = s_metaint;
            }
            continue;
        }
        if (s_metaint > 0 && s_until_meta == 0) {    /* the length byte */
            int mlen = (int)(*d) * 16;
            d++; len--;
            if (mlen > 0) { s_meta_need = mlen; s_meta_have = 0; }
            else          { s_until_meta = s_metaint; }
            continue;
        }
        int chunk = len;
        if (s_metaint > 0 && chunk > s_until_meta) chunk = s_until_meta;
        /* A short blocking window so a stop is noticed promptly. The ring is
         * 4 s deep, so a full ring means the decoder has stopped taking, not
         * that the reader is early. */
        size_t w = xStreamBufferSend(s_ring, d, (size_t)chunk,
                                     pdMS_TO_TICKS(100));
        d += w; len -= (int)w;
        if (s_metaint > 0) s_until_meta -= (int)w;
        if (w == 0) {
            if (s_stop_req) break;
            ESP_LOGW(TAG, "ring full - dropping %d B", chunk);
            break;
        }
    }
}

static void webradio_net_task(void *arg)
{
    (void)arg;
    esp_http_client_handle_t cl = NULL;

    s_metaint = s_until_meta = 0;
    s_meta_need = s_meta_have = 0;

    esp_http_client_config_t hc = {
        .url                         = s_url,
        .timeout_ms                  = 10000,
        .buffer_size                 = NET_CHUNK,
        /* Same trap as radio_browser.c: a NULL bundle is not "no verification",
         * it is a client esp-tls refuses to create. Most Icecast streams are
         * plain http and never reach this, but url_resolved from the directory
         * is https often enough to matter. */
        .crt_bundle_attach           = esp_crt_bundle_attach,
    };
    cl = esp_http_client_init(&hc);
    if (!cl) { set_err("Could not start the stream"); goto done; }

    /* Ask for the metadata. A server that does not do ICY simply omits the
     * header and s_metaint stays 0, which the reader treats as "all audio". */
    esp_http_client_set_header(cl, "Icy-MetaData", "1");
    esp_http_client_set_header(cl, "User-Agent", "qmx-panadapter/1.0");

    /* ⛔ REDIRECTS ARE OURS TO FOLLOW HERE. esp_http_client only chases them
     * inside _perform(); this opens the connection by hand so it can stream,
     * and a 3xx therefore arrives raw. Station directories are full of them -
     * the first station tried on the bench, SR P4 Malmohus, answered 302 and
     * the player reported "Station refused us (HTTP 302)" (2026-11-01).
     *
     * Bounded at four hops: a redirect loop must not spin a task that owns
     * the speaker. */
    int status = 0;
    for (int hop = 0; hop < 4; hop++) {
        if (esp_http_client_open(cl, 0) != ESP_OK) { set_err("Station did not answer"); goto done; }
        (void)esp_http_client_fetch_headers(cl);
        status = esp_http_client_get_status_code(cl);
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
            break;
        char *loc = NULL;
        if (esp_http_client_get_header(cl, "Location", &loc) != ESP_OK || !loc || !loc[0]) {
            set_err("Station redirected us nowhere");
            goto done;
        }
        ESP_LOGI(TAG, "redirect %d -> %s", status, loc);
        /* Copy before close: the header buffer belongs to the client. */
        char next[sizeof(s_url)];
        snprintf(next, sizeof(next), "%s", loc);
        esp_http_client_close(cl);
        if (esp_http_client_set_url(cl, next) != ESP_OK) {
            set_err("That redirect is not a URL we can follow");
            goto done;
        }
        /* ⚠ The header does not survive a redirect - ask again, or the new
         * host sends no ICY and the title pane stays empty for that station
         * only, which reads as a station fault. */
        esp_http_client_set_header(cl, "Icy-MetaData", "1");
    }
    if (status != 200) {
        char m[64];
        snprintf(m, sizeof(m), "Station refused us (HTTP %d)", status);
        set_err(m);
        goto done;
    }
    {
        char *v = NULL;
        if (esp_http_client_get_header(cl, "icy-metaint", &v) == ESP_OK && v) {
            s_metaint = atoi(v);
            s_until_meta = s_metaint;
            ESP_LOGI(TAG, "ICY metaint = %d", s_metaint);
        }
    }

    {
        uint8_t *buf = heap_caps_malloc(NET_CHUNK, MALLOC_CAP_SPIRAM);
        if (!buf) { set_err("No PSRAM for the reader"); goto done; }
        while (!s_stop_req) {
            int n = esp_http_client_read(cl, (char *)buf, NET_CHUNK);
            if (n < 0) { set_err("The stream dropped"); break; }
            if (n == 0) { if (!s_stop_req) set_err("The stream ended"); break; }
            feed_ring(buf, n);
        }
        heap_caps_free(buf);
    }

done:
    if (cl) { esp_http_client_close(cl); esp_http_client_cleanup(cl); }
    s_net = NULL;
    s_net_done = true;
    ESP_LOGI(TAG, "reader exit");
    vTaskDelete(NULL);
}

/* =========================================================================
 * THE DECODER. Ring -> Helix -> resample -> codec. Never touches the socket.
 * ========================================================================= */

/* ⭐ ONE LINE A SECOND, AND IT IS THE INSTRUMENT THIS FILE WAS MISSING.
 *
 * The first version's break-up was invisible to every counter on the device:
 * cpu_stats reads per-core IDLE, and a task blocked on a socket is not busy,
 * so idle1 while playing could not be told from idle1 while stopped. What
 * starvation actually looks like is `frames` below falling short of
 * DSP_SAMPLE_RATE_HZ and `ring` sitting near zero. Both are free to collect:
 * four esp_timer reads and a counter per frame. */
typedef struct {
    int64_t  t_ring, t_dec, t_wr;      /* us spent in each stage             */
    uint32_t frames;                   /* PCM frames handed to the codec     */
    uint32_t ring_min, ring_max;       /* bytes held in the jitter buffer    */
    uint64_t ring_sum; uint32_t ring_n;
    uint32_t resync, underflow;
} dec_stats_t;

static void stats_report(dec_stats_t *s, int64_t window_us)
{
    if (window_us <= 0) return;
    unsigned want = (unsigned)((uint64_t)DSP_SAMPLE_RATE_HZ * (uint64_t)window_us / 1000000ULL);
    unsigned pct  = want ? (unsigned)((uint64_t)s->frames * 100 / want) : 0;
    ESP_LOGI(TAG, "loop: ring %u/%u/%u B of %d | frames %u of %u (%u%%) | "
                  "ms ring %d dec %d write %d | resync %u underflow %u",
             (unsigned)(s->ring_n ? s->ring_min : 0),
             (unsigned)(s->ring_n ? s->ring_sum / s->ring_n : 0),
             (unsigned)s->ring_max, RING_BYTES,
             (unsigned)s->frames, want, pct,
             (int)(s->t_ring / 1000), (int)(s->t_dec / 1000), (int)(s->t_wr / 1000),
             (unsigned)s->resync, (unsigned)s->underflow);
    memset(s, 0, sizeof(*s));
    s->ring_min = 0xffffffffu;
}

static void webradio_task(void *arg)
{
    (void)arg;

    HMP3Decoder dec = MP3InitDecoder();
    bool claimed = false;

    if (!dec) { set_err("No memory for the MP3 decoder"); goto done; }

    /* ⛔ WHERE HELIX'S OWN STATE LANDED, AND WHY IT IS WORTH A LOG LINE.
     *
     * MEASURED on field 2026-11-01: MP3Decode burns 95.8% of core 1 and needs
     * 30.6 ms of CPU per 24 ms frame - 1.28x too slow in real time, which is
     * the 75% of samples the operator hears as break-up. The same library on
     * the same chip keeps up in dmatking/esp32-p4-webradio, and our build is
     * the MORE optimised one (-O2 against its -Og), with identical CPU, PSRAM
     * and flash settings. So the difference is not the library.
     *
     * AllocateBuffers() makes EIGHT PLAIN mallocs, all under
     * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384, so every one lands in
     * "internal" RAM - a lottery on this board that includes LP SRAM at 2.5x
     * slower than cached PSRAM (T133). HuffmanInfo, IMDCTInfo and SubbandInfo
     * are the per-frame hot state. The reference wins that lottery for free:
     * 4.5 kLOC of firmware and a roomy L2 heap, where ours runs ~30 KB free.
     *
     * ⭐ NO STANDING PATCH IS NEEDED TO SEE THIS. MP3DecInfo is a PUBLIC
     * struct and it holds all eight pointers, so the addresses can be read
     * straight off the handle. Only moving them would need to touch
     * buffers.c, and that is not worth doing until these lines say it is. */
    {
        MP3DecInfo *di = (MP3DecInfo *)dec;
        ESP_LOGW(TAG, "helix: dec %p %s | huff %p %s | imdct %p %s | subband %p %s",
                 (void *)di, mem_where(di),
                 di->HuffmanInfoPS, mem_where(di->HuffmanInfoPS),
                 di->IMDCTInfoPS,   mem_where(di->IMDCTInfoPS),
                 di->SubbandInfoPS, mem_where(di->SubbandInfoPS));
        ESP_LOGW(TAG, "helix: fh %p %s | si %p %s | sfi %p %s | dq %p %s | in %s pcm %s out %s",
                 di->FrameHeaderPS,     mem_where(di->FrameHeaderPS),
                 di->SideInfoPS,        mem_where(di->SideInfoPS),
                 di->ScaleFactorInfoPS, mem_where(di->ScaleFactorInfoPS),
                 di->DequantInfoPS,     mem_where(di->DequantInfoPS),
                 mem_where(s_in), mem_where(s_pcm), mem_where(s_out));
    }

    s_state = WEBRADIO_CONNECTING;
    s_title[0] = '\0';
    s_phase = 0.0f;
    s_tail[0] = s_tail[1] = 0;

    /* ⭐ PREBUFFER BEFORE THE FIRST SAMPLE. The reference gets its cushion
     * for free because Icecast bursts on connect, and relying on that is
     * relying on somebody else's server setting. Half the ring is 2 s, and it
     * is the difference between a stream that survives a WiFi hiccup and one
     * that does not. Bounded at 8 s, so a station that connects and then
     * dribbles fails rather than hanging on this line. */
    {
        int64_t t0 = esp_timer_get_time();
        while (!s_stop_req && !s_net_done &&
               xStreamBufferBytesAvailable(s_ring) < RING_BYTES / 2) {
            if (esp_timer_get_time() - t0 > 8000000) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_stop_req || s_state == WEBRADIO_ERROR) goto done;
        ESP_LOGI(TAG, "prebuffered %u B in %d ms",
                 (unsigned)xStreamBufferBytesAvailable(s_ring),
                 (int)((esp_timer_get_time() - t0) / 1000));
    }

    if (!rx_audio_aux_claim(true)) { set_err("The audio output is not open"); goto done; }
    claimed = true;

    {
    int have = 0;                   /* bytes of MP3 held in s_in */
    int reported_rate = 0;
    dec_stats_t st; memset(&st, 0, sizeof(st)); st.ring_min = 0xffffffffu;
    int64_t win_t0 = esp_timer_get_time();

    s_state = WEBRADIO_PLAYING;
    ESP_LOGI(TAG, "playing '%s'", s_name);

    while (!s_stop_req) {
        /* ⛔ THE REPORT GOES AT THE TOP, NOT AFTER THE WRITE. Put it after a
         * successful decode and it falls silent exactly when the decoder is
         * starving - and a silent instrument reads like a healthy quiet
         * system. A window with frames 0 is the most informative line this
         * loop can print. */
        int64_t now = esp_timer_get_time();
        if (now - win_t0 >= 1000000) { stats_report(&st, now - win_t0); win_t0 = now; }

        /* Top the decoder's own buffer up from the RING, never the socket.
         * This is the whole fix: the wait below is a 100 ms ring timeout with
         * 4 s of audio behind it, not a 768 ms socket read with 80 ms of DMA
         * behind it. */
        if (have < IN_BYTES) {
            uint32_t lvl = (uint32_t)xStreamBufferBytesAvailable(s_ring);
            if (lvl < st.ring_min) st.ring_min = lvl;
            if (lvl > st.ring_max) st.ring_max = lvl;
            st.ring_sum += lvl; st.ring_n++;

            int64_t t0 = esp_timer_get_time();
            int got = (int)xStreamBufferReceive(s_ring, s_in + have,
                                                (size_t)(IN_BYTES - have),
                                                pdMS_TO_TICKS(100));
            st.t_ring += esp_timer_get_time() - t0;
            have += got;
        }

        if (have < 128) {
            /* The reader is the authority on whether more is coming. */
            if (s_net_done && xStreamBufferBytesAvailable(s_ring) == 0) break;
            continue;
        }

        int off = MP3FindSyncWord(s_in, have);
        if (off < 0) { have = 0; st.resync++; continue; }
        if (off > 0) { memmove(s_in, s_in + off, (size_t)(have - off)); have -= off; }

        uint8_t *p = s_in;
        int left = have;
        int64_t t0 = esp_timer_get_time();
        int rc = MP3Decode(dec, &p, &left, s_pcm, 0);
        st.t_dec += esp_timer_get_time() - t0;

        if (rc != ERR_MP3_NONE) {
            if (rc == ERR_MP3_INDATA_UNDERFLOW || rc == ERR_MP3_MAINDATA_UNDERFLOW) {
                /* Not an error - the frame is simply not all here yet. Keep
                 * it and read more. If the buffer is already full and still
                 * short, the sync word was false: step past it. */
                st.underflow++;
                if (have >= IN_BYTES) {
                    memmove(s_in, s_in + 1, (size_t)(have - 1));
                    have--;
                }
                continue;
            }
            /* A corrupt frame is normal on a lossy link. Step one byte past
             * this sync word and look for the next. */
            memmove(s_in, s_in + 1, (size_t)(have - 1));
            have--;
            st.resync++;
            continue;
        }
        /* Consume exactly what Helix used. */
        memmove(s_in, p, (size_t)left);
        have = left;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (fi.outputSamps <= 0 || fi.samprate <= 0 ||
            (fi.nChans != 1 && fi.nChans != 2)) continue;
        if (fi.samprate != reported_rate) {
            reported_rate = fi.samprate;
            ESP_LOGI(TAG, "stream: %d Hz, %d ch, %d kbps", fi.samprate, fi.nChans,
                     fi.bitrate / 1000);
        }

        /* Helix gives MONO as one channel; the output is stereo, so a mono
         * stream is duplicated rather than played at half rate. */
        int frames = (fi.nChans == 2) ? fi.outputSamps / 2 : fi.outputSamps;
        if (fi.nChans == 1) {
            for (int i = frames - 1; i >= 0; i--) {
                int16_t v = s_pcm[i];
                s_pcm[i * 2] = v;
                s_pcm[i * 2 + 1] = v;
            }
        }

        int wrote = resample_to_channel(s_pcm, frames, fi.samprate, s_out, OUT_FRAMES);
        if (wrote > 0) {
            t0 = esp_timer_get_time();
            rx_audio_aux_write(s_out, wrote);
            st.t_wr += esp_timer_get_time() - t0;
            st.frames += (uint32_t)wrote;
        }
    }
    }

done:
    if (claimed) rx_audio_aux_claim(false);
    if (dec) MP3FreeDecoder(dec);
    if (s_state != WEBRADIO_ERROR) s_state = WEBRADIO_STOPPED;
    s_title[0] = '\0';
    s_task = NULL;
    ESP_LOGI(TAG, "task exit (state=%d)", (int)s_state);
    vTaskDelete(NULL);
}

static bool buffers_ready(void)
{
    if (!s_in)   s_in   = heap_caps_malloc(IN_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_pcm)  s_pcm  = heap_caps_malloc(PCM_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_out)  s_out  = heap_caps_malloc(OUT_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_meta) s_meta = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    /* ⛔ WithCaps, or the 64 KB ring lands in internal RAM - see the buffer
     * note above. */
    if (!s_ring) s_ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MALLOC_CAP_SPIRAM);
    return s_in && s_pcm && s_out && s_meta && s_ring;
}

bool webradio_play(const char *url, const char *name)
{
    if (!url || !url[0]) return false;
    webradio_stop();

    if (!buffers_ready()) { set_err("No PSRAM for the player"); return false; }
    xStreamBufferReset(s_ring);

    snprintf(s_url, sizeof(s_url), "%s", url);
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
    s_err[0] = '\0';
    s_stop_req = false;
    s_paused = false;
    s_net_done = false;

    /* ⛔ CORE 1 for both. Core 0 is the wall on this board - see webradio.h.
     *
     * The reader runs BELOW the decoder: if both are ready at once the
     * speaker comes first, and the reader has 4 s of ring to catch up in.
     * Stacks are generous and in PSRAM, where 12 MB is free - the lesson paid
     * for twice on the web task stacks. */
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        webradio_net_task, "webradio_net", 8192, NULL,
        tskIDLE_PRIORITY + 2, &s_net, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        s_net = NULL; s_net_done = true;
        set_err("Could not start the reader task");
        return false;
    }

    rc = xTaskCreatePinnedToCoreWithCaps(
        webradio_task, "webradio", 12288, NULL,
        tskIDLE_PRIORITY + 3, &s_task, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        s_task = NULL;
        s_stop_req = true;              /* take the reader down with it */
        set_err("Could not start the player task");
        return false;
    }
    return true;
}

void webradio_stop(void)
{
    if (!s_task && s_net_done) { s_state = WEBRADIO_STOPPED; return; }
    s_stop_req = true;
    /* Both tasks close down and delete themselves. A blocking read can hold
     * the reader for up to the HTTP timeout, so this waits rather than
     * assuming - returning early would let a second play() start while the
     * first still owned the output.
     *
     * ⛔ THE RING IS RESET ONLY ONCE THE READER IS GONE.
     * xStreamBufferReset() fails while a task is blocked on a send, and a
     * lingering producer would collide with the next reader. */
    for (int i = 0; i < 240 && (s_task || !s_net_done); i++)
        vTaskDelay(pdMS_TO_TICKS(50));
    if (s_task || !s_net_done) ESP_LOGW(TAG, "player did not exit in 12 s");
    else if (s_ring) xStreamBufferReset(s_ring);
    s_state = WEBRADIO_STOPPED;
}

void webradio_pause(void)
{
    if (s_state != WEBRADIO_PLAYING && s_state != WEBRADIO_CONNECTING) return;
    webradio_stop();
    s_paused = true;
}

void webradio_resume(void)
{
    if (!s_paused || !s_url[0]) return;
    s_paused = false;
    webradio_play(s_url, s_name);
}

bool webradio_is_paused(void) { return s_paused; }
webradio_state_t webradio_state(void) { return s_state; }

void webradio_get_name(char *o, int c)  { if (o && c) snprintf(o, (size_t)c, "%s", s_name); }
void webradio_get_title(char *o, int c) { if (o && c) snprintf(o, (size_t)c, "%s", s_title); }
void webradio_get_error(char *o, int c) { if (o && c) snprintf(o, (size_t)c, "%s", s_err); }

/* One codec, one knob: the player and receive audio share the volume, because
 * they share the output. Keeping a second number would mean the speaker jumped
 * in level depending on which of them last wrote. */
void webradio_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    rx_audio_set_volume((uint8_t)v);
}

int webradio_get_volume(void) { return (int)rx_audio_get_volume(); }
