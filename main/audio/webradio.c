// Web-radio player. See webradio.h for the ownership rules and why this
// borrows the speaker instead of opening a channel.
//
// Shape: ONE task does fetch, decode and write. A separate reader task feeding
// a ring buffer is the usual arrangement and it was deliberately not used -
// the codec write blocks and paces the loop for free, so a second task would
// add a buffer, a semaphore and a second stack to solve a problem this does
// not have. If a stream ever stutters on a busy band, that is the first thing
// to revisit, and the measurement to take first is where the task is blocked.
#include "webradio.h"

#include "rx_audio.h"
#include "dsp/dsp.h"              /* DSP_SAMPLE_RATE_HZ - the channel's rate   */

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mp3dec.h"               /* chmorgan/esp-libhelix-mp3 */

#include <stdlib.h>
#include <string.h>

static const char *TAG = "webradio";

/* ---- state, written by the task and read by LVGL ------------------------
 *
 * Plain variables behind no lock, and that is a decision rather than an
 * oversight: every reader is a 1 Hz repaint that can tolerate a torn string
 * for one frame, and the alternative - a mutex taken on the audio path - buys
 * a repaint nothing can see at the cost of a lock in the decode loop. The
 * strings are only ever written whole, by one writer. */
static volatile webradio_state_t s_state  = WEBRADIO_STOPPED;
static volatile bool             s_paused = false;
static volatile bool             s_stop_req = false;
static TaskHandle_t              s_task   = NULL;

static char s_url[192]  = "";
static char s_name[48]  = "";
static char s_title[96] = "";
static char s_err[80]   = "";

/* ---- buffers ------------------------------------------------------------
 *
 * ⛔ ALL IN PSRAM. Internal RAM on this board runs within ~30 KB of empty
 * (#65) and none of this is DMA. The input buffer holds a couple of MP3
 * frames plus slack; the PCM buffer holds one decoded frame (1152 samples
 * per channel at most) and its resampled copy. */
#define IN_BYTES    (16 * 1024)
#define PCM_SAMPLES (1152 * 2)                 /* one frame, stereo, interleaved */
#define OUT_FRAMES  (PCM_SAMPLES)              /* 48/44.1 grows it - see resample */

static uint8_t *s_in;
static int16_t *s_pcm;
static int16_t *s_out;

/* ---- ICY metadata -------------------------------------------------------
 *
 * The server interleaves a metadata block every `icy-metaint` bytes of audio:
 * one length byte (in 16-byte units) then that many bytes of text, usually
 * "StreamTitle='...';". It is how the programme or track name reaches the
 * screen, and it is the only source for it - the directory knows the station,
 * never what is on it right now. */
static int s_metaint = 0;          /* 0 = the server sends none */
static int s_until_meta = 0;

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
 * neighbouring frames, with the phase carried across calls so there is no
 * click at the frame boundary.
 *
 * ⚠ IT IS LINEAR, and that is all it is. There is no anti-imaging filter, so
 * this adds high-frequency junk a proper resampler would not. For a speaker on
 * a workbench playing a 128 kbps stream that is inaudible against the codec's
 * own artefacts; it is written down because somebody will one day wonder why
 * this is not a polyphase filter, and the answer is cost, not oversight.
 *
 * Returns frames written to `dst`. */
static float s_phase = 0.0f;

static int resample_to_channel(const int16_t *src, int src_frames, int src_rate,
                               int16_t *dst, int dst_cap_frames)
{
    if (src_rate == DSP_SAMPLE_RATE_HZ) {
        int n = src_frames < dst_cap_frames ? src_frames : dst_cap_frames;
        memcpy(dst, src, (size_t)n * 2 * sizeof(int16_t));
        return n;
    }
    const float step = (float)src_rate / (float)DSP_SAMPLE_RATE_HZ;
    int o = 0;
    while (o < dst_cap_frames) {
        int i = (int)s_phase;
        if (i >= src_frames - 1) break;
        float f = s_phase - (float)i;
        for (int c = 0; c < 2; c++) {
            int a = src[i * 2 + c], b = src[(i + 1) * 2 + c];
            dst[o * 2 + c] = (int16_t)(a + (int)((b - a) * f));
        }
        o++;
        s_phase += step;
    }
    /* Carry the fraction into the next frame rather than resetting it. */
    s_phase -= (float)src_frames;
    if (s_phase < 0.0f) s_phase = 0.0f;
    return o;
}

static void set_err(const char *msg)
{
    snprintf(s_err, sizeof(s_err), "%s", msg ? msg : "");
    s_state = WEBRADIO_ERROR;
    ESP_LOGW(TAG, "%s", s_err);
}

/* Read exactly `want` bytes unless the stream ends. Returns what it got. */
static int read_full(esp_http_client_handle_t cl, uint8_t *dst, int want)
{
    int got = 0;
    while (got < want && !s_stop_req) {
        int n = esp_http_client_read(cl, (char *)dst + got, want - got);
        if (n <= 0) break;
        got += n;
    }
    return got;
}

/* Pull `want` bytes of AUDIO, stepping over any ICY metadata block that falls
 * inside the run. Returns audio bytes delivered. */
static int read_audio(esp_http_client_handle_t cl, uint8_t *dst, int want)
{
    if (s_metaint <= 0) return read_full(cl, dst, want);

    int done = 0;
    while (done < want && !s_stop_req) {
        if (s_until_meta == 0) {
            uint8_t lenb = 0;
            if (read_full(cl, &lenb, 1) != 1) break;
            int mlen = (int)lenb * 16;
            if (mlen > 0) {
                /* Bounded and on the stack: the protocol caps a block at
                 * 255*16 = 4080 bytes, and this task has a generous stack. */
                char meta[4096];
                int n = read_full(cl, (uint8_t *)meta, mlen);
                if (n != mlen) break;
                meta[n] = '\0';
                icy_take_title(meta);
            }
            s_until_meta = s_metaint;
            continue;
        }
        int chunk = want - done;
        if (chunk > s_until_meta) chunk = s_until_meta;
        int n = read_full(cl, dst + done, chunk);
        if (n <= 0) break;
        done += n;
        s_until_meta -= n;
    }
    return done;
}

static void webradio_task(void *arg)
{
    (void)arg;

    HMP3Decoder dec = MP3InitDecoder();
    esp_http_client_handle_t cl = NULL;
    bool claimed = false;

    if (!dec) { set_err("No memory for the MP3 decoder"); goto done; }

    s_state = WEBRADIO_CONNECTING;
    s_title[0] = '\0';
    s_metaint = 0;
    s_until_meta = 0;
    s_phase = 0.0f;

    esp_http_client_config_t hc = {
        .url                         = s_url,
        .timeout_ms                  = 10000,
        .buffer_size                 = 4096,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach           = NULL,
    };
    cl = esp_http_client_init(&hc);
    if (!cl) { set_err("Could not start the stream"); goto done; }

    /* Ask for the metadata. A server that does not do ICY simply omits the
     * header and s_metaint stays 0, which the reader treats as "all audio". */
    esp_http_client_set_header(cl, "Icy-MetaData", "1");
    esp_http_client_set_header(cl, "User-Agent", "qmx-panadapter/1.0");

    if (esp_http_client_open(cl, 0) != ESP_OK) { set_err("Station did not answer"); goto done; }
    int64_t hdr = esp_http_client_fetch_headers(cl);
    (void)hdr;
    int status = esp_http_client_get_status_code(cl);
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

    if (!rx_audio_aux_claim(true)) { set_err("The audio output is not open"); goto done; }
    claimed = true;

    int have = 0;                   /* bytes of MP3 held in s_in */
    int reported_rate = 0;
    s_state = WEBRADIO_PLAYING;
    ESP_LOGI(TAG, "playing '%s'", s_name);

    while (!s_stop_req) {
        /* Top the buffer up. The codec write below is what paces this loop. */
        if (have < IN_BYTES / 2) {
            int n = read_audio(cl, s_in + have, IN_BYTES - have);
            if (n <= 0 && have == 0) { set_err("The stream ended"); break; }
            have += n;
        }

        int off = MP3FindSyncWord(s_in, have);
        if (off < 0) { have = 0; continue; }     /* nothing usable - refill */
        if (off > 0) { memmove(s_in, s_in + off, (size_t)(have - off)); have -= off; }

        uint8_t *p = s_in;
        int left = have;
        int rc = MP3Decode(dec, &p, &left, s_pcm, 0);
        if (rc != ERR_MP3_NONE) {
            if (rc == ERR_MP3_INDATA_UNDERFLOW || rc == ERR_MP3_MAINDATA_UNDERFLOW) {
                /* Not an error: the frame is simply not all here yet. Keep
                 * what we have and read more. */
                memmove(s_in, p, (size_t)left);
                have = left;
                continue;
            }
            /* A corrupt frame mid-stream is normal on a lossy link. Skip one
             * byte past the sync word and look for the next one rather than
             * tearing the connection down. */
            memmove(s_in, s_in + 1, (size_t)(have - 1));
            have--;
            continue;
        }
        memmove(s_in, p, (size_t)left);
        have = left;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (fi.samprate != reported_rate) {
            reported_rate = fi.samprate;
            ESP_LOGI(TAG, "stream: %d Hz, %d ch, %d kbps", fi.samprate, fi.nChans,
                     fi.bitrate / 1000);
        }
        if (fi.outputSamps <= 0) continue;

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
        if (wrote > 0) rx_audio_aux_write(s_out, wrote);
    }

done:
    if (claimed) rx_audio_aux_claim(false);
    if (cl) { esp_http_client_close(cl); esp_http_client_cleanup(cl); }
    if (dec) MP3FreeDecoder(dec);
    if (s_state != WEBRADIO_ERROR) s_state = WEBRADIO_STOPPED;
    s_title[0] = '\0';
    s_task = NULL;
    ESP_LOGI(TAG, "task exit (state=%d)", (int)s_state);
    vTaskDelete(NULL);
}

static bool buffers_ready(void)
{
    if (!s_in)  s_in  = heap_caps_malloc(IN_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_pcm) s_pcm = heap_caps_malloc(PCM_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_out) s_out = heap_caps_malloc(OUT_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    return s_in && s_pcm && s_out;
}

bool webradio_play(const char *url, const char *name)
{
    if (!url || !url[0]) return false;
    webradio_stop();

    if (!buffers_ready()) { set_err("No PSRAM for the player"); return false; }

    snprintf(s_url, sizeof(s_url), "%s", url);
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
    s_err[0] = '\0';
    s_stop_req = false;
    s_paused = false;

    /* ⛔ CORE 1. Core 0 is the wall on this board - see webradio.h. 8 KB covers
     * the ICY metadata block (4 KB) plus the HTTP client's frames. */
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        webradio_task, "webradio", 12288, NULL,
        tskIDLE_PRIORITY + 3, &s_task, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        s_task = NULL;
        set_err("Could not start the player task");
        return false;
    }
    return true;
}

void webradio_stop(void)
{
    if (!s_task) { s_state = WEBRADIO_STOPPED; return; }
    s_stop_req = true;
    /* The task closes the socket, returns the speaker and deletes itself. A
     * blocking read can hold it for up to the HTTP timeout, so this waits
     * rather than assuming - returning early would let a second play() start
     * while the first still owned the output. */
    for (int i = 0; i < 120 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_task) ESP_LOGW(TAG, "player task did not exit in 6 s");
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
