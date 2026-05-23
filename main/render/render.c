#include "render.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "dsp.h"
#include "ui.h"
#include "render_waterfall.h"

static const char *TAG = "render";

// Render at ~30 Hz. FFT produces ~48 Hz; we naturally downsample.
#define RENDER_PERIOD_MS  33

static TaskHandle_t s_render_task = NULL;
static float *s_scratch = NULL;

static void render_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RENDER_PERIOD_MS));

        esp_err_t err = dsp_get_spectrum(s_scratch);
        if (err == ESP_OK) {
            ui_push_spectrum(s_scratch, DSP_FFT_SIZE);
            render_waterfall_tick(s_scratch, DSP_FFT_SIZE);
        }
        // ESP_ERR_NOT_FOUND just means no spectrum yet (no audio); skip silently.
    }
}

esp_err_t render_init(void)
{
    ESP_LOGI(TAG, "Render init (Phase 5.2 - spectrum + waterfall at %d Hz)",
             1000 / RENDER_PERIOD_MS);

    // Scratch buffer in PSRAM, accessed once per frame
    s_scratch = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_scratch) {
        ESP_LOGE(TAG, "Failed to alloc render scratch buffer");
        return ESP_ERR_NO_MEM;
    }
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







