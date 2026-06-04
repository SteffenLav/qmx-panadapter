#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ft8_test.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/m5stack_tab5.h"

#include "display.h"
#include "ui.h"
#include "status.h"
#include "battery.h"
#include "bsp_info.h"
#include "cat.h"
#include "audio.h"
#include "dsp.h"
#include "render.h"
#include "render_waterfall.h"
#include "settings.h"
#include "wifi.h"
#include "iq_balance.h"
#include "ui_mode.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "QMX+ Panadapter starting");
    ESP_LOGI(TAG, "PSRAM total: %zu MB",
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024 * 1024));
    // Initialise NVS (settings persistence). If the partition is full or
    // a new version invalidated it, erase and retry - never block boot.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (0x%x); erasing and retrying", nvs_err);
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: 0x%x - settings will not persist", nvs_err);
    } else {
        ESP_LOGI(TAG, "NVS initialised");
    }

    settings_init();
    qmx_settings_t cfg;
    settings_load_all(&cfg);

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));

    bsp_info_log();

    // Enable battery charging (BSP defines these but never calls them)
    bsp_set_charge_qc_en(true);
    bsp_set_charge_en(true);

    // Initialise INA226 battery monitor (shares main I2C bus with PI4IO)
    battery_init(bsp_i2c_get_handle());

    ui_init(disp);

    // Apply persisted settings to UI / render pipeline.
    ui_set_db_range(cfg.db_min, cfg.db_max);
    ui_set_db_labels(cfg.db_min, cfg.db_max);
    render_set_ema_alpha(cfg.ema_alpha);
    status_bar_start();

    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true));
    ESP_LOGI(TAG, "USB host started");

    ESP_ERROR_CHECK(audio_init());
    iq_balance_set_enabled(cfg.iq_enabled);
    ui_set_flat_mode(cfg.flat_mode);
    ui_set_cw_pitch_hz(cfg.cw_pitch_hz);
    render_waterfall_set_colormap(cfg.colormap_idx);

    // Restore last-known VFO frequency (display only; QMX is source of truth).
    if (cfg.last_vfo_hz != 0) {
        ESP_LOGI(TAG, "Restored last VFO: %lu Hz", (unsigned long)cfg.last_vfo_hz);
        ui_update_frequency(cfg.last_vfo_hz);
    } else {
        ESP_LOGI(TAG, "No stored VFO (first boot or cleared NVS)");
    }
    ESP_ERROR_CHECK(cat_init());

    // WiFi+SNTP runs in a background task; doesn't block boot.
    // DISABLED pending C6 firmware investigation (see CLAUDE.md / git log).
    panadapter_wifi_start();
    ESP_ERROR_CHECK(dsp_init());
    ESP_ERROR_CHECK(render_init());

    ESP_LOGI(TAG, "Init complete - main task idle");
    // Spawn FT8 self-test on a dedicated task (32 KB stack, core 1).
    // Verifies ft8_lib encoder + monitor + decoder work on ESP32-P4.
    // Logs PASS/FAIL with per-stage timing once the worker completes.
    // Step 4b v0.10: boot directly into FT8 mode so the existing
    // flash-and-watch decode flow keeps working. Step 4c will let
    // the user toggle from the settings drawer.
    ui_mode_set(UI_MODE_FT8);
    ft8_self_test();
}

