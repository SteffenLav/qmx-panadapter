#include "bsp_info.h"

#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "hal/efuse_hal.h"
#include "driver/i2c_master.h"
#include "bsp/m5stack_tab5.h"


static const char *TAG = "bsp_info";

#define ST7123_TOUCH_ADDR       (0x55)
#define GT911_TOUCH_ADDR        (0x5D)
#define GT911_TOUCH_ADDR_ALT    (0x14)

#define I2C_PROBE_TIMEOUT_MS    (20)

void bsp_info_log(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t rev   = efuse_hal_chip_revision();
    uint32_t major = rev / 100;
    uint32_t minor = rev % 100;
    const char *family = (chip.model == CHIP_ESP32P4) ? "ESP32-P4" : "unknown";

    size_t psram_bytes   = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // Panel/touch names come from the BSP's cached detection result (which
    // read the touch FW-version register during display bring-up) — NOT from
    // an address probe. ST7121 and ST7123 share I2C 0x55, so a bare probe
    // cannot tell them apart; this block used to hardcode "ST7123" for every
    // unit answering at 0x55, contradicting the driver's own ST7121 detection
    // two lines up in the same boot log (reported by Paul VE3PIK, v1.2.0).
    // The probe below is kept purely to report WHICH ADDRESS answered.
    const char *touch_name = bsp_touch_controller_name();
    const char *panel_name = bsp_display_panel_name();
    uint8_t     touch_addr = 0;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus != NULL) {
        if (i2c_master_probe(bus, ST7123_TOUCH_ADDR, I2C_PROBE_TIMEOUT_MS) == ESP_OK) {
            touch_addr = ST7123_TOUCH_ADDR;
        } else if (i2c_master_probe(bus, GT911_TOUCH_ADDR, I2C_PROBE_TIMEOUT_MS) == ESP_OK) {
            touch_addr = GT911_TOUCH_ADDR;
        } else if (i2c_master_probe(bus, GT911_TOUCH_ADDR_ALT, I2C_PROBE_TIMEOUT_MS) == ESP_OK) {
            touch_addr = GT911_TOUCH_ADDR_ALT;
        }
    }

    ESP_LOGI(TAG, "=== TAB5 BSP INFO ===");
    ESP_LOGI(TAG, "chip:     %s rev v%u.%u", family, (unsigned)major, (unsigned)minor);
    ESP_LOGI(TAG, "psram:    %u MB", (unsigned)(psram_bytes / (1024u * 1024u)));
    ESP_LOGI(TAG, "panel:    %s (from hardware detection)", panel_name);
    if (touch_addr != 0) {
        ESP_LOGI(TAG, "touch:    %s @ 0x%02X", touch_name, touch_addr);
    } else {
        ESP_LOGI(TAG, "touch:    unknown (no I2C ACK at 0x55/0x5D/0x14)");
    }
    ESP_LOGI(TAG, "heap:     %.1f kB internal free, %.2f MB PSRAM free",
             free_internal / 1024.0f, free_psram / (1024.0f * 1024.0f));
    ESP_LOGI(TAG, "idf:      %s", esp_get_idf_version());
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "firmware: %s", app ? app->version : "unknown");
    ESP_LOGI(TAG, "=====================");
}