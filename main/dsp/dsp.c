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
static int    s_ft8_idx    = 0;
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

esp_err_t dsp_ft8_capture(float *dst_180000, uint32_t timeout_ms)
{
    if (!dst_180000) return ESP_ERR_INVALID_ARG;
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
    s_ft8_dst    = dst_180000;
    s_ft8_idx    = 0;
    s_ft8_target = 180000;
    __sync_synchronize();
    s_ft8_active = true;
    BaseType_t ok = xSemaphoreTake(s_ft8_done_sem, pdMS_TO_TICKS(timeout_ms));
    s_ft8_active = false;
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "FT8 capture timeout after %lu ms (got %d/%d samples)",
                 (unsigned long)timeout_ms, s_ft8_idx, s_ft8_target);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
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

// Phase 5.10D: peak dBm in a window around bin DSP_FFT_SIZE/2 (the VFO center).
// We treat the spectrum as it sits in s_spectrum which is in FFT-natural order;
// the center bin of the displayed spectrum is at index DSP_FFT_SIZE/2 after
// the fftshift performed in ui_push_spectrum. However s_spectrum here is the
// raw post-FFT array. To be consistent with the displayed center we use the
// DC bin (index 0) plus the negative-frequency half, equivalent to the center
// of the shifted spectrum being bin 0.  Concretely: bin 0 is DC = VFO.
esp_err_t dsp_get_peak_dbm_around_vfo(int half_width_bins, float *peak_dbm)
{
    if (!s_spectrum_mtx || !peak_dbm) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_spectrum_mtx, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_have_spectrum) {
        xSemaphoreGive(s_spectrum_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    // VFO = bin 0 (DC) in s_spectrum (pre-shift order).
    // Search positive bins 0..half and negative bins -half..-1 (mapped to
    // DSP_FFT_SIZE - half ... DSP_FFT_SIZE - 1).
    if (half_width_bins <= 0) half_width_bins = 1;
    if (half_width_bins > DSP_FFT_SIZE / 2) half_width_bins = DSP_FFT_SIZE / 2;
    float peak = -1e9f;
    for (int i = 0; i <= half_width_bins; i++) {
        if (s_spectrum[i] > peak) peak = s_spectrum[i];
    }
    for (int i = DSP_FFT_SIZE - half_width_bins; i < DSP_FFT_SIZE; i++) {
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
    float peak_db = -1e9f;
    int   peak_bin = center_bin;
    float sum_db = 0.0f;
    int   count = 0;
    for (int d = -radius_bins; d <= radius_bins; d++) {
        int b = ((center_bin + d) % N + N) % N;  // positive modulo
        float v = s_spectrum[b];
        sum_db += v;
        count++;
        if (v > peak_db) { peak_db = v; peak_bin = b; }
    }
    float mean_db = (count > 0) ? (sum_db / (float)count) : -120.0f;
    xSemaphoreGive(s_spectrum_mtx);

    // Only snap if the peak is meaningfully above local mean (avoids snapping to noise).
    if (peak_db - mean_db < 3.0f) {
        *out_hz = center_hz;
        return ESP_OK;
    }

    // Unwrap peak_bin back into a signed bin offset (-N/2 .. +N/2), then to Hz from dial.
    int signed_bin = peak_bin;
    if (signed_bin >= N / 2) signed_bin -= N;
    int32_t peak_hz_baseband = (int32_t)(signed_bin * bin_width + 0.5f * (signed_bin >= 0 ? 1.0f : -1.0f));
    *out_hz = peak_hz_baseband - 12000;
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
            continue;
        }
        // Step 4b v0.10: FT8 mode but no capture armed - drain and
        // discard. We MUST still consume samples here; audio.c is
        // single-consumer and a stalled fft_task would back-pressure
        // the ring buffer and corrupt the next capture.
        if (ui_mode_get() == UI_MODE_FT8) {
            continue;
        }
        // DC blocker (Phase 5.6): one-pole IIR per QUISK sound.c / Lyons UDSP 3rd ed. p.762
        // ~100 Hz high-pass at Fs=48 kHz; removes any slow drift on I and Q channels.
        // QMX has 12 kHz IF so there is no useful signal at DC anyway.
#if DSP_DC_BLOCKER
        {
            static float dc_state_I = 0.0f;
            static float dc_state_Q = 0.0f;
            const float alpha = 0.9869f;  // omega = pi * 100 / (48000/2) -> alpha per QUISK formula
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
        // Apply window and pack into interleaved complex (I=real, Q=imag).
        // samples[] is interleaved L,R,L,R,... where L = I, R = Q.
        for (int i = 0; i < DSP_FFT_SIZE; i++) {
            float I = (float)samples[2*i];
            float Q = (float)samples[2*i + 1];
            float w = s_window[i];
            s_workbuf[2*i]     = I * w;
            s_workbuf[2*i + 1] = Q * w;
        }

        // FFT in place
        dsps_fft2r_fc32(s_workbuf, DSP_FFT_SIZE);
        dsps_bit_rev_fc32(s_workbuf, DSP_FFT_SIZE);

        // Magnitude² -> dB. Use 10*log10 since we keep mag squared.
        // Floor to -120 dB to avoid log of zero on totally empty bins.
        const float floor_mag2 = 1e-12f;  // ~-120 dB given normalized scale
        float sum_db = 0.0f;
        float min_db = +1e9f;
        float max_db = -1e9f;
        for (int i = 0; i < DSP_FFT_SIZE; i++) {
            float re = s_workbuf[2*i];
            float im = s_workbuf[2*i + 1];
            float mag2 = re*re + im*im;
            if (mag2 < floor_mag2) mag2 = floor_mag2;
            // Normalize by FFT size and window gain factor to get repeatable
            // dB scale across different input amplitudes / FFT sizes.
            // For 16-bit input max=32768, mag2 max ~ (32768 * N * windowGain)^2
            // We just take 10*log10 directly here; scaling can be tuned later.
            float db = 10.0f * log10f(mag2) + DSP_DB_CALIBRATION_OFFSET;
            tmp_spectrum[i] = db;
            sum_db += db;
            if (db < min_db) min_db = db;
            if (db > max_db) max_db = db;
        }
        float mean_db = sum_db / (float)DSP_FFT_SIZE;
        last_min = min_db;
        last_max = max_db;
        last_mean = mean_db;


        // Publish under mutex
        if (xSemaphoreTake(s_spectrum_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
            memcpy(s_spectrum, tmp_spectrum, DSP_FFT_SIZE * sizeof(float));
            s_have_spectrum = true;
            xSemaphoreGive(s_spectrum_mtx);
        }

        s_frames_this_period++;
        log_stats(last_min, last_max, last_mean);
    }
}