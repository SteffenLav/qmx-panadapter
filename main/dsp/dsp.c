#include "dsp.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

// Phase 5.6: enable DC blocker on I/Q stream before FFT (standard SDR hygiene)
// Set to 0 to bypass.
#ifndef DSP_DC_BLOCKER
#define DSP_DC_BLOCKER 1
#endif

// Phase 5.8: dBm calibration offset. Added to every dB value before display so
// readings match real-world signal strength. Procedure: with QMX on dummy load,
// log the per-second MEDIAN dB across all bins (= noise floor in raw dB);
// the offset is then -130 - median.
#ifndef DSP_DB_CALIBRATION_OFFSET
#define DSP_DB_CALIBRATION_OFFSET -148.0f  /* measured against QMX on dummy load (-130 dBm floor target) */
#endif
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "dsps_fft2r.h"
#include "dsps_wind_blackman_harris.h"
#include "dsps_fir.h"

#include "audio.h"
#include "ui_mode.h"

static const char *TAG = "dsp";

#define STATS_PERIOD_MS  1000

// Buffers
static float *s_window   = NULL;
static float *s_workbuf  = NULL;
static float *s_spectrum = NULL;

static SemaphoreHandle_t s_spectrum_mtx = NULL;
static bool s_have_spectrum = false;

static TaskHandle_t s_fft_task = NULL;
static uint32_t s_frames_this_period = 0;
static int64_t  s_period_start_us = 0;

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ----- v0.16.0 Zoom-FFT --------------------------------------------------
#define ZOOM_FIR_LEN 31

static SemaphoreHandle_t s_zoom_cfg_mtx  = NULL;

// Config, written by dsp_set_zoom() (UI/LVGL task), read by fft_task.
static volatile int   s_zoom_decim  = 1;   // 1, 2, 4, 8, 16
static volatile float s_zoom_residual = 1.0f;
static volatile float s_zoom_fshift_hz = 0.0f;
static volatile int   s_zoom_gen    = 0;

// Applied state, owned by fft_task only.
static int   s_zoom_applied_gen   = -1;
static float s_zoom_lpf_taps[ZOOM_FIR_LEN];
static float s_zoom_fir_delay_i[ZOOM_FIR_LEN];
static float s_zoom_fir_delay_q[ZOOM_FIR_LEN];
static fir_f32_t s_zoom_fir_i;
static fir_f32_t s_zoom_fir_q;
static float s_zoom_rot_re, s_zoom_rot_im;     // per-sample NCO rotation
static float s_zoom_phase_re, s_zoom_phase_im; // current NCO phase
static float s_zoom_mix_i[DSP_FFT_SIZE];
static float s_zoom_mix_q[DSP_FFT_SIZE];
static float s_zoom_dec_i[DSP_FFT_SIZE];
static float s_zoom_dec_q[DSP_FFT_SIZE];
static float *s_zoom_acc      = NULL;  // interleaved I/Q, DSP_FFT_SIZE complex
static float *s_zoom_workbuf  = NULL;  // interleaved I/Q, DSP_FFT_SIZE complex
// Double-buffered published spectrum (dB, DSP_FFT_SIZE each). fft_task
// writes into the non-ready buffer then flips s_zoom_ready_idx; readers
// (LVGL/render task) take whichever buffer is currently marked ready
// without copying or locking. Lock-free: a 32-bit aligned int read/write
// is atomic on this target, and the writer never touches the ready buffer.
static float *s_zoom_spectrum[2] = { NULL, NULL };
static volatile int s_zoom_ready_idx = -1;
static int    s_zoom_acc_idx  = 0;

// Windowed-sinc lowpass, Hamming window. fc_norm = cutoff / sample_rate.
static void zoom_design_lpf(float *taps, int n_taps, float fc_norm)
{
    int M = n_taps - 1;
    float sum = 0.0f;
    for (int i = 0; i <= M; i++) {
        float k = (float)i - (float)M / 2.0f;
        float sinc;
        if (fabsf(k) < 1e-6f) {
            sinc = 2.0f * fc_norm;
        } else {
            sinc = sinf(2.0f * (float)M_PI * fc_norm * k) / ((float)M_PI * k);
        }
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (float)M);
        taps[i] = sinc * w;
        sum += taps[i];
    }
    for (int i = 0; i <= M; i++) taps[i] /= sum;
}

// ----- Step 3 v0.10 FT8 RX capture -----------------------------------------
// 31-tap LPF: scipy.signal.firwin(31, 4500/24000, window='hamming')
// Passband 0-3 kHz flat (<0.5 dB), -65 dB at 9 kHz (worst aliaser into
// 0-3 kHz after /4 decimation to 12 kHz).
static const float s_ft8_lpf_taps[31] = {
    +9.4083799097e-04f, +1.8869410051e-03f, +2.8691449162e-03f, +3.1405658090e-03f,
    +1.3076221170e-03f, -3.7668515379e-03f, -1.1670857504e-02f, -1.9524454627e-02f,
    -2.2180134993e-02f, -1.3814782554e-02f, +9.5396753619e-03f, +4.7587798377e-02f,
    +9.4688024813e-02f, +1.4084394523e-01f, +1.7463386062e-01f, +1.8703732996e-01f,
    +1.7463386062e-01f, +1.4084394523e-01f, +9.4688024813e-02f, +4.7587798377e-02f,
    +9.5396753619e-03f, -1.3814782554e-02f, -2.2180134993e-02f, -1.9524454627e-02f,
    -1.1670857504e-02f, -3.7668515379e-03f, +1.3076221170e-03f, +3.1405658090e-03f,
    +2.8691449162e-03f, +1.8869410051e-03f, +9.4083799097e-04f
};
static float       s_ft8_fir_delay[31];
static fir_f32_t   s_ft8_fir;
static SemaphoreHandle_t s_ft8_done_sem = NULL;
static float *s_ft8_dst    = NULL;
static volatile int s_ft8_idx    = 0;   // advanced by fft_task; polled cross-task by the FT8 capture caller
static int    s_ft8_target = 0;
static volatile bool s_ft8_active = false;
static float s_ft8_mix_buf[DSP_FFT_SIZE];
static float s_ft8_dec_buf[DSP_FFT_SIZE / 4];
// Debug instrumentation: once-per-second log of FT8 branch activity.
static uint32_t s_ft8_iter_count = 0;
static uint32_t s_ft8_total_n_out = 0;
static int64_t  s_ft8_last_log_us = 0;
static uint64_t s_ft8_read_us = 0;
static uint64_t s_ft8_work_us = 0;
static uint32_t s_ft8_slow_reads = 0;
static uint32_t s_ft8_slow_works = 0;

// Incremental capture API (v0.18 fast-decode). Splits the old one-shot
// dsp_ft8_capture into arm / poll-progress / finalize so the caller can run
// the FT8 STFT (monitor_process) block-by-block *during* the slot instead of
// batching it after capture completes. The audio producer (fft_task FT8 branch)
// is unchanged — it still mixes, decimates and appends decimated 12 kHz samples
// to s_ft8_dst, advancing s_ft8_idx.

// Arm capture: dst MUST point to >= target_samples floats in PSRAM.
esp_err_t dsp_ft8_capture_begin(float *dst, uint32_t target_samples)
{
    if (!dst) return ESP_ERR_INVALID_ARG;
    if (!s_ft8_done_sem) {
        s_ft8_done_sem = xSemaphoreCreateBinary();
        if (!s_ft8_done_sem) return ESP_ERR_NO_MEM;
    }
    memset(s_ft8_fir_delay, 0, sizeof(s_ft8_fir_delay));
    esp_err_t e = dsps_fird_init_f32(&s_ft8_fir,
                                     (float *)s_ft8_lpf_taps,
                                     s_ft8_fir_delay,
                                     31, 4);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "dsps_fird_init_f32 failed: %d", e);
        return e;
    }
    xSemaphoreTake(s_ft8_done_sem, 0);  // drain stale
    s_ft8_dst    = dst;
    s_ft8_idx    = 0;
    s_ft8_target = (int)target_samples;
    __sync_synchronize();
    s_ft8_active = true;
    return ESP_OK;
}

// Number of decimated 12 kHz samples committed to dst so far. Cross-task read
// of a volatile, monotonically-increasing counter — safe to poll.
int dsp_ft8_capture_progress(void)
{
    __sync_synchronize();
    return s_ft8_idx;
}

// Finalize: wait up to timeout_ms for the target sample count, then disarm and
// zero-pad any shortfall (the slot's dead air after the FT8 signal).
// timeout_ms is the time until the next UTC slot boundary: the caller caps each
// capture there so the 15 s window stays anchored to the FT8 timing grid.
// Returns ESP_ERR_TIMEOUT only on a genuine audio failure (zero samples).
esp_err_t dsp_ft8_capture_finish(uint32_t timeout_ms)
{
    BaseType_t ok = xSemaphoreTake(s_ft8_done_sem, pdMS_TO_TICKS(timeout_ms));
    s_ft8_active = false;
    __sync_synchronize();

    if (ok != pdTRUE) {
        // Boundary reached before a full frame. This is the normal path (the
        // QMX clock isn't bit-exact 48 kHz, so 180000 samples take a hair more
        // than one slot). Let the FT8 branch finish any in-flight chunk, then
        // zero-pad the tail — it's the slot's dead air, after the FT8 signal.
        vTaskDelay(pdMS_TO_TICKS(20));
        int got = s_ft8_idx;
        if (got <= 0) {
            ESP_LOGW(TAG, "FT8 capture: no audio in %lu ms", (unsigned long)timeout_ms);
            return ESP_ERR_TIMEOUT;     // genuine audio failure
        }
        if (got < s_ft8_target) {
            memset(&s_ft8_dst[got], 0,
                   (size_t)(s_ft8_target - got) * sizeof(float));
        }
        ESP_LOGD(TAG, "FT8 capture cut at boundary: %d/%d samples (padded)",
                 got, s_ft8_target);
    }
    return ESP_OK;
}

// One-shot capture: arm + finalize. Retained for any non-streaming caller.
esp_err_t dsp_ft8_capture(float *dst_180000, uint32_t timeout_ms)
{
    esp_err_t e = dsp_ft8_capture_begin(dst_180000, 180000);
    if (e != ESP_OK) return e;
    return dsp_ft8_capture_finish(timeout_ms);
}
// ----- end Step 3 v0.10 FT8 RX capture --------------------------------------

static void fft_task(void *arg);

esp_err_t dsp_init(void)
{
    ESP_LOGI(TAG, "DSP init (Phase 4.2 - real-time FFT on audio ring buffer)");

    s_window = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_workbuf = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(float),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_spectrum = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_window || !s_workbuf || !s_spectrum) {
        ESP_LOGE(TAG, "Failed to allocate DSP buffers");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Allocated buffers: window=%d B, workbuf=%d B, spectrum=%d B",
             (int)(DSP_FFT_SIZE * sizeof(float)),
             (int)(DSP_FFT_SIZE * 2 * sizeof(float)),
             (int)(DSP_FFT_SIZE * sizeof(float)));

    dsps_wind_blackman_harris_f32(s_window, DSP_FFT_SIZE);
    ESP_LOGI(TAG, "Blackman-Harris window computed");

    esp_err_t err = dsps_fft2r_init_fc32(NULL, DSP_FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: 0x%x (%s)",
                 err, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "esp-dsp FFT initialized for %d-pt complex", DSP_FFT_SIZE);

    s_spectrum_mtx = xSemaphoreCreateMutex();
    if (!s_spectrum_mtx) return ESP_ERR_NO_MEM;

    // v0.16.0 zoom-FFT buffers (PSRAM; only touched when zoom > x1).
    s_zoom_acc        = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_zoom_workbuf    = heap_caps_malloc(DSP_FFT_SIZE * 2 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_zoom_spectrum[0] = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_zoom_spectrum[1] = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_zoom_acc || !s_zoom_workbuf || !s_zoom_spectrum[0] || !s_zoom_spectrum[1]) {
        ESP_LOGE(TAG, "Failed to allocate zoom-FFT buffers");
        return ESP_ERR_NO_MEM;
    }
    s_zoom_cfg_mtx = xSemaphoreCreateMutex();
    if (!s_zoom_cfg_mtx) return ESP_ERR_NO_MEM;

    // ---- Self-test: same as Phase 4.1 — validate the FFT works at boot ----
    const int test_bin = 100;
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float phase = 2.0f * (float)M_PI * (float)test_bin * (float)i
                      / (float)DSP_FFT_SIZE;
        s_workbuf[2*i]     = cosf(phase) * s_window[i];
        s_workbuf[2*i + 1] = sinf(phase) * s_window[i];
    }
    dsps_fft2r_fc32(s_workbuf, DSP_FFT_SIZE);
    dsps_bit_rev_fc32(s_workbuf, DSP_FFT_SIZE);
    float peak_mag = 0.0f;
    int peak_idx = -1;
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float re = s_workbuf[2*i];
        float im = s_workbuf[2*i + 1];
        float mag = re*re + im*im;
        if (mag > peak_mag) { peak_mag = mag; peak_idx = i; }
    }
    if (peak_idx == test_bin) {
        ESP_LOGI(TAG, "Self-test PASSED (bin %d -> bin %d)", test_bin, peak_idx);
    } else {
        ESP_LOGW(TAG, "Self-test FAILED (expected bin %d, got %d)", test_bin, peak_idx);
    }

    // Spawn the real-time FFT task on core 1 at lower priority than audio.
    BaseType_t ok = xTaskCreatePinnedToCore(
        fft_task, "fft_task", 6144, NULL, 4, &s_fft_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fft_task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "FFT task started");
    return ESP_OK;
}

esp_err_t dsp_get_spectrum(float *dst)
{
    if (!s_spectrum_mtx || !dst) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_spectrum_mtx, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_have_spectrum) {
        xSemaphoreGive(s_spectrum_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(dst, s_spectrum, DSP_FFT_SIZE * sizeof(float));
    xSemaphoreGive(s_spectrum_mtx);
    return ESP_OK;
}

// Phase 5.10D: peak dBm in a window centered on the VFO bin.
// s_spectrum is the raw (non-fftshifted) post-FFT array, DC at index 0. The
// VFO sits at the +12 kHz IF offset, not at DC, so the caller passes the
// VFO's raw-array bin index (center_bin); we search +/-half_width_bins
// around it, wrapping at the array edges.
esp_err_t dsp_get_peak_dbm_around_vfo(int center_bin, int half_width_bins, float *peak_dbm)
{
    if (!s_spectrum_mtx || !peak_dbm) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_spectrum_mtx, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_have_spectrum) {
        xSemaphoreGive(s_spectrum_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    if (half_width_bins <= 0) half_width_bins = 1;
    if (half_width_bins > DSP_FFT_SIZE / 2) half_width_bins = DSP_FFT_SIZE / 2;
    center_bin = ((center_bin % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE;
    float peak = -1e9f;
    for (int d = -half_width_bins; d <= half_width_bins; d++) {
        int i = ((center_bin + d) % DSP_FFT_SIZE + DSP_FFT_SIZE) % DSP_FFT_SIZE;
        if (s_spectrum[i] > peak) peak = s_spectrum[i];
    }
    *peak_dbm = peak;
    xSemaphoreGive(s_spectrum_mtx);
    return ESP_OK;
}

// Snap-to-peak — see dsp.h for contract.
esp_err_t dsp_find_peak_hz_around(int32_t center_hz, int32_t radius_hz, int32_t *out_hz)
{
    if (!s_spectrum_mtx || !out_hz) return ESP_ERR_INVALID_ARG;
    if (radius_hz <= 0) { *out_hz = center_hz; return ESP_OK; }

    if (xSemaphoreTake(s_spectrum_mtx, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_have_spectrum) {
        xSemaphoreGive(s_spectrum_mtx);
        *out_hz = center_hz;
        return ESP_ERR_NOT_FOUND;
    }

    // Convert "Hz from dial" to "DSP bin index".
    // DSP bin 0 = audio DC; QMX dial is at +12 kHz in baseband, so a touch at
    // center_hz from dial maps to baseband freq (center_hz + 12000) Hz, which is
    // bin (center_hz + 12000) / bin_width. Negative bins wrap to N - |bin|.
    const float bin_width = (float)DSP_SAMPLE_RATE_HZ / (float)DSP_FFT_SIZE;  // 46.875 Hz
    const int N = DSP_FFT_SIZE;

    int center_bin = (int)((center_hz + 12000) / bin_width);  // truncation OK; we search a window
    int radius_bins = (int)(radius_hz / bin_width);
    if (radius_bins < 1) radius_bins = 1;

    // Search [center_bin - radius_bins, center_bin + radius_bins], wrapping mod N.
    // Track the peak's offset (in bins) FROM the search center, not its absolute
    // bin, so the result is always within ±radius_hz of the tapped position.
    float peak_db = -1e9f;
    int   peak_d  = 0;
    float sum_db = 0.0f;
    int   count = 0;
    for (int d = -radius_bins; d <= radius_bins; d++) {
        int b = ((center_bin + d) % N + N) % N;  // positive modulo
        float v = s_spectrum[b];
        sum_db += v;
        count++;
        if (v > peak_db) { peak_db = v; peak_d = d; }
    }
    float mean_db = (count > 0) ? (sum_db / (float)count) : -120.0f;
    xSemaphoreGive(s_spectrum_mtx);

    // Only snap if the peak is meaningfully above local mean (avoids snapping to noise).
    if (peak_db - mean_db < 3.0f) {
        *out_hz = center_hz;
        return ESP_OK;
    }

    // Return the peak relative to the tapped position. The old code unwrapped the
    // peak's *absolute* bin to a signed frequency, which aliased once
    // center_hz+12 kHz crossed the +24 kHz Nyquist edge (taps in the right ~quarter
    // of the 48 kHz display) — wrapping to a large NEGATIVE frequency, so a tap to
    // the RIGHT tuned DOWN (reported by Ian G4LXX). Offsetting from center_hz keeps
    // the snap inside ±radius_hz by construction, so direction is always preserved.
    *out_hz = center_hz + (int32_t)lroundf((float)peak_d * bin_width);
    return ESP_OK;
}

static void log_stats(float min_db, float max_db, float mean_db)
{
    int64_t now = esp_timer_get_time();
    if (s_period_start_us == 0) { s_period_start_us = now; return; }
    if (now - s_period_start_us < STATS_PERIOD_MS * 1000) return;

    uint32_t frames = s_frames_this_period;
    s_frames_this_period = 0;
    s_period_start_us = now;
    ESP_LOGD(TAG, "Spectrum: min=%.1f dBm, max=%.1f dBm, mean=%.1f dBm, frames=%u/s",
             min_db, max_db, mean_db, (unsigned)frames);
}

// Shared DC-blocker/window/FFT/dB/publish path, used both for the normal
// panadapter pipeline and periodically during FT8 capture to keep the
// S-meter (dsp_get_peak_dbm_around_vfo reads s_spectrum) updating.
static void compute_and_publish_spectrum(int16_t *samples, float *tmp_spectrum,
                                          float *last_min, float *last_max, float *last_mean)
{
#if DSP_DC_BLOCKER
    {
        static float dc_state_I = 0.0f;
        static float dc_state_Q = 0.0f;
        const float alpha = 0.9869f;
        for (int i = 0; i < DSP_FFT_SIZE; i++) {
            float xI = (float)samples[2*i];
            float xQ = (float)samples[2*i + 1];
            float cI = xI + dc_state_I * alpha;
            float cQ = xQ + dc_state_Q * alpha;
            float yI = cI - dc_state_I;
            float yQ = cQ - dc_state_Q;
            dc_state_I = cI;
            dc_state_Q = cQ;
            samples[2*i]     = (int16_t)yI;
            samples[2*i + 1] = (int16_t)yQ;
        }
    }
#endif
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float I = (float)samples[2*i];
        float Q = (float)samples[2*i + 1];
        float w = s_window[i];
        s_workbuf[2*i]     = I * w;
        s_workbuf[2*i + 1] = Q * w;
    }

    dsps_fft2r_fc32(s_workbuf, DSP_FFT_SIZE);
    dsps_bit_rev_fc32(s_workbuf, DSP_FFT_SIZE);

    const float floor_mag2 = 1e-12f;
    float sum_db = 0.0f;
    float min_db = +1e9f;
    float max_db = -1e9f;
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float re = s_workbuf[2*i];
        float im = s_workbuf[2*i + 1];
        float mag2 = re*re + im*im;
        if (mag2 < floor_mag2) mag2 = floor_mag2;
        float db = 10.0f * log10f(mag2) + DSP_DB_CALIBRATION_OFFSET;
        tmp_spectrum[i] = db;
        sum_db += db;
        if (db < min_db) min_db = db;
        if (db > max_db) max_db = db;
    }
    *last_min = min_db;
    *last_max = max_db;
    *last_mean = sum_db / (float)DSP_FFT_SIZE;

    if (xSemaphoreTake(s_spectrum_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(s_spectrum, tmp_spectrum, DSP_FFT_SIZE * sizeof(float));
        s_have_spectrum = true;
        xSemaphoreGive(s_spectrum_mtx);
    }

    s_frames_this_period++;
    log_stats(*last_min, *last_max, *last_mean);
}

static void zoom_run_fft(void)
{
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float w = s_window[i];
        s_zoom_workbuf[2*i]     = s_zoom_acc[2*i]     * w;
        s_zoom_workbuf[2*i + 1] = s_zoom_acc[2*i + 1] * w;
    }
    dsps_fft2r_fc32(s_zoom_workbuf, DSP_FFT_SIZE);
    dsps_bit_rev_fc32(s_zoom_workbuf, DSP_FFT_SIZE);

    // Write into the buffer that isn't currently published (index 0 the
    // first time, when s_zoom_ready_idx is still -1).
    int ridx = s_zoom_ready_idx;
    int widx = (ridx == 0) ? 1 : 0;
    float *out = s_zoom_spectrum[widx];
    const float *prev = (ridx >= 0) ? s_zoom_spectrum[ridx] : NULL;

    // Light EMA across successive zoom-FFT frames to take the edge off
    // frame-to-frame jumps at high decimation (slower cadence). Too low an
    // alpha smears/blurs the waterfall (signals visibly trail/fade), so
    // keep this fairly high - it's a touch-up, not the main smoothing.
    const float alpha = 0.6f;
    const float floor_mag2 = 1e-12f;
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float re = s_zoom_workbuf[2*i];
        float im = s_zoom_workbuf[2*i + 1];
        float mag2 = re*re + im*im;
        if (mag2 < floor_mag2) mag2 = floor_mag2;
        float db = 10.0f * log10f(mag2) + DSP_DB_CALIBRATION_OFFSET;
        out[i] = prev ? (alpha * db + (1.0f - alpha) * prev[i]) : db;
    }
    s_zoom_ready_idx = widx;
}

// Mix the pan-center to DC, LPF + decimate by D, accumulate DSP_FFT_SIZE
// decimated complex samples and run a second FFT. `samples` is the same
// (already DC-blocked) buffer compute_and_publish_spectrum just used.
static void zoom_process(const int16_t *samples)
{
    int D;
    int gen;
    float fshift;
    if (xSemaphoreTake(s_zoom_cfg_mtx, 0) != pdTRUE) return;
    D = s_zoom_decim;
    gen = s_zoom_gen;
    fshift = s_zoom_fshift_hz;
    xSemaphoreGive(s_zoom_cfg_mtx);

    if (D <= 1) {
        s_zoom_ready_idx = -1;
        s_zoom_applied_gen = -1;  // force re-init if D goes back up
        return;
    }

    if (gen != s_zoom_applied_gen) {
        float cutoff_norm = 0.45f / (float)D;
        zoom_design_lpf(s_zoom_lpf_taps, ZOOM_FIR_LEN, cutoff_norm);
        memset(s_zoom_fir_delay_i, 0, sizeof(s_zoom_fir_delay_i));
        memset(s_zoom_fir_delay_q, 0, sizeof(s_zoom_fir_delay_q));
        dsps_fird_init_f32(&s_zoom_fir_i, s_zoom_lpf_taps, s_zoom_fir_delay_i, ZOOM_FIR_LEN, D);
        dsps_fird_init_f32(&s_zoom_fir_q, s_zoom_lpf_taps, s_zoom_fir_delay_q, ZOOM_FIR_LEN, D);

        float omega = -2.0f * (float)M_PI * fshift / (float)DSP_SAMPLE_RATE_HZ;
        s_zoom_rot_re = cosf(omega);
        s_zoom_rot_im = sinf(omega);
        s_zoom_phase_re = 1.0f;
        s_zoom_phase_im = 0.0f;
        s_zoom_acc_idx = 0;
        s_zoom_ready_idx = -1;
        s_zoom_applied_gen = gen;
    }

    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float I = (float)samples[2*i];
        float Q = (float)samples[2*i + 1];
        float ri = s_zoom_phase_re, rq = s_zoom_phase_im;
        s_zoom_mix_i[i] = I * ri - Q * rq;
        s_zoom_mix_q[i] = I * rq + Q * ri;
        float nr = ri * s_zoom_rot_re - rq * s_zoom_rot_im;
        float nq = ri * s_zoom_rot_im + rq * s_zoom_rot_re;
        s_zoom_phase_re = nr;
        s_zoom_phase_im = nq;
    }
    // Periodic renormalization keeps the NCO phasor on the unit circle.
    float mag2 = s_zoom_phase_re * s_zoom_phase_re + s_zoom_phase_im * s_zoom_phase_im;
    if (mag2 < 0.95f || mag2 > 1.05f) {
        float inv = 1.0f / sqrtf(mag2);
        s_zoom_phase_re *= inv;
        s_zoom_phase_im *= inv;
    }

    int n_out_target = DSP_FFT_SIZE / D;
    int n_out_i = dsps_fird_f32(&s_zoom_fir_i, s_zoom_mix_i, s_zoom_dec_i, n_out_target);
    int n_out_q = dsps_fird_f32(&s_zoom_fir_q, s_zoom_mix_q, s_zoom_dec_q, n_out_target);
    int n_out = (n_out_i < n_out_q) ? n_out_i : n_out_q;

    for (int i = 0; i < n_out; i++) {
        if (s_zoom_acc_idx < DSP_FFT_SIZE) {
            s_zoom_acc[2*s_zoom_acc_idx]     = s_zoom_dec_i[i];
            s_zoom_acc[2*s_zoom_acc_idx + 1] = s_zoom_dec_q[i];
            s_zoom_acc_idx++;
        }
        if (s_zoom_acc_idx >= DSP_FFT_SIZE) {
            zoom_run_fft();
            s_zoom_acc_idx = 0;
        }
    }
}

// Update the zoom-FFT configuration. Called from the UI/LVGL task on every
// zoom/pan change; fft_task picks up the new generation on its next iteration.
void dsp_set_zoom(float zoom_factor, int pan_offset_bins, int if_bin_shift)
{
    int D = 1;
    if (zoom_factor >= 16.0f)      D = 16;
    else if (zoom_factor >= 8.0f)  D = 8;
    else if (zoom_factor >= 4.0f)  D = 4;
    else if (zoom_factor >= 2.0f)  D = 2;
    float residual = zoom_factor / (float)D;

    const int N = DSP_FFT_SIZE;
    int c = ((if_bin_shift + pan_offset_bins) % N + N) % N;
    int signed_bin = (c >= N / 2) ? c - N : c;
    float fshift_hz = (float)signed_bin * (float)DSP_SAMPLE_RATE_HZ / (float)N;

    if (!s_zoom_cfg_mtx) return;
    if (xSemaphoreTake(s_zoom_cfg_mtx, portMAX_DELAY) == pdTRUE) {
        s_zoom_decim     = D;
        s_zoom_residual  = residual;
        s_zoom_fshift_hz = fshift_hz;
        s_zoom_gen++;
        xSemaphoreGive(s_zoom_cfg_mtx);
    }
}

int dsp_get_zoom_decim(void)
{
    return s_zoom_decim;
}

float dsp_get_zoom_residual(void)
{
    return s_zoom_residual;
}

const float *dsp_get_zoom_spectrum(void)
{
    if (s_zoom_decim <= 1) return NULL;
    int idx = s_zoom_ready_idx;
    if (idx < 0) return NULL;
    return s_zoom_spectrum[idx];
}

static void fft_task(void *arg)
{
    // Local scratch buffers
    static int16_t samples[DSP_FFT_SIZE * 2];   // 1024 stereo pairs = 2048 int16
    static float tmp_spectrum[DSP_FFT_SIZE];     // dB output, local before copying

    // Track most recent stats for periodic logging
    float last_min = 0, last_max = 0, last_mean = 0;

    while (1) {
        // Block until we have a full FFT window of stereo pairs (1024 pairs).
        // audio_read_samples may return less than requested - loop until full.
        size_t got = 0;
        while (got < DSP_FFT_SIZE) {
            size_t want = DSP_FFT_SIZE - got;
            size_t r = audio_read_samples(&samples[got * 2], want, 50);
            if (r == 0) {
                // No data yet (audio not streaming) - keep waiting
                continue;
            }
            got += r;
        }

        // Step 3 v0.10: FT8 capture branch. fs/4 sign-flip mixer
        // shifts QMX +12 kHz IF to DC, then /4 FIR decimator.
        // Skip DC blocker / FFT / render push during capture.
        if (s_ft8_active) {
            static int64_t s_prev_work_end_us = 0;
            int64_t t_read_done = esp_timer_get_time();
            if (s_prev_work_end_us != 0) {
                int64_t dt = t_read_done - s_prev_work_end_us;
                s_ft8_read_us += dt;
                if (dt > 30000) s_ft8_slow_reads++;
            }
            for (int i = 0; i < DSP_FFT_SIZE; i += 4) {
                s_ft8_mix_buf[i + 0] =  (float)samples[2*(i+0)];      // +I
                s_ft8_mix_buf[i + 1] =  (float)samples[2*(i+1) + 1];  // +Q
                s_ft8_mix_buf[i + 2] = -(float)samples[2*(i+2)];      // -I
                s_ft8_mix_buf[i + 3] = -(float)samples[2*(i+3) + 1];  // -Q
            }
            // NB: dsps_fird_f32's `len` is OUTPUT length, not input.
            // For decim=4 and 1024 inputs, request 256 outputs.
            int n_out = dsps_fird_f32(&s_ft8_fir,
                                      s_ft8_mix_buf,
                                      s_ft8_dec_buf,
                                      DSP_FFT_SIZE / 4);
            int writable = s_ft8_target - s_ft8_idx;
            if (writable > 0 && n_out > 0) {
                int copy = (n_out < writable) ? n_out : writable;
                memcpy(&s_ft8_dst[s_ft8_idx],
                       s_ft8_dec_buf,
                       copy * sizeof(float));
                s_ft8_idx += copy;
            }
            if (s_ft8_idx >= s_ft8_target) {
                xSemaphoreGive(s_ft8_done_sem);
            }
            int64_t t_work_end = esp_timer_get_time();
            int64_t work_dt = t_work_end - t_read_done;
            s_ft8_work_us += work_dt;
            if (work_dt > 10000) s_ft8_slow_works++;
            s_prev_work_end_us = t_work_end;
            s_ft8_iter_count++;
            s_ft8_total_n_out += n_out;
            int64_t now = t_work_end;
            if (now - s_ft8_last_log_us > 1000000) {
                uint32_t n = s_ft8_iter_count ? s_ft8_iter_count : 1;
                ESP_LOGI(TAG,
                    "FT8 br: %u iters %u smp idx=%d/%d  read_avg=%uus work_avg=%uus  slow_r=%u slow_w=%u",
                    (unsigned)s_ft8_iter_count,
                    (unsigned)s_ft8_total_n_out,
                    s_ft8_idx, s_ft8_target,
                    (unsigned)(s_ft8_read_us / n),
                    (unsigned)(s_ft8_work_us / n),
                    (unsigned)s_ft8_slow_reads,
                    (unsigned)s_ft8_slow_works);
                s_ft8_iter_count = 0;
                s_ft8_total_n_out = 0;
                s_ft8_read_us = 0;
                s_ft8_work_us = 0;
                s_ft8_slow_reads = 0;
                s_ft8_slow_works = 0;
                s_ft8_last_log_us = now;
            }
            // S-meter still needs fresh spectrum data during active FT8
            // capture (s_ft8_active stays true almost continuously per the
            // "no slot-skip" design, so the idle-branch refresh below rarely
            // runs). samples[] here is the raw, un-mixed IQ (the fs/4 mixer
            // above writes to s_ft8_mix_buf, not samples[]), so the spectrum
            // stays IF-bin-aligned for dsp_get_peak_dbm_around_vfo(vfo_bin).
            {
                static int s_ft8_active_smeter_tick = 0;
                if (++s_ft8_active_smeter_tick >= 10) {
                    s_ft8_active_smeter_tick = 0;
                    compute_and_publish_spectrum(samples, tmp_spectrum, &last_min, &last_max, &last_mean);
                }
            }
            continue;
        }
        // Step 4b v0.10: FT8 mode but no capture armed - drain and
        // discard. We MUST still consume samples here; audio.c is
        // single-consumer and a stalled fft_task would back-pressure
        // the ring buffer and corrupt the next capture.
        //
        // S-meter still needs fresh spectrum data while in FT8 mode (it's
        // read from s_spectrum at 5 Hz by render_task), so every ~10
        // iterations (~213 ms @ 48 kHz/1024) run the FFT below instead of
        // discarding. The resulting spectrum/waterfall push in render_task
        // is harmless - the panadapter canvases aren't visible in FT8 mode.
        if (ui_mode_get() == UI_MODE_FT8) {
            static int s_ft8_smeter_tick = 0;
            if (++s_ft8_smeter_tick < 10) {
                continue;
            }
            s_ft8_smeter_tick = 0;
        }
        // DC blocker, windowing, FFT, dB conversion, publish to s_spectrum
        // (normal panadapter path).
        compute_and_publish_spectrum(samples, tmp_spectrum, &last_min, &last_max, &last_mean);

        // v0.16.0 zoom-FFT: mix/decimate/accumulate toward a second,
        // higher-resolution FFT centered on the pan target. No-op when
        // zoom <= x2 (s_zoom_decim == 1). Reuses `samples`, which
        // compute_and_publish_spectrum already DC-blocked in place.
        zoom_process(samples);
    }
}