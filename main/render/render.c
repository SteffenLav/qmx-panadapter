#include "render.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "dsp.h"
#include "ui.h"
#include "render_waterfall.h"
#include <string.h>

static const char *TAG = "render";

// Render at ~30 Hz. FFT produces ~48 Hz; we naturally downsample.
#define RENDER_PERIOD_MS  33

static TaskHandle_t s_render_task = NULL;
static float *s_scratch = NULL;

// Phase 5.4: smoothing (EMA per bin, alpha=0.4)
static float *s_smoothed = NULL;
static bool s_smoothed_init = false;
#define SMOOTH_ALPHA  0.4f

// Phase 5.4: autoscale (update display dB range once per second)
#define AUTOSCALE_PERIOD_MS  1000
static int64_t s_last_autoscale_ms = 0;

// Helper: approximate median via partial quickselect-style scan.
// Cheap and good enough for noise-floor estimation.
static float approx_median(const float *arr, int n)
{
    // Sample N bins evenly, sort, return middle.
    // For DSP_FFT_SIZE=1024, sampling 64 bins is plenty.
    #define MED_SAMPLES 64
    float samples[MED_SAMPLES];
    int stride = n / MED_SAMPLES;
    if (stride < 1) stride = 1;
    int collected = 0;
    for (int i = 0; i < n && collected < MED_SAMPLES; i += stride) {
        samples[collected++] = arr[i];
    }
    // Simple bubble sort — only 64 elements, runs once per second.
    for (int i = 0; i < collected - 1; i++) {
        for (int j = 0; j < collected - 1 - i; j++) {
            if (samples[j] > samples[j+1]) {
                float t = samples[j]; samples[j] = samples[j+1]; samples[j+1] = t;
            }
        }
    }
    return samples[collected / 2];
}

static void autoscale_update(const float *spectrum, int n)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_autoscale_ms < AUTOSCALE_PERIOD_MS) return;
    s_last_autoscale_ms = now_ms;

    float noise_floor = approx_median(spectrum, n);
    float max_db = spectrum[0];
    for (int i = 1; i < n; i++) {
        if (spectrum[i] > max_db) max_db = spectrum[i];
    }
    // Floor a few dB below noise, top a few dB above strongest signal,
    // with sensible bounds so the range never collapses or explodes.
    float new_min = noise_floor - 10.0f;
    float new_max = max_db + 5.0f;
    if (new_max - new_min < 40.0f) new_max = new_min + 40.0f;   // min 40 dB span
    if (new_max - new_min > 120.0f) new_max = new_min + 120.0f; // max 120 dB span
    ui_set_db_range(new_min, new_max);
    ui_set_db_labels(new_min, new_max);
}

static void render_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RENDER_PERIOD_MS));

        esp_err_t err = dsp_get_spectrum(s_scratch);
        if (err == ESP_OK) {
            // Phase 5.4: EMA smoothing
            if (!s_smoothed_init) {
                // First frame: initialize smoothed with current values (no fade-in)
                memcpy(s_smoothed, s_scratch, DSP_FFT_SIZE * sizeof(float));
                s_smoothed_init = true;
            } else {
                for (int i = 0; i < DSP_FFT_SIZE; i++) {
                    s_smoothed[i] = SMOOTH_ALPHA * s_scratch[i]
                                  + (1.0f - SMOOTH_ALPHA) * s_smoothed[i];
                }
            }

            // Phase 5.4: autoscale display range (1 Hz update)
            autoscale_update(s_smoothed, DSP_FFT_SIZE);

            // Push smoothed spectrum to UI and waterfall
            ui_push_spectrum(s_smoothed, DSP_FFT_SIZE);
            render_waterfall_tick(s_smoothed, DSP_FFT_SIZE);
        }
        // ESP_ERR_NOT_FOUND just means no spectrum yet (no audio); skip silently.
    }
}

esp_err_t render_init(void)
{
    ESP_LOGI(TAG, "Render init (Phase 5.4 - smoothed spectrum + waterfall at %d Hz)",
             1000 / RENDER_PERIOD_MS);

    // Scratch buffer in PSRAM, accessed once per frame
    s_scratch = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_scratch) {
        ESP_LOGE(TAG, "Failed to alloc render scratch buffer");
        return ESP_ERR_NO_MEM;
    }
    // Phase 5.4: smoothing buffer (internal RAM for fast access)
    s_smoothed = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_smoothed) {
        ESP_LOGE(TAG, "Failed to alloc smoothing buffer");
        return ESP_ERR_NO_MEM;
    }
    s_smoothed_init = false;
    esp_err_t wferr = render_waterfall_init();
    if (wferr != ESP_OK) {
        return wferr;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(
        render_task, "render", 4096, NULL, 3, &s_render_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create render task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Render task started");
    return ESP_OK;
}










