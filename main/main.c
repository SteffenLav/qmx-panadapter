#include <stdio.h>
#include <stdlib.h>
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
#include "net/update_check.h"
#include "net/manual_embed.h"  // built-in user manual (boot integrity check)
#include "ui/reader_view.h"
#include "iq_balance.h"
#include "ui_mode.h"
#include "ft8_screen.h"
#include "ft8_tx.h"
#include "ft8_status.h"
#include "ft8_qso.h"
#include "ft8_pileup.h"
#include "ft8_sim.h"
#include "net/pskreporter.h"
#include "net/spots.h"
#include "net/rbn.h"
#include "ft8_hash.h"
#include "diag_log.h"
#include "factory_reset.h"
#include "cpu_stats.h"
#include "sd_archive.h"
#include "tab5_keyboard.h"
#include "usb_hid_mouse.h"
#include "time_sync.h"
#include "adif/adif_log.h"
#include "util/psram_task.h"
#include "util/usb_replug.h"

static const char *TAG = "main";

void app_main(void)
{
    // Install the diagnostic log capture hook first so the whole boot
    // sequence is captured. Diagnostic logging is always-on (no opt-in) — the
    // session header is written once below, after settings come up.
    diag_log_init();

    // Apply any pending selective NVS reset requested from the web UI before a
    // reboot. Must run before nvs_flash_init()/settings_init() open handles on
    // the partitions we may be about to erase. No-op on a normal boot.
    factory_reset_apply_pending();

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

    // === BENCH HARNESS - MUST be 0 in shipping builds ==================
    // Drives an unattended simulated QSO so FT8 exchange/logging changes can be
    // verified with no antenna, no QMX and no touch input: sim mode supplies
    // phantom stations, the robot answers their CQ from IDLE, and the completed
    // QSO lands in the ADIF log (fetch /api/adif). Used 2026-07-26 to verify the
    // GRIDSQUARE fix (93106be) end-to-end: 2/2 sim QSOs logged their grid,
    // against a 5/34 baseline in the real log on the buggy firmware.
    //   1  = force sim + robot + grey-list ON
    //  -1  = force them OFF **and delete the FREQ==0 sim QSOs** the run logged
    //        (they would otherwise be uploaded to QRZ/LoTW/eQSL as real
    //        contacts). Run -1, confirm, then reflash with 0.
    // The settings calls PERSIST to NVS, which is why -1 exists at all.
    #define FT8_BENCH_SIM 0
    #if FT8_BENCH_SIM != 0
    {
        bool on = (FT8_BENCH_SIM > 0);
        qmx_settings_t bs;
        settings_load_all(&bs);
        ft8_filters_t bf = bs.ft8_filters;
        bf.robot_en = on;
        settings_set_ft8_filters(&bf);
        settings_set_sim_mode_en(on);
        // The sim's G0ABC phantom is deliberately DEAF and the robot's picker
        // will keep choosing it - every pounce times out and no QSO ever
        // completes. Grey-listing is what breaks that loop (two timeouts -> the
        // auto pickers skip it), so the bench run needs it on.
        settings_set_greylist_en(on);
        settings_flush();
        ESP_LOGW(TAG, "BENCH: sim=%d robot=%d greylist=%d (FT8_BENCH_SIM=%d)",
                 on, on, on, FT8_BENCH_SIM);

        if (!on) {
            // Same scan as the ADIF viewer's "Del N test" button: a real
            // contact always has a CAT frequency, so FREQ==0 marks a sim QSO.
            // Delete HIGHEST-INDEX-FIRST - adif_log_delete_record() shifts
            // every later record down one slot.
            int cap = adif_log_count();
            int *idxs = cap > 0 ? heap_caps_malloc((size_t)cap * sizeof(int),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
            int n_test = 0;
            if (idxs) {
                FILE *f = fopen(adif_log_file_path(), "r");
                if (f) {
                    char raw[1024], freq_s[16];
                    bool hdr = false;
                    int rec = 0;
                    while (fgets(raw, sizeof(raw), f) && n_test < cap) {
                        if (!hdr) { hdr = true; continue; }
                        int this_rec = rec++;
                        freq_s[0] = '\0';
                        if (adif_log_extract_field(raw, "FREQ", freq_s, sizeof(freq_s)) &&
                            atof(freq_s) < 0.001) idxs[n_test++] = this_rec;
                    }
                    fclose(f);
                }
            }
            int deleted = 0;
            for (int i = n_test - 1; i >= 0; i--)
                if (adif_log_delete_record(idxs[i])) deleted++;
            if (idxs) heap_caps_free(idxs);
            ESP_LOGW(TAG, "BENCH: deleted %d sim (FREQ==0) QSO record(s)", deleted);
        }
    }
    #endif
    // === END BENCH HARNESS =============================================

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
    manual_embed_log_summary();   // built-in user manual shipped intact?


    // Enable battery charging (BSP defines these but never calls them)
    bsp_set_charge_qc_en(true);
    bsp_set_charge_en(true);

    // Initialise INA226 battery monitor (shares main I2C bus with PI4IO)
    battery_init(bsp_i2c_get_handle());

    // Init RX8130CE supercap RTC and apply stored time to system clock.
    // Spawns the periodic QMX time-sync background task.
    time_sync_init(bsp_i2c_get_handle());

    ui_init(disp);
    ui_mouse_init();   // LVGL pointer indev + cursor for a USB mouse (hidden until one appears)
    display_fade_in_backlight(cfg.brightness_pct);  // reveal the app over 500ms instead of an instant flash

    // === BENCH HOOK - MUST be 0 in shipping builds =======================
    // Opens the Reader at boot so the built-in manual can be screenshotted via
    // /ss.bmp without touching the screen. Under display_lock because LVGL is not
    // thread-safe and app_main is not the LVGL thread.
    #define READER_BENCH_OPEN 0
    #if READER_BENCH_OPEN
    if (display_lock(1000)) { reader_view_show(); display_unlock(); }
    #endif
    // === END BENCH HOOK ==================================================

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

    // NO automatic replug at boot - deliberately. Hardware-tested 2026-08-03
    // (TODO #74): the stale-QMX wedge (QMX answers enumeration with 8 of 16
    // descriptor bytes after some warm reboots) is QMX-firmware-side and
    // survives every host-side cue - bus resets, root-port power cycles,
    // USB5V_EN cuts up to 8 s. A boot replug can't cure it, and aborting a
    // healthy first enumeration (which normally succeeds) risks INDUCING
    // the wedge. usb_replug() remains available via the hidden /api/cmd
    // action for experiments; the task below detects the wedge and tells
    // the operator to power-cycle the QMX instead of leaving a dead screen.
    usb_replug_watchdog_start();

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

    // USB HID mouse (Phase 1: enumerate + log). Installs the HID host driver
    // alongside the QMX's UAC+CDC-ACM on the same host; a mouse shares the port
    // via a powered hub. No-op if no mouse/hub is present.
    usb_hid_mouse_init();

    // WiFi+SNTP runs in a background task; doesn't block boot.
    // DISABLED pending C6 firmware investigation (see CLAUDE.md / git log).
    // === BENCH EXPERIMENT - MUST be 1 in shipping builds =================
    // Simulates a genuinely WiFi-off (POTA/field) unit so the SD archive's
    // WiFi-off branch can be exercised. Capture over SERIAL - there is no
    // network in that mode.
    //   1 = normal: start WiFi, touch no settings          <- shipping
    //   0 = test:    force wifi_enabled=false, don't start WiFi
    //  -1 = restore: force wifi_enabled=true, start WiFi   (run once, then set 1)
    // 0 and -1 WRITE NVS, which is why the restore step is explicit rather than
    // automatic - a normal boot must never override a user who turned WiFi off.
    #define BENCH_WIFI_ENABLED 1
    #if BENCH_WIFI_ENABLED != 1
    settings_set_wifi_enabled(BENCH_WIFI_ENABLED == -1);
    settings_flush();
    ESP_LOGW(TAG, "BENCH: forced wifi_enabled=%d (BENCH_WIFI_ENABLED=%d)",
             (BENCH_WIFI_ENABLED == -1), BENCH_WIFI_ENABLED);
    #endif
    #if BENCH_WIFI_ENABLED != 0
    panadapter_wifi_start();
    #else
    ESP_LOGW(TAG, "BENCH: WiFi deliberately NOT started (SD isolation test)");
    #endif
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

    // Tier 0 resource diagnostics: per-task per-core CPU% every 10 s into the
    // diag log. Started last so the boot-time task churn above doesn't skew
    // the first window.
    cpu_stats_init();   // v2: idle-only O(1) sampler (see cpu_stats.c for why no per-task walks)

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
    ft8_pileup_init();
    ft8_sim_init();
    pskreporter_init();
    spots_init();          // live POTA spots on the spectrum (WiFi, opt-out)
    rbn_init();            // RBN as a second source into the same store (opt-IN)
    ft8_arrl_fd_selftest();
    ft8_hash_selftest();
    ft8_sim_synth_selftest();
    ft8_arrl_fd_e2e_selftest();
    // Restore last UI mode (Panadapter/FT8), persisted across reboots.
    ui_apply_saved_mode();

    // Background firmware-update poller for the docs Reader page. Self-throttles
    // (first check ~30 s after boot, then every 6 h) and no-ops while WiFi is
    // down, so it's harmless on offline/POTA units.
    update_check_start();
}

