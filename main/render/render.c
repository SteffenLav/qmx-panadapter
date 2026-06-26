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

// Render at 10 Hz. Higher rates cause LVGL flush cascades on this hardware
// (PSRAM ~30 MB/s vs 1280x720 RGB565 framebuffer); 10 Hz gives LVGL clean
// breathing room between iterations and keeps unlock cost stable ~26 ms.
#define RENDER_PERIOD_MS  100

static TaskHandle_t s_render_task = NULL;
static float *s_scratch = NULL;

// Phase 5.4: smoothing (EMA per bin, alpha=0.4)
static float *s_smoothed = NULL;
static bool s_smoothed_init = false;
// Phase 5.10D Stage 2: runtime-adjustable EMA smoothing
static float s_ema_alpha = 0.4f;

// v0.16.0: separate, more heavily smoothed spectrum fed only to the
// waterfall. The spectrum trace wants responsiveness (alpha 0.4), but the
// waterfall's per-bin noise floor tracker compares against a single frame -
// with alpha 0.4 the frame-to-frame variance of pure noise routinely
// exceeds the floor+6dB threshold, so noise bins light up as speckle even
// when the floor tracking itself is correct. A slower EMA here reduces that
// per-frame variance without affecting the spectrum trace's responsiveness.
#define WF_EMA_ALPHA 0.15f
static float *s_wf_smoothed = NULL;
static bool s_wf_smoothed_init = false;

void render_set_ema_alpha(float alpha)
{
    if (alpha < 0.05f) alpha = 0.05f;
    if (alpha > 1.0f) alpha = 1.0f;
    s_ema_alpha = alpha;
    ESP_LOGI("render", "EMA alpha = %.2f", (double)alpha);
}

static volatile bool s_wf_2x = false;

void render_set_waterfall_2x(bool on)
{
    s_wf_2x = on;
    ESP_LOGI("render", "waterfall 3x speed: %s", on ? "on" : "off");
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


            // Push smoothed spectrum to UI
            ui_push_spectrum(s_smoothed, DSP_FFT_SIZE);

            // Separate, more heavily smoothed spectrum for the waterfall only
            if (!s_wf_smoothed_init) {
                memcpy(s_wf_smoothed, s_scratch, DSP_FFT_SIZE * sizeof(float));
                s_wf_smoothed_init = true;
            } else {
                for (int i = 0; i < DSP_FFT_SIZE; i++) {
                    s_wf_smoothed[i] = WF_EMA_ALPHA * s_scratch[i]
                                     + (1.0f - WF_EMA_ALPHA) * s_wf_smoothed[i];
                }
            }

            // Phase 5.10D: sample S-meter at ~5 Hz from spectrum peak around VFO
            {
                static int s_smeter_tick = 0;
                s_smeter_tick++;
                if (s_smeter_tick >= 6) {  // 30 Hz / 6 = 5 Hz
                    s_smeter_tick = 0;
                    float peak_dbm;
                    int vfo_bin = ((ui_get_if_bin_shift(DSP_FFT_SIZE) % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE;
                    if (dsp_get_peak_dbm_around_vfo(vfo_bin, 64, &peak_dbm) == ESP_OK) {
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
            render_waterfall_tick(s_wf_smoothed, DSP_FFT_SIZE);
            // Fast mode: push 2 more rows immediately (same spectrum content -
            // we don't have a fresher sample within this period) for 3x total
            // waterfall scroll speed, without touching the spectrum/S-meter
            // cadence above, which stays at the normal 10 Hz.
            if (s_wf_2x) {
                render_waterfall_tick(s_wf_smoothed, DSP_FFT_SIZE);
                render_waterfall_tick(s_wf_smoothed, DSP_FFT_SIZE);
            }
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

    s_wf_smoothed = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_wf_smoothed) {
        ESP_LOGE(TAG, "Failed to alloc waterfall smoothing buffer");
        return ESP_ERR_NO_MEM;
    }
    s_wf_smoothed_init = false;

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











