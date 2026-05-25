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
// Phase 5.10D Stage 2: runtime-adjustable EMA smoothing
static float s_ema_alpha = 0.4f;

void render_set_ema_alpha(float alpha)
{
    if (alpha < 0.05f) alpha = 0.05f;
    if (alpha > 1.0f) alpha = 1.0f;
    s_ema_alpha = alpha;
    ESP_LOGI("render", "EMA alpha = %.2f", (double)alpha);
}



// Phase 5.5: autoscale removed — static Ref/Range, manual control


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
                    s_smoothed[i] = s_ema_alpha * s_scratch[i]
                                  + (1.0f - s_ema_alpha) * s_smoothed[i];
                }
            }


            // Push smoothed spectrum to UI and waterfall
            ui_push_spectrum(s_smoothed, DSP_FFT_SIZE);

            // Phase 5.10D: sample S-meter at ~5 Hz from spectrum peak around VFO
            {
                static int s_smeter_tick = 0;
                s_smeter_tick++;
                if (s_smeter_tick >= 6) {  // 30 Hz / 6 = 5 Hz
                    s_smeter_tick = 0;
                    float peak_dbm;
                    if (dsp_get_peak_dbm_around_vfo(64, &peak_dbm) == ESP_OK) {
                        // S-unit conversion: S9 = -73 dBm, 6 dB per S-unit below.
                        // Above S9, we use S9+xx where xx = dbm - (-73).
                        int s_units;
                        if (peak_dbm >= -73.0f) {
                            s_units = 9 + (int)((peak_dbm + 73.0f) + 0.5f);
                        } else {
                            s_units = 9 + (int)((peak_dbm + 73.0f) / 6.0f + 0.5f);
                            if (s_units < 0) s_units = 0;
                        }
                        ui_update_smeter(s_units);
                    }
                }
            }
            render_waterfall_tick(s_smoothed, DSP_FFT_SIZE);
        }
        // ESP_ERR_NOT_FOUND just means no spectrum yet (no audio); skip silently.
    }
}

esp_err_t render_init(void)
{
    ESP_LOGI(TAG, "Render init (Phase 5.5 - static scale, smoothed spectrum at %d Hz)",
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











