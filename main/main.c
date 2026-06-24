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
#include "cw_audio.h"
#include "dsp.h"
#include "render.h"
#include "render_waterfall.h"
#include "settings.h"
#include "dsp/iq_balance.h"
#include "mem_channels.h"
#include "wifi.h"
#include "iq_balance.h"
#include "ui_mode.h"
#include "ft8_screen.h"
#include "ft8_tx.h"
#include "ft8_status.h"
#include "ft8_qso.h"
#include "diag_log.h"
#include "tab5_keyboard.h"
#include "time_sync.h"
#include "adif/adif_log.h"

static const char *TAG = "main";


void app_main(void)
{
    // Install the diagnostic log capture hook first so the whole boot
    // sequence is captured if diagnostic logging was left enabled. Capture
    // only actually starts once diag_log_set_enabled(true) runs below.
    diag_log_init();

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
    mem_channels_init();
    adif_log_init();
    qmx_settings_t cfg;
    settings_load_all(&cfg);
    iq_balance_init(cfg.iq_enabled);  /* Restore IQ balance state from NVS */
    diag_log_set_enabled(cfg.diag_log);  /* Restore diagnostic logging state from NVS */

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));

    bsp_info_log();

    // Enable battery charging (BSP defines these but never calls them)
    bsp_set_charge_qc_en(true);
    bsp_set_charge_en(true);

    // Initialise INA226 battery monitor (shares main I2C bus with PI4IO)
    battery_init(bsp_i2c_get_handle());

    // Init RX8130CE supercap RTC and apply stored time to system clock.
    // Spawns the periodic QMX time-sync background task.
    time_sync_init(bsp_i2c_get_handle());

    ui_init(disp);

    // Physical Tab5 snap-on keyboard (optional). Probes I2C 0x6D on GPIO0/1;
    // if present, switches it to String mode and types into the focused
    // textarea. Silently disabled (with a bus scan logged) if not attached.
    ui_kbd_bridge_init();
    if (tab5_keyboard_init() == ESP_OK) {
        ESP_LOGI(TAG, "Tab5 physical keyboard ready");
    }

    // Apply persisted settings to UI / render pipeline.
    ui_set_db_range(cfg.db_min, cfg.db_max);
    ui_set_db_labels(cfg.db_min, cfg.db_max);
    render_set_ema_alpha(cfg.ema_alpha);
    display_set_flipped(cfg.display_flip);  // restore upside-down mounting orientation
    status_bar_start();

    // BAND-AID (v0.18.5): e07f114 (CW audio) introduced cw_audio_preopen() which
    // degrades FT8 decode yield by 2-3x even when CW is disabled. Root cause under
    // investigation (likely I2S/DMA contention with USB-audio pipeline). Disabled
    // pending a proper fix. CW audio remains shelved until pipeline rework.
    // cw_audio_preopen();

    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true));
    ESP_LOGI(TAG, "USB host started");

    ESP_ERROR_CHECK(audio_init());
    iq_balance_set_enabled(cfg.iq_enabled);
    ui_set_flat_mode(cfg.flat_mode);
    ui_set_cw_pitch_hz(cfg.cw_pitch_hz);
    ui_set_cw_cal_hz(cfg.cw_cal_hz);
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
    // ui_init() (above) applied the persisted zoom level via ui_set_zoom(),
    // but dsp_set_zoom() is a no-op before dsp_init() creates its config
    // mutex - re-apply now so a saved zoom > x1 engages the zoom-FFT
    // (increased resolution) from first boot instead of staying in plain
    // magnification mode until the user touches the zoom control.
    ui_set_zoom(ui_get_zoom_factor(), ui_get_pan_offset_bins());
    ESP_ERROR_CHECK(render_init());

    // Apply persisted waterfall colorisation + FFT window (Waterfall drawer).
    render_waterfall_set_black_level(cfg.wf_black_db);
    render_waterfall_set_contrast_db(cfg.wf_contrast_db);
    render_waterfall_set_floor_blend((float)cfg.wf_floor_blend / 100.0f);
    dsp_set_window(cfg.wf_window);

    // CW audio out: demodulate CW from the I/Q and play it on the Tab5
    // speaker/headphone. Idle (no CPU, codec released) unless enabled and the
    // radio is in CW/CW-R. Needs dsp_init() (forward ring) and settings.
    cw_audio_init();

    ESP_LOGI(TAG, "Init complete - main task idle");
    // Spawn FT8 self-test on a dedicated task (32 KB stack, core 1).
    // Verifies ft8_lib encoder + monitor + decoder work on ESP32-P4.
    // Logs PASS/FAIL with per-stage timing once the worker completes.
    // Step 4b v0.10: boot directly into FT8 mode so the existing
    // flash-and-watch decode flow keeps working. Step 4c will let
    // the user toggle from the settings drawer.
    ft8_screen_init();
    ft8_status_init();
    ft8_tx_init();
    ft8_qso_init();
    // Restore last UI mode (Panadapter/FT8), persisted across reboots.
    ui_apply_saved_mode();
}

