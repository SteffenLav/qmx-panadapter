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

#include "audio.h"

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