#include "sdio_ready.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdio_rdy";

// Plain volatile bool rather than an event group: it is written exactly once,
// from the SDIO read task, and polled by one waiter on a 25 ms step. A sync
// primitive here would have to be created before either side runs, which is
// the ordering problem this file exists to avoid.
static volatile bool s_ready = false;

void qmx_sdio_card_ready_set(void)
{
    if (s_ready) return;
    s_ready = true;
    ESP_LOGW(TAG, "SDIO card init finished - SD mount may now take its DMA");
}

bool qmx_sdio_card_ready(void) { return s_ready; }

bool qmx_sdio_card_ready_wait(uint32_t timeout_ms)
{
    const uint32_t step = 25;
    uint32_t waited = 0;
    int64_t t0 = esp_timer_get_time();
    while (!s_ready && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (s_ready) {
        ESP_LOGI(TAG, "waited %d ms for the SDIO card init", ms);
        return true;
    }
    // Not an error. WiFi may be off, the sdio_drv.c patch may be missing after
    // a fullclean, or the init may have failed - and none of those should mean
    // "never mount the SD card".
    ESP_LOGW(TAG, "SDIO card init did not signal within %u ms - mounting the SD "
                  "card anyway (WiFi off, patch missing, or init failed)",
             (unsigned)timeout_ms);
    return false;
}
