#include "dsp.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "dsps_fft2r.h"
#include "dsps_wind_blackman_harris.h"

static const char *TAG = "dsp";

// Buffers
// - s_window: Blackman-Harris window coefficients, one per stereo PAIR
//             (length = DSP_FFT_SIZE, applied identically to I and Q)
// - s_workbuf: interleaved real/imag input/output for in-place FFT
//              (length = DSP_FFT_SIZE * 2 floats)
// - s_spectrum: dB magnitude per bin, snapshot output
//               (length = DSP_FFT_SIZE floats)
static float *s_window   = NULL;
static float *s_workbuf  = NULL;
static float *s_spectrum = NULL;

static SemaphoreHandle_t s_spectrum_mtx = NULL;
static bool s_have_spectrum = false;

esp_err_t dsp_init(void)
{
    ESP_LOGI(TAG, "DSP init (Phase 4.1 - FFT primitive bring-up, %d-pt complex)",
             DSP_FFT_SIZE);

    // Allocate buffers in regular RAM (faster than PSRAM for math-heavy loops)
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

    // Precompute window
    dsps_wind_blackman_harris_f32(s_window, DSP_FFT_SIZE);
    ESP_LOGI(TAG, "Blackman-Harris window computed (sum first 4: %.4f %.4f %.4f %.4f)",
             s_window[0], s_window[1], s_window[2], s_window[3]);

    // Initialize esp-dsp FFT engine
    esp_err_t err = dsps_fft2r_init_fc32(NULL, DSP_FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: 0x%x (%s)",
                 err, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "esp-dsp FFT initialized for %d-pt complex", DSP_FFT_SIZE);

    s_spectrum_mtx = xSemaphoreCreateMutex();
    if (!s_spectrum_mtx) return ESP_ERR_NO_MEM;

    // ---- Self-test: synthetic sine at bin 100, verify FFT detects it ----
    // Fill workbuf with a complex tone: re = cos(2*pi*100*n/N), im = sin(...)
    const int test_bin = 100;
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float phase = 2.0f * (float)M_PI * (float)test_bin * (float)i
                      / (float)DSP_FFT_SIZE;
        s_workbuf[2*i]     = cosf(phase) * s_window[i]; // real
        s_workbuf[2*i + 1] = sinf(phase) * s_window[i]; // imag
    }

    int64_t t0 = esp_log_timestamp();
    dsps_fft2r_fc32(s_workbuf, DSP_FFT_SIZE);
    dsps_bit_rev_fc32(s_workbuf, DSP_FFT_SIZE);
    int64_t t1 = esp_log_timestamp();

    // Find peak bin
    float peak_mag = 0.0f;
    int peak_idx = -1;
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        float re = s_workbuf[2*i];
        float im = s_workbuf[2*i + 1];
        float mag = re*re + im*im;
        if (mag > peak_mag) { peak_mag = mag; peak_idx = i; }
    }
    ESP_LOGI(TAG, "Self-test: synthetic tone at bin %d -> FFT peak at bin %d "
                  "(magnitude=%.1f), FFT took %lld ms",
             test_bin, peak_idx, sqrtf(peak_mag), (long long)(t1 - t0));

    if (peak_idx != test_bin) {
        ESP_LOGW(TAG, "Peak bin mismatch! Expected %d, got %d. "
                      "Check FFT direction / convention.",
                 test_bin, peak_idx);
    } else {
        ESP_LOGI(TAG, "Self-test PASSED");
    }

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
