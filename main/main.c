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
#include "ft8_sim.h"
#include "diag_log.h"
#include "sd_archive.h"
#include "tab5_keyboard.h"
#include "time_sync.h"
#include "adif/adif_log.h"

static const char *TAG = "main";


void app_main(void)
{
    // Install the diagnostic log capture hook first so the whole boot
    // sequence is captured. Diagnostic logging is always-on (no opt-in) — the
    // session header is written once below, after settings come up.
    diag_log_init();

    ESP_LOGI(TAG, "QMX+ Panadapter starting");
    ESP_LOGI(TAG, "HEAP boot: int=%uKB psram=%zuMB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
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
    // adif_log_init() mounted SPIFFS; now the diag log can persist to flash so
    // it survives power-off with no SD card (POTA: log in the field, analyse
    // at home). Background task, 256 KB rolling file, downloadable at
    // /api/log/saved.
    diag_log_persist_start();
    qmx_settings_t cfg;
    settings_load_all(&cfg);
    iq_balance_init(cfg.iq_enabled);  /* Restore IQ balance state from NVS */
    diag_log_write_session_header();  /* always-on capture; stamp the session */

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

    // Background microSD auto-archive: mirrors the diag log, ADIF, and config
    // to a card if one is present (probes for it; no card-detect line). Started
    // after ui_init so the SD mount never races display bring-up and the dot
    // exists when the first mount callback fires.
    sd_archive_init();
    // Belt-and-suspenders: sync the dot in case a mount completed before this.
    ui_set_sd_active(sd_archive_is_mounted());

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

    // BAND-AID EXTENDED (v0.18.6): cw_audio_init() spawns cw_audio_task at
    // PRIORITY 6 on core 1 - higher than fft_task (4) and both FT8 tasks (1) -
    // looping forever on a 120ms vTaskDelay even though cw_audio_preopen() is
    // already disabled above (s_codec_ready can never become true, so the task
    // does nothing but wake/check/sleep). The v0.18.5 band-aid disabled the two
    // things CW audio actually DOES but missed this: a priority-6 "ghost" task
    // preempting fft_task - the audio ring's sole consumer for BOTH panadapter
    // and FT8 capture - ~125 times per 15s FT8 slot, for the entire session.
    // Root-caused 2026-06-25 via empirical diff against v0.18.0 (which has no
    // cw_audio.c at all) after the user found NO release after v0.18.0 matched
    // its sustained decode yield, even with the v0.18.5 band-aid applied. Fits
    // the "first slot decodes great, every slot after collapses" pattern from
    // the v0.18.4 investigation: fresh-boot ring has no backlog yet; periodic
    // high-priority preemption of fft_task lets the ring backlog grow, so each
    // subsequent FT8 capture reads time-shifted audio that still syncs (sync
    // detection tolerates jitter) but doesn't decode (LDPC needs exact symbol
    // alignment). CW audio remains fully shelved - do not re-enable without
    // also fixing this task's priority/cadence as part of the pipeline rework.
    // cw_audio_init();

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
    ft8_sim_init();
    ft8_arrl_fd_selftest();
    ft8_sim_synth_selftest();
    ft8_arrl_fd_e2e_selftest();
    // Restore last UI mode (Panadapter/FT8), persisted across reboots.
    ui_apply_saved_mode();
}

