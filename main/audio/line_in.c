/* line_in.c - the 3.5 mm jack as a receive source. See line_in.h for what the
 * hardware can and cannot do; this file is how.
 *
 * THE CHAIN
 *   ES7210, 4 channels @ 48 kHz, 16-bit  ->  take TDM slot 3 (the jack)
 *     -> Hilbert transform, 127 taps      ->  Q
 *     -> matched 63-sample delay          ->  I
 *     -> audio_push_pairs(I,Q)            ->  the same ring the QMX feeds
 *
 *   Everything downstream is then unchanged. That is the whole point of doing
 *   the quadrature synthesis here rather than teaching the FFT, the waterfall,
 *   zoom and the S-meter about a second kind of input.
 *
 * ⛔ THE DECODERS DO NOT USE THE ANALYTIC SIGNAL. dsp.c's FT8/JS8/WSPR capture
 *   branch takes the I channel - which is the original audio, just delayed -
 *   and skips its fs/4 mixer, because a real source has no +12 kHz IF to move
 *   down. A constant delay is invisible to a decoder that re-syncs every slot.
 *   Running them off the Hilbert output instead would put the filter's weak
 *   low-frequency response directly in the path of 300-1000 Hz FT8 tones.
 */
#include "line_in.h"

#include <math.h>
#include <string.h>

#include "audio.h"
#include "dsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage/settings.h"

#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"
#include "dsps_fir.h"
#include "line_in_hilbert.h"

static const char *TAG = "line_in";

#define LI_CHANS        4       /* the ES7210 is opened with all four; we read slot 3 */
#define LI_JACK_SLOT    3       /* MEASURED by Tony A 2026-10-10 - see line_in.h */
#define LI_RATE_HZ      DSP_SAMPLE_RATE_HZ
#define LI_CHUNK        256     /* frames per read: 5.3 ms, small enough that the
                                 * FIR scratch stays a few KB of INTERNAL RAM */

/* The kernel, the tap count and the group delay live in line_in_hilbert.h so
 * that test/hilbert_harness.c measures THIS code and not a copy. The harness
 * is where the usable band and the image rejection are actually established -
 * read it before changing LI_HILBERT_N. */

#define LI_FIR_DELAY_N  (LI_HILBERT_N + 4)

static esp_codec_dev_handle_t s_mic;
static bool       s_running;
static TaskHandle_t s_task;

/* All scratch is allocated at start(), not declared static, so a board running
 * the QMX pays nothing for it. INTERNAL RAM deliberately: this is the audio
 * hot path at 48 kHz, which is exactly what the internal-RAM audit says must
 * not move to PSRAM. About 5.4 KB in total, against the ~20 KB the USB host
 * is NOT claiming in this mode. */
static int16_t *s_raw;        /* LI_CHUNK * LI_CHANS interleaved from the codec */
static float   *s_in;         /* LI_CHUNK, the jack channel as float */
static float   *s_q;          /* LI_CHUNK, the Hilbert output */
static float   *s_coeffs;     /* LI_HILBERT_N */
static float   *s_delay;      /* LI_FIR_DELAY_N - see the warning above */
static int16_t *s_idelay;     /* LI_HILBERT_MID + 1, the matched delay on I */
static int16_t *s_iq;         /* LI_CHUNK * 2, interleaved for the ring */
static fir_f32_t s_fir;
static uint32_t s_idelay_pos;

/* Level meter state. Peak over the last ~100 ms, published as dBFS. */
static volatile float s_peak_dbfs = -99.0f;
static int32_t  s_peak_acc;
static uint32_t s_peak_frames;

static void free_scratch(void)
{
    heap_caps_free(s_raw);     s_raw = NULL;
    heap_caps_free(s_in);      s_in = NULL;
    heap_caps_free(s_q);       s_q = NULL;
    heap_caps_free(s_coeffs);  s_coeffs = NULL;
    heap_caps_free(s_delay);   s_delay = NULL;
    heap_caps_free(s_idelay);  s_idelay = NULL;
    heap_caps_free(s_iq);      s_iq = NULL;
}

static bool alloc_scratch(void)
{
    const uint32_t cap = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    s_raw    = heap_caps_malloc(LI_CHUNK * LI_CHANS * sizeof(int16_t), cap);
    s_in     = heap_caps_malloc(LI_CHUNK * sizeof(float), cap);
    s_q      = heap_caps_malloc(LI_CHUNK * sizeof(float), cap);
    s_coeffs = heap_caps_malloc(LI_HILBERT_N * sizeof(float), cap);
    s_delay  = heap_caps_malloc(LI_FIR_DELAY_N * sizeof(float), cap);
    s_idelay = heap_caps_calloc(LI_HILBERT_MID + 1, sizeof(int16_t), cap);
    s_iq     = heap_caps_malloc(LI_CHUNK * 2 * sizeof(int16_t), cap);
    if (!s_raw || !s_in || !s_q || !s_coeffs || !s_delay || !s_idelay || !s_iq) {
        ESP_LOGE(TAG, "scratch alloc failed - no line input");
        free_scratch();
        return false;
    }
    return true;
}

/* Roll the running peak into the published dBFS figure every ~100 ms. The
 * peak itself is accumulated in the sample loop; this only decides when to
 * publish and reset. */
static void publish_level(int n)
{
    s_peak_frames += (uint32_t)n;
    if (s_peak_frames >= LI_RATE_HZ / 10) {        /* ~100 ms */
        s_peak_dbfs = (s_peak_acc > 0)
            ? 20.0f * log10f((float)s_peak_acc / 32768.0f)
            : -99.0f;
        s_peak_acc    = 0;
        s_peak_frames = 0;
    }
}

static void line_in_task(void *arg)
{
    (void)arg;
    const size_t bytes = LI_CHUNK * LI_CHANS * sizeof(int16_t);
    uint32_t fail_run = 0;

    while (1) {
        if (esp_codec_dev_read(s_mic, s_raw, bytes) != ESP_OK) {
            /* A read failure here is not a dropped buffer, it is the capture
             * path being gone. Say so once per second rather than per read:
             * at 5.3 ms a chunk, logging every failure IS the fault. */
            if ((fail_run++ % 188) == 0)
                ESP_LOGW(TAG, "codec read failed (%u in a row)", (unsigned)fail_run);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (fail_run) {
            ESP_LOGW(TAG, "codec read recovered after %u failures", (unsigned)fail_run);
            fail_run = 0;
        }

        /* De-interleave the one slot that is wired to the jack. */
        for (int f = 0; f < LI_CHUNK; f++)
            s_in[f] = (float)s_raw[f * LI_CHANS + LI_JACK_SLOT];

        /* Q = Hilbert(x). */
        dsps_fir_f32(&s_fir, s_in, s_q, LI_CHUNK);

        /* I = x delayed by the filter's group delay, so I and Q describe the
         * SAME instant. Getting this wrong does not fail loudly - it degrades
         * the image rejection smoothly, which looks like a weak mirror signal
         * rather than a bug. */
        for (int f = 0; f < LI_CHUNK; f++) {
            int16_t x = (int16_t)s_in[f];
            int16_t i_s = s_idelay[s_idelay_pos];
            s_idelay[s_idelay_pos] = x;
            s_idelay_pos = (s_idelay_pos + 1) % (LI_HILBERT_MID + 1);

            float qf = s_q[f];
            if (qf > 32767.0f) qf = 32767.0f; else if (qf < -32768.0f) qf = -32768.0f;

            s_iq[2 * f]     = i_s;
            s_iq[2 * f + 1] = (int16_t)qf;

            /* Level from the DELAYED I, which is the signal as it reaches the
             * ring - the same samples the operator is setting the gain for.
             * Accumulated here rather than in a second pass: a 256-entry copy
             * on this task's stack to compute one maximum would be the kind of
             * cost that only shows up as a core-0 margin. */
            int32_t a = i_s < 0 ? -(int32_t)i_s : (int32_t)i_s;
            if (a > s_peak_acc) s_peak_acc = a;
        }
        publish_level(LI_CHUNK);

        audio_push_pairs(s_iq, LI_CHUNK);
    }
}

esp_err_t line_in_start(void)
{
    if (s_running) return ESP_OK;

    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "ES7210 init FAILED - no line input");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = LI_CHANS,
        .channel_mask    = 0,
        .sample_rate     = LI_RATE_HZ,
    };
    if (esp_codec_dev_open(s_mic, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 open FAILED - no line input");
        return ESP_FAIL;
    }

    if (!alloc_scratch()) return ESP_ERR_NO_MEM;

    li_build_hilbert(s_coeffs);
    if (dsps_fir_init_f32(&s_fir, s_coeffs, s_delay, LI_HILBERT_N) != ESP_OK) {
        ESP_LOGE(TAG, "Hilbert FIR init failed");
        free_scratch();
        return ESP_FAIL;
    }

    line_in_set_gain_db(settings_get_line_in_gain_db());

    /* ⛔ NOT psram_task_create(). This is the audio producer - it replaces
     * audio_task, which psram_task.h names explicitly as a task that must not
     * take a PSRAM stack. Priority 6 on core 0 is what audio_task uses, and
     * that placement is the #51 fix: below LVGL it loses to a decode-list
     * rebuild and the capture starves. */
    if (xTaskCreatePinnedToCore(line_in_task, "line_in", 6144, NULL, 6,
                                &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "line_in task create failed");
        free_scratch();
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    ESP_LOGW(TAG, "LINE IN active: ES7210 slot %d, %d ch @ %d Hz, gain %u dB, "
                  "Hilbert %d taps (delay %d smp). NO USB host in this mode.",
             LI_JACK_SLOT, LI_CHANS, LI_RATE_HZ,
             (unsigned)settings_get_line_in_gain_db(),
             LI_HILBERT_N, LI_HILBERT_MID);
    return ESP_OK;
}

bool line_in_running(void) { return s_running; }

esp_err_t line_in_set_gain_db(uint8_t db)
{
    if (!s_mic) return ESP_ERR_INVALID_STATE;
    db = line_in_gain_db_normalize(db);
    /* Only the jack's channel. The internal microphones are on slots 0 and 2
     * and nothing reads them in this mode, but winding their gain up with the
     * jack's would be a surprise waiting for whoever implements
     * RX_SOURCE_MIC_INT. */
    int rc = esp_codec_dev_set_in_channel_gain(s_mic, 1u << LI_JACK_SLOT, (float)db);
    if (rc != 0) {
        ESP_LOGW(TAG, "set gain %u dB failed (rc=%d)", (unsigned)db, rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "line-in gain %u dB", (unsigned)db);
    return ESP_OK;
}

float line_in_peak_dbfs(void) { return s_peak_dbfs; }
