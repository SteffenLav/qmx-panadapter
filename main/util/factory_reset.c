#include "factory_reset.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "factory_reset";

// RTC_NOINIT_ATTR survives esp_restart() (RTC RAM is retained across a soft
// reboot) but is NOT zero-initialised, so after a real cold power-on it holds
// garbage. The magic word guards against that: only a value written by
// factory_reset_request() this power-cycle is honoured. A 1-in-2^32 chance of
// garbage matching the magic once is acceptable (worst case: one spurious
// settings reset, recoverable by re-entering settings).
RTC_NOINIT_ATTR static uint32_t s_reset_magic;
RTC_NOINIT_ATTR static uint32_t s_reset_flags;

#define RESET_MAGIC     0x0FACE5E7u   // "factory reset" sentinel
#define FLAG_SETTINGS   (1u << 0)     // erase user_nvs (app settings)
#define FLAG_NETWORK    (1u << 1)     // erase default nvs (WiFi/system)

void factory_reset_apply_pending(void)
{
    if (s_reset_magic != RESET_MAGIC) {
        return;  // normal boot — nothing pending
    }
    uint32_t flags = s_reset_flags;
    // Consume the request immediately so a crash mid-erase can't wedge us into
    // an erase-reboot loop.
    s_reset_magic = 0;
    s_reset_flags = 0;

    if (flags & FLAG_SETTINGS) {
        // "user_nvs" holds all app settings + memory channels. Erasing the
        // whole partition (vs one namespace) is the clean-slate the user wants,
        // and it leaves the ADIF QSO log (SPIFFS "storage") untouched.
        esp_err_t err = nvs_flash_erase_partition("user_nvs");
        ESP_LOGW(TAG, "SETTINGS reset: erased user_nvs -> 0x%x", err);
    }
    if (flags & FLAG_NETWORK) {
        // Default "nvs" partition: WiFi credentials, PHY calibration, and any
        // esp_hosted/system state. This is what clears a stuck WiFi/link state
        // that a normal reflash leaves in place.
        esp_err_t err = nvs_flash_erase();
        ESP_LOGW(TAG, "NETWORK reset: erased default nvs -> 0x%x", err);
    }
    // Fall through into the normal boot: the following nvs_flash_init() /
    // settings_init() will re-format the blank partition(s) and load defaults.
}

static void reboot_task(void *arg)
{
    (void)arg;
    // Give the HTTP handler's response time to flush over WiFi before we pull
    // the rug out. The request came in over the same link we're about to reset.
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "rebooting to apply factory reset (flags=0x%lx)",
             (unsigned long)s_reset_flags);
    esp_restart();
}

void factory_reset_request(bool reset_settings, bool reset_network)
{
    if (!reset_settings && !reset_network) {
        return;  // nothing to do
    }
    s_reset_flags = (reset_settings ? FLAG_SETTINGS : 0) |
                    (reset_network  ? FLAG_NETWORK  : 0);
    s_reset_magic = RESET_MAGIC;
    ESP_LOGW(TAG, "factory reset requested (settings=%d network=%d) — rebooting",
             reset_settings, reset_network);
    xTaskCreate(reboot_task, "reset_reboot", 2048, NULL, 6, NULL);
}
