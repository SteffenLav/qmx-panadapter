#include "spiffs_guard.h"

#include "esp_spiffs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "panic_hook.h"   // T138: a gc runs with the flash cache off

static const char *TAG = "spiffs_guard";
static SemaphoreHandle_t s_mx;

void spiffs_guard_init(void)
{
    if (!s_mx) s_mx = xSemaphoreCreateRecursiveMutex();
    if (!s_mx) ESP_LOGE(TAG, "no memory for the SPIFFS write guard");
}

// A missing mutex means init has not run: only one task exists then, so
// running unguarded cannot race anything (same reasoning as adif_log.c's s_lock).
void spiffs_guard_take(void) { if (s_mx) xSemaphoreTakeRecursive(s_mx, portMAX_DELAY); }
void spiffs_guard_give(void) { if (s_mx) xSemaphoreGiveRecursive(s_mx); }

bool spiffs_guard_reserve(size_t need)
{
    panic_hook_crumb_enter("spiffs:reserve-gc");
    esp_err_t r = esp_spiffs_gc("storage", need + SPIFFS_GUARD_MARGIN);
    panic_hook_crumb_exit();
    if (r != ESP_OK)
        ESP_LOGW(TAG, "cannot guarantee %u B on storage (%s) - write refused",
                 (unsigned)(need + SPIFFS_GUARD_MARGIN), esp_err_to_name(r));
    return r == ESP_OK;
}
