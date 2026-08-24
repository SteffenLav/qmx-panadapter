/* WSPR receive slot loop. See wspr_rx.h for what it does and what it does not
 * do (it decodes every other cycle - read that note before "fixing" it). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "dsp.h"
#include "ui_mode.h"
#include "util/psram_task.h"
#include "util/maidenhead.h"
#include "util/dxcc.h"
#include "storage/settings.h"
#include "fft/kiss_fftr.h"
#include "wspr_decode.h"
#include "wspr_sim.h"
#include "wspr_spots.h"
#include "wspr_rx.h"

static const char *TAG = "wspr_rx";

#define WSPR_CYCLE_MS     120000
#define CAP_SAMPLES       ((uint32_t)(120 * (uint32_t)WSPR_SAMPLE_RATE_HZ))  /* 1,440,000 */
#define WSPR_MAX_CANDS    8

/* The audio window sits between 1400 and 1600 Hz for a standard WSPR dial, but
 * the search is widened a little either side: the operator's dial calibration,
 * the QMX's own, and a transmitter's offset all move real signals about, and a
 * candidate found slightly outside the nominal window still decodes. */
#define SEARCH_LO_HZ      1350.0
#define SEARCH_HI_HZ      1650.0

/* ---- per-cycle waterfall ----
 * Built from the captured window (see wspr_rx.h for why a LIVE spectrum is not
 * possible on this page). Allocated on first use and reused: ~36 KB, and the
 * kiss_fftr config for 8192 points is a few hundred KB more, so neither wants
 * to be built 176 times a cycle - let alone once per row. */
static uint8_t  *s_wf;                 /* WSPR_WF_ROWS * WSPR_WF_COLS */
static uint32_t  s_wf_seq;
static SemaphoreHandle_t s_wf_mtx;

static volatile bool s_run;
static TaskHandle_t  s_task;
static char          s_status[48] = "idle";

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

const char *wspr_rx_status(void) { return s_status; }
bool wspr_rx_running(void)       { return s_run; }

/* Same conversion the spot store's other producer does. Distance, bearing and
 * country come from the device's own helpers so this list and the FT8 list
 * cannot disagree. */
static void file_spot(const wspr_decode_result_t *r, int64_t cycle_utc, int snr_db)
{
    wspr_spot_t sp;
    memset(&sp, 0, sizeof(sp));
    snprintf(sp.call, sizeof(sp.call), "%s", r->callsign);
    snprintf(sp.grid, sizeof(sp.grid), "%s", r->grid);
    sp.cycle_utc = cycle_utc;
    sp.freq_hz   = (float)r->freq_hz;
    sp.power_dbm = (int16_t)r->power_dbm;
    sp.snr_db    = (int16_t)snr_db;          /* WSPR_SNR_UNKNOWN until measured */
    sp.drift_hz  = WSPR_DRIFT_UNKNOWN;       /* likewise - 0 Hz is a REAL value */
    sp.km = -1; sp.bearing_deg = -1;

    const char *cty = dxcc_lookup_alpha3(r->callsign);
    if (cty) snprintf(sp.cty, sizeof(sp.cty), "%s", cty);

    qmx_settings_t qs;
    settings_load_all(&qs);
    double mlat, mlon, tlat, tlon;
    if (qs.my_grid[0] && maidenhead_to_latlon(qs.my_grid, &mlat, &mlon) &&
        maidenhead_to_latlon(sp.grid, &tlat, &tlon)) {
        sp.km          = (int32_t)haversine_km(mlat, mlon, tlat, tlon);
        sp.bearing_deg = (int16_t)bearing_deg(mlat, mlon, tlat, tlon);
    }
    wspr_spots_add(&sp);
}

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* One row per symbol period, one column per 1.4648 Hz bin.
 *
 * The bin size is not a choice: an 8192-point FFT at 12 kHz gives exactly
 * WSPR's tone spacing, so a transmission occupies four adjacent columns and
 * reads as a clean vertical trace rather than a smear.
 *
 * Scaled per capture against its own median and peak. WSPR captures have no
 * absolute reference - the RF gain, the band and the time of day all move the
 * floor - so a fixed dB mapping would be black on one band and saturated on the
 * next. The median is the noise floor by construction here, since a WSPR window
 * is mostly noise. */
static void build_waterfall(const int16_t *pcm, long n)
{
    if (!s_wf) {
        s_wf = heap_caps_malloc(WSPR_WF_ROWS * WSPR_WF_COLS,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_wf) { ESP_LOGW(TAG, "no PSRAM for the waterfall"); return; }
    }
    const int NF = 8192;
    kiss_fftr_cfg cfg = kiss_fftr_alloc(NF, 0, NULL, NULL);
    kiss_fft_scalar *in = malloc((size_t)NF * sizeof(kiss_fft_scalar));
    kiss_fft_cpx *sp = malloc((size_t)(NF / 2 + 1) * sizeof(kiss_fft_cpx));
    float *mag = malloc((size_t)WSPR_WF_ROWS * WSPR_WF_COLS * sizeof(float));
    if (!cfg || !in || !sp || !mag) {
        ESP_LOGW(TAG, "waterfall: out of memory");
        free(in); free(sp); free(mag); free(cfg);
        return;
    }

    const double bin_hz = WSPR_SAMPLE_RATE_HZ / NF;      /* 1.46484375 */
    const int lo_bin = (int)(WSPR_WF_LO_HZ / bin_hz + 0.5);

    /* TWO FFTs averaged per displayed row.
     *
     * A single FFT bin's noise power is exponentially distributed - its standard
     * deviation EQUALS its mean, about 5.6 dB once expressed in dB. That is the
     * speckle, and it swamps the signal: a weak WSPR transmission is only ~7 dB
     * above the noise in a 1.4648 Hz bin (-25 dB in the 2500 Hz reference plus
     * 10*log10(2500/1.4648)). Averaging two halves the variance for the cost of
     * halving the time resolution, which at 1.4 s per row is free - a WSPR trace
     * lasts 110 s. */
    for (int r = 0; r < WSPR_WF_ROWS; r++) {
        for (int c = 0; c < WSPR_WF_COLS; c++) mag[r * WSPR_WF_COLS + c] = 0;
        for (int k = 0; k < 2; k++) {
            long off = ((long)r * 2 + k) * (NF / 2);
            for (int i = 0; i < NF; i++)
                in[i] = (off + i < n) ? (kiss_fft_scalar)(pcm[off + i] / 32768.0) : 0;
            kiss_fftr(cfg, in, sp);
            for (int c = 0; c < WSPR_WF_COLS; c++) {
                int b = lo_bin + c;
                float p = (b <= NF / 2) ? sp[b].r * sp[b].r + sp[b].i * sp[b].i : 0;
                mag[r * WSPR_WF_COLS + c] += p * 0.5f;
            }
        }
    }

    /* median of a subsample as the floor. The 99.5th percentile no longer
     * sets the scale (see below) but is still measured and logged, because it
     * is the number that says how loud the interference was this cycle. */
    static float samp[2048];
    int ns = 0;
    for (int i = 0; i < WSPR_WF_ROWS * WSPR_WF_COLS && ns < 2048; i += 17)
        samp[ns++] = mag[i];
    for (int a = 1; a < ns; a++) {          /* insertion sort, ns is small */
        float v = samp[a]; int b = a - 1;
        while (b >= 0 && samp[b] > v) { samp[b + 1] = samp[b]; b--; }
        samp[b + 1] = v;
    }
    float med = ns ? samp[ns / 2] : 1e-12f;
    float top = ns ? samp[(int)(ns * 0.995f)] : med * 100.0f;
    if (med <= 0) med = 1e-12f;
    if (top <= med) top = med * 100.0f;

    /* BLACK IS WELL ABOVE THE FLOOR, not at it - and the TOP IS FIXED.
     *
     * Mapping the median to black showed half the noise, and with ~5.6 dB of
     * per-bin spread that reads as dense speckle with the signal lost in it -
     * which is what the first version looked like on the operator's screen.
     * Hence a black point above the floor rather than at it.
     *
     * The top used to be the capture's own 99.5th percentile, clamped to
     * 20-40 dB, on the reasoning that a loud local station should not clip
     * everything flat. That was exactly backwards, and a screenshot proved it:
     * the scale ended up set by THE LOUDEST THING IN THE WINDOW, which on this
     * band is broadband QRM, not a WSPR signal. With a burst present the
     * percentile pinned hi_db at the 40 dB clamp, and the arithmetic then reads
     *
     *     weak signal  7 dB above floor -> (7-5)/(40-5)  =  6% brightness
     *     solid signal 20 dB            -> (20-5)/(40-5) = 43%
     *
     * i.e. real traces sat near black while the interference took the whole top
     * of the ramp. Observed directly: YU1DGH at 1456.97 Hz decoded fine and was
     * a faint vertical streak, while horizontal QRM at 1573-1630 Hz was
     * blinding.
     *
     * So the span is FIXED, relative to this capture's median: black at +4 dB,
     * saturated at +16 dB. A WSPR signal in a 1.4648 Hz bin runs ~7 dB above
     * the floor when weak and ~20 dB when strong, so that maps them to 25% and
     * fully-on respectively. Interference still clips - but clipping the QRM is
     * the correct trade, because the display exists to show WSPR traces. The
     * floor stays adaptive (the median), so this still tracks band noise; only
     * the SPAN is now independent of what happens to be loudest. */
    float lo_db = 4.0f;
    float hi_db = 16.0f;

    /* What the old adaptive rule WOULD have chosen, so a screenshot that still
     * looks wrong can be judged against a number instead of an impression. */
    ESP_LOGI(TAG, "waterfall: span %.0f-%.0f dB over median; loudest bin +%.1f dB",
             lo_db, hi_db, 10.0f * log10f(top / med));

    if (s_wf_mtx) xSemaphoreTake(s_wf_mtx, portMAX_DELAY);
    for (int i = 0; i < WSPR_WF_ROWS * WSPR_WF_COLS; i++) {
        float db = 10.0f * log10f((mag[i] + 1e-20f) / med);
        float t = (db - lo_db) / (hi_db - lo_db);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        s_wf[i] = (uint8_t)(t * 255.0f);
    }
    s_wf_seq++;
    if (s_wf_mtx) xSemaphoreGive(s_wf_mtx);

    free(in); free(sp); free(mag); free(cfg);
}

bool wspr_rx_get_waterfall(uint8_t *out)
{
    if (!s_wf || !out || s_wf_seq == 0) return false;
    if (s_wf_mtx) xSemaphoreTake(s_wf_mtx, portMAX_DELAY);
    memcpy(out, s_wf, WSPR_WF_ROWS * WSPR_WF_COLS);
    if (s_wf_mtx) xSemaphoreGive(s_wf_mtx);
    return true;
}

uint32_t wspr_rx_waterfall_seq(void) { return s_wf_seq; }

static void wspr_rx_task(void *arg)
{
    (void)arg;

    /* Two buffers, allocated once and reused for the life of the loop.
     *
     * float capture (5.76 MB) + int16 for the decoder (2.88 MB) = 8.6 MB of the
     * ~14.7 MB of free PSRAM. Allocating per cycle instead would risk failing on
     * a fragmented heap 20 minutes in, which is exactly the kind of fault that
     * shows up as "it stopped decoding overnight" and is miserable to chase. */
    float   *cap = heap_caps_malloc((size_t)CAP_SAMPLES * sizeof(float),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc((size_t)CAP_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cap || !pcm) {
        ESP_LOGE(TAG, "could not allocate the capture buffers (%u KB + %u KB)",
                 (unsigned)(CAP_SAMPLES * sizeof(float) / 1024),
                 (unsigned)(CAP_SAMPLES * sizeof(int16_t) / 1024));
        free(cap); free(pcm);
        set_status("out of memory");
        s_run = false; s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "slot loop up: %u KB capture + %u KB decode, searching %.0f-%.0f Hz",
             (unsigned)(CAP_SAMPLES * sizeof(float) / 1024),
             (unsigned)(CAP_SAMPLES * sizeof(int16_t) / 1024),
             SEARCH_LO_HZ, SEARCH_HI_HZ);

    while (s_run) {
        /* ---- wait for the next even UTC minute ---- */
        int64_t t = now_ms();
        int64_t into = t % WSPR_CYCLE_MS;
        int64_t wait = (into == 0) ? 0 : (WSPR_CYCLE_MS - into);
        set_status("waiting %llds", (long long)(wait / 1000));
        while (s_run && wait > 0) {
            int64_t chunk = wait > 500 ? 500 : wait;   /* stay responsive to stop */
            vTaskDelay(pdMS_TO_TICKS((uint32_t)chunk));
            t = now_ms();
            into = t % WSPR_CYCLE_MS;
            wait = (into == 0) ? 0 : (WSPR_CYCLE_MS - into);
            if (wait > WSPR_CYCLE_MS - 100) break;     /* boundary just passed */
        }
        if (!s_run) break;

        int64_t cycle_utc = (now_ms() / WSPR_CYCLE_MS) * (WSPR_CYCLE_MS / 1000);

        /* ---- SIMULATION: synthesize the window instead of capturing it ----
         *
         * Everything downstream is untouched - the same waterfall, the same
         * decoder, the same spot store, the same page. That is the whole value:
         * a sim that shortcut the decoder would test nothing and would quietly
         * misrepresent what the radio can hear. If a phantom does not decode,
         * it does not appear.
         *
         * No 120 s wait either. The audio is generated in a moment, so results
         * land ~70 s into the cycle instead of ~190 s, which makes this usable
         * for practice and for iterating on the display. */
        if (wspr_sim_enabled()) {
            set_status("simulating");
            wspr_sim_build_window(pcm, CAP_SAMPLES, cycle_utc);
            build_waterfall(pcm, CAP_SAMPLES);
            goto decode_window;
        }

        /* ---- capture the window ----
         * backfill covers however late we armed: the pre-ring is already being
         * filled continuously by the DSP in this mode, so the window is anchored
         * to the boundary rather than to when this task got a slice. Same
         * mechanism, and same reason, as the FT8 boundary-discard fix. */
        uint32_t start_off_ms = (uint32_t)(now_ms() % WSPR_CYCLE_MS);
        uint32_t backfill     = start_off_ms * (uint32_t)(WSPR_SAMPLE_RATE_HZ / 1000);
        esp_err_t err = dsp_ft8_capture_begin(cap, CAP_SAMPLES, backfill);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "capture_begin failed (%s) - skipping this cycle",
                     esp_err_to_name(err));
            set_status("capture failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "cycle %lld: capturing (armed +%u ms)",
                 (long long)cycle_utc, (unsigned)start_off_ms);

        while (s_run) {
            int got = dsp_ft8_capture_progress();
            if (got >= (int)CAP_SAMPLES) break;
            int64_t elapsed = now_ms() % WSPR_CYCLE_MS;
            if (elapsed > WSPR_CYCLE_MS - 500 && got > 0) break;  /* boundary */
            set_status("capturing %d/120 s", got / (int)WSPR_SAMPLE_RATE_HZ);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        dsp_ft8_capture_finish(2000);
        if (!s_run) break;

        /* ---- float -> int16 for the decoder ----
         * The capture pipeline produces floats; the decoder takes int16, the
         * format every WSPR reference recording uses and the one the host
         * harnesses are validated against. Converting is cheap next to the
         * decode and keeps the decoder byte-identical to the tested one. */
        /* ---- float -> int16, NORMALISED ----
         *
         * MEASURED, and it overturned the obvious assumption. The capture
         * pipeline hands back floats built from raw int16 IQ, so a straight cast
         * looked right - but the real levels off the air are
         * min=-57.5 max=22.2 rms=3.7. Casting those to int16 gives the decoder
         * about SIX bits of a sixteen-bit format and silently throws ~9 bits of
         * dynamic range away. It still decoded a strong Australian beacon, which
         * is exactly why this would have gone unnoticed: the failure is not "no
         * decodes", it is "only the loud ones".
         *
         * A per-capture gain is the correct fix rather than a fudge: every stage
         * downstream - the comb search, the sync correlation, the Fano metric -
         * works on RELATIVE power within the capture, so scaling the whole window
         * changes nothing except how much of the int16 range is used.
         *
         * Guarded against silence: a capture of near-nothing must not be
         * amplified into full-scale noise, which would manufacture candidates
         * out of the noise floor. */
        float peak = 0.0f;
        for (uint32_t i = 0; i < CAP_SAMPLES; i++) {
            float a = cap[i] < 0 ? -cap[i] : cap[i];
            if (a > peak) peak = a;
        }
        float gain = 1.0f;
        if (peak > 1e-3f) {
            gain = 28000.0f / peak;          /* headroom below full scale */
            if (gain > 4096.0f) gain = 4096.0f;   /* silence guard */
            if (gain < 1.0f)    gain = 1.0f;      /* already large: don't attenuate */
        }
        double sumsq = 0;
        for (uint32_t i = 0; i < CAP_SAMPLES; i++) {
            float v = cap[i] * gain;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            sumsq += (double)v * v;
            pcm[i] = (int16_t)v;
        }
        ESP_LOGW(TAG, "capture level: peak=%.1f gain=%.0fx -> rms=%.0f of 32768",
                 peak, gain, sqrt(sumsq / CAP_SAMPLES));

        /* Built BEFORE the decode, not after: the decode takes ~66 s and the
         * operator should see the window they just captured while it runs, not
         * a minute later. */
        build_waterfall(pcm, CAP_SAMPLES);

    decode_window:
        /* ---- decode ---- */
        int64_t t0 = esp_timer_get_time();
        wspr_freq_candidate_t cands[WSPR_MAX_CANDS];
        int ncand = wspr_find_candidates(pcm, CAP_SAMPLES, SEARCH_LO_HZ, SEARCH_HI_HZ,
                                          cands, WSPR_MAX_CANDS);
        int decoded = 0;
        for (int i = 0; i < ncand && s_run; i++) {
            set_status("decoding %d/%d", i + 1, ncand);
            wspr_decode_result_t r;
            wspr_decode_candidate(pcm, CAP_SAMPLES, cands[i].freq_hz, &r);
            /* Log EVERY candidate, not just the ones that decode. A silent
             * "0 decodes" cannot distinguish "nothing was on the air" from
             * "the search looked in the wrong place" from "the audio was
             * wrong" - and those need completely different fixes. */
            ESP_LOGI(TAG, "  cand %d: f=%.2f Hz score=%.3g cycles=%u %s",
                     i, cands[i].freq_hz, (double)cands[i].comb_score,
                     r.cycles, r.ok ? "DECODED" : "rejected");
            if (!r.ok) continue;
            decoded++;
            ESP_LOGW(TAG, "  DECODED '%s' '%s' %d dBm  f=%.2f Hz dt=%.2fs cycles=%u",
                     r.callsign, r.grid, r.power_dbm, r.freq_hz,
                     r.best_dt_samples / WSPR_SAMPLE_RATE_HZ, r.cycles);
            file_spot(&r, cycle_utc, WSPR_SNR_UNKNOWN);
        }
        int64_t dec_ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGW(TAG, "cycle %lld: %d candidate(s), %d decode(s), %lld ms",
                 (long long)cycle_utc, ncand, decoded, (long long)dec_ms);
        set_status("%d decoded", decoded);
    }

    free(cap);
    free(pcm);
    set_status("idle");
    ESP_LOGI(TAG, "slot loop stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

bool wspr_rx_start(void)
{
    if (s_run) return true;

    /* Claimed BEFORE the task exists, and cleared if creation fails - #199: a
     * flag set as the task's first statement means "has begun running" while
     * every reader needs "exists", and on this board the gap between the two is
     * long enough to matter. */
    if (!s_wf_mtx) s_wf_mtx = xSemaphoreCreateMutex();
    s_run = true;
    ui_mode_set(UI_MODE_WSPR);

    /* 32 KB and in PSRAM: wspr_decode_candidate() alone reserves a flat 16 KB
     * frame in its prologue, the self-test measured ~27.5 KB of high-water, and
     * xTaskCreate() would take that from the ~40 KB of free INTERNAL RAM. This
     * is background work on a two-minute cadence, which is what
     * psram_task_create() is for. */
    s_task = psram_task_create(wspr_rx_task, "wspr_rx", 32768, NULL,
                               tskIDLE_PRIORITY + 1, tskNO_AFFINITY);
    if (!s_task) {
        ESP_LOGE(TAG, "could not create the slot-loop task");
        s_run = false;
        ui_mode_set(UI_MODE_PANADAPTER);
        return false;
    }
    return true;
}

void wspr_rx_stop(void)
{
    if (!s_run) return;
    s_run = false;
    /* The task frees its own buffers and clears s_task; the mode goes back here
     * so the panadapter starts drawing again immediately rather than waiting for
     * a capture in flight to finish. */
    ui_mode_set(UI_MODE_PANADAPTER);
}
