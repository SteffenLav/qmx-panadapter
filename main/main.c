#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "util/format_freq.h"   // #302
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
#include "gpio_relay.h"
#include "cat.h"
#include "cw_decode.h"
#include "audio.h"
#include "rx_audio.h"
#include "dsp.h"
#include "render.h"
#include "render_waterfall.h"
#include "settings.h"
#include "ft8_robot.h"   // ft8_robot_stand_down - auto-answer must not survive a boot
#include "dsp/iq_balance.h"
#include "mem_channels.h"
#include "wifi.h"
#include "util/usb_shutdown.h"
#include "net/update_check.h"
#include "net/ota_update.h"
#include "net/manual_embed.h"  // built-in user manual (boot integrity check)
#include "net/reader_net.h"    // reader_net_purge_legacy_caches() at boot
#include "ui/reader_view.h"
#include "iq_balance.h"
#include "spur_map.h"
#include "ui_mode.h"
#include "ft8_screen.h"
#include "ft8_tx.h"
#include "wspr_tx.h"
#include "wspr_spots.h"
#include "net/wsprnet.h"
#include "ft8_status.h"
#include "ft8_qso.h"
#include "ft8_pileup.h"
#include "ft8_sim.h"
#include "net/pskreporter.h"
#include "net/spots.h"
#include "net/qrz_coords.h"
#include "net/pskr_self.h"
#include "net/wspr_self.h"
#include "net/band_conditions.h"
#include "net/psk_rx.h"
#include "bt_hid_mouse.h"
#include "net/rbn.h"
#include "net/dxcluster.h"
#include "ft8_hash.h"
#include "diag_log.h"
#include "panic_hook.h"
#include "factory_reset.h"
#include "cpu_stats.h"
#include "util/dma_owners.h"   // TEMP INSTRUMENT #283 - boot DMA trace
#include "sd_archive.h"
#include "tab5_keyboard.h"
#include "usb_hid_mouse.h"
#include "time_sync.h"
#include "adif/adif_log.h"
#include "util/psram_task.h"
#include "util/usb_replug.h"
#include "util/heap_watch.h"   /* INSTRUMENT 2026-09-26 - the 84 zero-dips */
#include "cJSON.h"
#include "esp_heap_caps.h"

static const char *TAG = "main";

/* ---- Boot memory ledger -------------------------------------------------
 *
 * Internal RAM is THE constraint on this board and nothing measured where it
 * goes. On 2026-09-25 the bench sat at 19 KB free / 10-12 KB largest block
 * with audio on, which is enough to cost three separate features:
 *   - the SD diag mirror (esp_dma_capable_malloc refused, backing off to 240 s),
 *   - four network feeds (gated off whenever RX audio is on),
 *   - the BLE mouse (its guard needs 24576 B and measured 21391 B).
 *
 * dma_owners can attribute it, but only with CONFIG_HEAP_TASK_TRACKING, which
 * stores an owner handle in EVERY block header and so changes the very numbers
 * being measured (documented in sdkconfig.defaults, measured 2026-08-28).
 *
 * This is the non-perturbing alternative: print free DMA-capable and internal
 * bytes either side of each major init, so the consumer is named by
 * SUBTRACTION rather than by attribution. Two heap_caps_get_free_size() calls
 * and one log line per milestone - no tracking, no allocation, nothing that
 * changes what it observes.
 *
 * Deliberately INFO and deliberately permanent: every field diagnostic then
 * carries the same ledger, so a user reporting "no Bluetooth" or a yellow SD
 * dot arrives with the reason already in the file. */
#define MEM_LEDGER(stage)                                                     \
    ESP_LOGI("memledger", "%-26s dma=%6u B (lblk %5u)  int=%6u B (lblk %5u)", \
             (stage),                                                         \
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),               \
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),      \
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),          \
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))


/* ⭐ cJSON OUT OF INTERNAL RAM (2026-09-26).
 *
 * MEASURED, not assumed: heap_watch's per-task attribution caught the `httpd`
 * task growing +8688 B across 311 BLOCKS inside a single internal-RAM dip,
 * and `httpd` was also the task whose 116 B allocations were being refused.
 * webserver.c makes 495 cJSON_Add* calls, each of which allocates a node and
 * often a string, and cJSON had NO allocator hooks anywhere in the tree - so
 * every one of them came out of internal RAM, the scarcest pool on the board.
 *
 * JSON never needs to be DMA-capable or internal: it is built, printed into
 * the socket, and freed. PSRAM is 14 MB free and idle.
 *
 * Hooks are global and must be installed before ANY cJSON use. heap_caps_free
 * handles both pools, so anything allocated before this point still frees
 * correctly. Falls back to internal RAM if PSRAM ever refuses, because a
 * failed API response is worse than a small internal allocation. */
static void *qmx_json_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}
static void qmx_json_free(void *p) { heap_caps_free(p); }

void app_main(void)
{
    // Install the diagnostic log capture hook first so the whole boot
    // sequence is captured. Diagnostic logging is always-on (no opt-in) — the
    // session header is written once below, after settings come up.
    diag_log_init();

    // Library INFO chatter that says nothing about OUR state (2026-09-13 log
    // audit): NimBLE prints 5-6 lines every time a scan window starts (every
    // 60 s + 15 s, for ever, on a unit with no mouse), and the cert bundle
    // prints one per TLS handshake. Warnings and errors still come through;
    // btmouse logs the BLE events that matter under its own tag.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    // #117: if the last boot crashed, report it NOW - immediately after the log
    // hook is installed, so the record is captured, and before anything else can
    // panic and cost us the report. Panics never reach diag_log's vprintf hook,
    // so before this a field crash left nothing on the device at all: the
    // operator sent a diagnostic download containing everything except the one
    // thing needed. No-op on a clean boot.
    panic_hook_report_previous();

    // Apply any pending selective NVS reset requested from the web UI before a
    // reboot. Must run before nvs_flash_init()/settings_init() open handles on
    // the partitions we may be about to erase. No-op on a normal boot.
    factory_reset_apply_pending();

    ESP_LOGI(TAG, "QMX+ Panadapter starting");
    ESP_LOGI(TAG, "HEAP boot: int=%uKB psram=%zuMB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024 * 1024));
    MEM_LEDGER("app_main entry");
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
    // Unattended transmission must never be the state the device powers up in.
    // If auto-answer was left on at shutdown it used to be on at boot and would
    // start answering CQs within a cycle or two - before the operator had
    // checked the antenna, the band, or that they meant to be transmitting at
    // all (Roy KI0ER). Turning it on is a deliberate act, once per session.
    // NULL: no toast at boot, the UI does not exist yet and the checkbox will
    // simply read unchecked, which is the truth.
    ft8_robot_stand_down(NULL);
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

    // adif_log_init() mounted SPIFFS. Reclaim the reader page/TOC caches an
    // older firmware left there BEFORE the diag log starts filling the space -
    // nothing has read those files since 2026-08-06 (see reader_net.c).
    reader_net_purge_legacy_caches();

    // Now the diag log can persist to flash so it survives power-off with no SD
    // card (POTA: log in the field, analyse at home). Background task, 256 KB
    // rolling file, downloadable at /api/log/saved.
    diag_log_persist_start();
    /* Start early: the dips begin about 30 min in, but an allocation that
     * FAILS during boot is exactly the SD/BLE symptom we are chasing. */
    heap_watch_start();
    { cJSON_Hooks h = { .malloc_fn = qmx_json_malloc, .free_fn = qmx_json_free };
      cJSON_InitHooks(&h); }

    // TEMP INSTRUMENT, ANSWERED AND STOOD DOWN AGAIN 2026-09-20. Re-armed at
    // 180/420/600 s to find who holds MALLOC_CAP_DMA once it settles (~8-12 KB
    // free, confirmed FLAT across the whole 10-minute trace - not a leak, a
    // fixed ceiling: 180,616 B under one dead app_main handle - WiFi/esp_hosted/
    // LVGL bring-up, legitimately long-lived - plus 96,000 B across 26 task
    // stacks still internal, taskLVGL's 17,408 B the single biggest of those.
    // See apply_lvgl_port_task_psram.ps1, the fix taken from this reading.
    // Re-enable the call (and CONFIG_HEAP_TASK_TRACKING in sdkconfig) to ask
    // again.
    //
    // Prior run 2026-09-06 (8/18/60 s): usb_host_install() takes 47 KB, dsp_init
    // 37 KB, pool spent before WiFi is reached.

    /* Heap, not stack: this struct grew again (#pwrcal) and "main" is an 8 KB
     * task that still has everything below to do - see CLAUDE.md, "Task
     * stacks on this board are TINY". Used across a wide span of app_main()
     * (down to dsp_set_window() below), so it lives until its last read,
     * freed right after. 28 MB of PSRAM is free at this point in boot; a
     * failed allocation here means something is badly wrong, so fall back to
     * a zeroed on-stack instance rather than dereference NULL for the next
     * couple hundred lines. */
    /* ...and in PSRAM, not internal DIRAM (2026-09-17): this is 3,596 bytes of
     * the scarcest memory on the board, held for the entire session, insuring
     * a path that by the comment above only runs when "something is badly
     * wrong". Safe despite insuring a PSRAM allocation - .ext_ram.bss is
     * mapped at LINK time, so it does not depend on the heap that failed.
     * Part of the DIRAM reclamation that took the internal-free watermark off
     * 0 KB; see ft8_screen.c's s_table for the measurements. */
    static EXT_RAM_BSS_ATTR qmx_settings_t s_cfg_fallback;  // static, NOT stack - see above
    qmx_settings_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cfg) {
        settings_load_all(cfg);
    } else {
        ESP_LOGE(TAG, "boot config: PSRAM allocation failed - using defaults");
        cfg = &s_cfg_fallback;
    }
    iq_balance_init(cfg->iq_enabled);  /* Restore IQ balance state from NVS */
    diag_log_write_session_header();  /* always-on capture; stamp the session */

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));
    MEM_LEDGER("display_init");

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

    /* ⛔ THE DATA LAYERS COME BEFORE THE UI, so the saved page can be restored
     * before the screen is revealed. These are mutexes and PSRAM buffers -
     * nothing here touches audio, dsp, cat or the network - and they used to
     * sit ~180 lines further down purely by accident of when they were added.
     * Leaving them there is what forced the mode restore to happen late. */
    ft8_screen_init();
    ft8_status_init();
    ft8_tx_init();
    wspr_tx_init();
    wspr_spots_init();

    ui_init(disp);
    MEM_LEDGER("ui_init (LVGL)");
    ui_mouse_init();   // LVGL pointer indev + cursor for a USB mouse (hidden until one appears)

    /* ⭐ RESTORE THE PAGE BEFORE THE BACKLIGHT, so the Tab5 is ON the page it
     * was left on from the first frame anybody sees, rather than switching to
     * it once the rest of boot has crawled past. The engines follow later, in
     * ui_apply_saved_mode_start(), where audio/dsp/cat exist.
     *
     * ⛔ Under display_lock: this moves LVGL widgets and shows whole views, and
     * we are the main task, not taskLVGL. */
    if (display_lock(2000)) {
        ui_apply_saved_mode_view();
        display_unlock();
    } else {
        ESP_LOGW(TAG, "no display lock - the saved page will not be restored");
    }

    display_fade_in_backlight(cfg->brightness_pct);  // reveal the app over 500ms instead of an instant flash

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
    MEM_LEDGER("sd_archive_init");

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
    ui_set_db_range(cfg->db_min, cfg->db_max);
    ui_set_db_labels(cfg->db_min, cfg->db_max);
    render_set_ema_alpha(cfg->ema_alpha);
    display_set_flipped(cfg->display_flip);  // restore upside-down mounting orientation
    status_bar_start();

    // Opens the ES8388/I2S output path before USB host claims DMA-capable RAM
    // (see rx_audio.h). No-op unless RX audio is persisted-enabled, so this
    // costs nothing on any unit that has never turned it on.
    rx_audio_preopen();
    MEM_LEDGER("rx_audio_preopen (I2S)");

    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true));
    MEM_LEDGER("usb_host_start");
    ESP_LOGI(TAG, "USB host started");
    // Make every firmware-initiated reboot tear the USB link down properly, so
    // the QMX is told we are going rather than finding out (see usb_shutdown.h).
    usb_shutdown_install_handler();

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
    MEM_LEDGER("audio_init (UAC ring)");
    iq_balance_set_enabled(cfg->iq_enabled);
    ui_set_flat_mode(cfg->flat_mode);
    // Seed only - do NOT push to the radio here. CAT does not exist yet (it opens
    // ~13 s from now), so the MMCW write this used to make went nowhere on every
    // single boot. The radio tells us its own centre at link-up instead, and that is
    // the value that wins; this is just what to show until it does.
    ui_seed_cw_pitch_hz(cfg->cw_pitch_hz);
    ui_set_cw_cal_hz(cfg->cw_cal_hz);
    ui_set_rit_pill_show(cfg->rit_pill_show);   // before the drawer is ever opened
    /* #298: before the first frame, so nobody sees the wrong one and then a jump.
     * Defaults ON; the one-time notice in ui.c tells the operator how to go back. */
    ui_set_still_view(cfg->still_view);
    ui_still_notice_arm(!cfg->still_notice_done);
    render_waterfall_set_colormap(cfg->colormap_idx);

    /* ⛔ SAY IT ON THE SCREEN WHEN THE SUPPLY BROWNED OUT.
     *
     * Samuel W7STF, 2026-09-24: days lost to "the Tab5 reboots on its own"
     * while listening to SSB, convinced his unit was faulty - "it's bothersome
     * others are not reporting this". His logs carry TEN
     * reset_reason=brownout. The firmware knew every time and told nobody.
     *
     * Brownout is the one reset the operator can actually fix, and the one
     * that is NOT our bug, so it is the one that must not stay buried in a
     * log file. Long toast because it arrives during boot clutter and the
     * whole point is that it gets read. Not persisted and not a modal: a
     * single brownout on a dodgy cable should inform, not nag. */
    if (diag_log_booted_after_brownout()) {
        ESP_LOGW(TAG, "previous boot ended in a SUPPLY BROWNOUT - not a crash; "
                      "the 5 V rail dropped below the chip's threshold");
        ui_toast_ms("Restarted by a power brownout - not a fault. "
                    "Check the USB-C supply and cable; loud audio draws the most.",
                    12000);
    }

    // Restore last-known VFO frequency (display only; QMX is source of truth).
    if (cfg->last_vfo_hz != 0) {
        ESP_LOGI(TAG, "Restored last VFO: %lu Hz", (unsigned long)cfg->last_vfo_hz);
        ui_update_frequency(cfg->last_vfo_hz);
    } else {
        ESP_LOGI(TAG, "No stored VFO (first boot or cleared NVS)");
    }
    // Before cat_init(): the poll task starts inside it and can deliver a TB
    // response immediately, and cw_decode_feed() drops everything until the
    // ring exists.
    /* #302: the stored frequency punctuation, applied before any UI is built.
       Every caller reads g_freq_style at format time, so this is the only
       place it has to be set. */
    {
        /* Heap, not stack - same reasoning as the cfg block above. */
        qmx_settings_t *fs = heap_caps_malloc(sizeof(*fs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (fs) {
            settings_load_all(fs);
            g_freq_style = (fs->freq_sep_style == 1) ? FREQ_STYLE_COMMA : FREQ_STYLE_DOTS;
            heap_caps_free(fs);
        }
    }
    cw_decode_init();
    ESP_ERROR_CHECK(cat_init());
    MEM_LEDGER("cat_init (CDC)");

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
    // ⛔ WAIT FOR THE SD BOOT PROBE BEFORE STARTING WiFi (2026-09-06).
    //
    // sd_archive.h has said for a long time that sd_archive_wait_mounted() is
    // "intended for one caller: app_main ... BEFORE panadapter_wifi_start() -
    // the only window in which SD writes are reliable on this board". It had
    // ZERO callers, so that ordering was never actually enforced and the two
    // simply raced.
    //
    // Measured on the dev bench, same build, two consecutive boots:
    //
    //   mount finished 5.703 s, esp_hosted_init 5.842 s -> mounted FIRST TRY,
    //     one attempt, 51 KB of MALLOC_CAP_DMA taken and held (the card's own
    //     working buffers - legitimate, not a leak)
    //   attempt 1 at 5.62 s, esp_hosted_init 5.88 s      -> all FIVE attempts
    //     failed 0x108/0x109/0x103/0x104, DMA fell 135 KB -> 4.4 KB, and that
    //     same boot took an Instruction access fault in sdio_read with
    //     MEPC=0x00000000
    //
    // A ~200 ms scheduling difference decided it. That is precisely CLAUDE.md's
    // long-standing "mount fails intermittently with 0x108, observed on 2 of 5
    // boots" - not bad luck, a race that was documented as being prevented.
    //
    // The cost is bounded: the probe is 5 tries with a short gap, ~1.2 s worst
    // case, and returns as soon as a card mounts (typically ~0.5 s). A unit
    // with no card pays the full window once per boot, before WiFi - which is
    // the same trade the probe itself already makes.
    //
    // ⚠ This does NOT claim to fix the SD/WiFi wedge. It removes one race that
    // was making the mount a coin toss; whether the wedge survives it is a
    // separate question and needs its own soak.
    #if BENCH_WIFI_ENABLED != 0
    panadapter_wifi_start();
    MEM_LEDGER("wifi_start (hosted/SDIO)");
    // ⭐ REVERSED 2026-09-06, and the reason is measured rather than tidy.
    //
    // This used to wait for the SD mount BEFORE starting WiFi, on the strength
    // of sd_archive.h's "the only window in which SD writes are reliable".
    // That guarantees the mount takes its ~51 KB of MALLOC_CAP_DMA first, and
    // measurement showed what that leaves esp_hosted's SDIO card init: 2039 B,
    // largest block 1152 B, against 19927/10240 with the archive off. A card
    // init that cannot allocate used to reboot the device.
    //
    // So WiFi goes first now, and sd_archive's own boot probe waits for the
    // card init to finish (qmx_sdio_card_ready_wait) before mounting. Both
    // still get their memory before the system is busy; neither is asking
    // while the other allocates.
    //
    // The wait below is kept, and still runs before the Reader's staged
    // offline manual is flushed - it just no longer gates WiFi on it.
    if (!sd_archive_wait_mounted(9000))
        ESP_LOGW(TAG, "SD: no card mounted in the boot window");
    #else
    ESP_LOGW(TAG, "BENCH: WiFi deliberately NOT started (SD isolation test)");
    #endif
    ESP_ERROR_CHECK(dsp_init());
    MEM_LEDGER("dsp_init");
    // After dsp_init: the detector arms dsp_avg_*, which needs the FFT running.
    spur_map_init();
    // ui_init() (above) applied the persisted zoom level via ui_set_zoom(),
    // but dsp_set_zoom() is a no-op before dsp_init() creates its config
    // mutex - re-apply now so a saved zoom > x1 engages the zoom-FFT
    // (increased resolution) from first boot instead of staying in plain
    // magnification mode until the user touches the zoom control.
    ui_set_zoom(ui_get_zoom_factor(), ui_get_pan_offset_bins());
    ESP_ERROR_CHECK(render_init());
    MEM_LEDGER("render_init");

    // The drawer opens on whichever half the operator last chose. Applied here
    // rather than inside ui_init() because it only moves widgets that already
    // exist, and ui_set_drawer_expert() is a no-op when the value matches.
    ui_set_drawer_expert(cfg->drawer_expert);

    // Apply persisted waterfall colorisation + FFT window (Waterfall drawer).
    render_waterfall_set_black_level(cfg->wf_black_db);
    render_waterfall_set_contrast_db(cfg->wf_contrast_db);
    render_waterfall_set_floor_blend((float)cfg->wf_floor_blend / 100.0f);
    dsp_set_window(cfg->wf_window);
    render_set_waterfall_speed_mult(cfg->wf_speed_mult);
    if (cfg != &s_cfg_fallback) heap_caps_free(cfg);  // last read of cfg - see its declaration above
    cfg = NULL;

    // Spawns rx_audio_task, pinned strictly below fft_task's priority and
    // fully blocked (no wakeups at all) whenever RX audio is disabled - see
    // rx_audio.h for why the old CW-only version (priority ABOVE fft_task,
    // polling every 120 ms even when idle) cost FT8 decode yield, and how
    // this supersedes it. Confirmed clean over an 8.7 h live FT8 session
    // before this call was re-enabled (memory project_rx_audio_track.md).
    rx_audio_init();
    MEM_LEDGER("rx_audio_init");

    // Tier 0 resource diagnostics: per-task per-core CPU% every 10 s into the
    // diag log. Started last so the boot-time task churn above doesn't skew
    // the first window.
    cpu_stats_init();   // v2: idle-only O(1) sampler (see cpu_stats.c for why no per-task walks)
    gpio_relay_init();  // GPIO53/54 remote relay pulse - see gpio_relay.h

    ESP_LOGI(TAG, "Init complete - main task idle");
    // Spawn FT8 self-test on a dedicated task (32 KB stack, core 1).
    // Verifies ft8_lib encoder + monitor + decoder work on ESP32-P4.
    // Logs PASS/FAIL with per-stage timing once the worker completes.
    // Step 4b v0.10: boot directly into FT8 mode so the existing
    // flash-and-watch decode flow keeps working. Step 4c will let
    // the user toggle from the settings drawer.
    /* ⬆ ft8_screen_init / ft8_status_init / ft8_tx_init / wspr_tx_init /
     * wspr_spots_init MOVED ABOVE ui_init() - see the block there. They are
     * mutexes and buffers with no dependency on anything between here and
     * there, and the mode restore needs them before it can show a page. */
    wsprnet_init();          /* OFF unless the operator enabled it */
    ft8_qso_init();
    ft8_pileup_init();
    ft8_sim_init();
    pskreporter_init();
    spots_init();          // live POTA spots on the spectrum (WiFi, opt-out)
    psk_rx_selftest();     // attribute names still match the collector's output
    psk_rx_init();         // propagation feedback: who is hearing US (WiFi, opt-in)
    bt_hid_mouse_init();   // BLE mouse over the C6 (opt-in; Stage 1 = scan only)
    qrz_coords_start();    // real-station lookup for RBN self-spot skimmers on the spot map (opt-in via credentials)
    pskr_self_init();      // live PSK Reporter self-spotting (MQTT) for the spot map's Digi side
    wspr_self_init();      // wsprnet.org self-spotting query for the spot map's WSPR side
    band_conditions_start(); // HF band conditions for the spot map's CONDITIONS tab
    rbn_init();            // RBN as a second source into the same store (opt-IN)
    dxcluster_selftest();  // parser vs lines captured from a real cluster node
    dxcluster_init();      // human DX-cluster spots - the only PHONE source (opt-in)
    /* ⛔ RESTORE THE MODE BEFORE THE SELF-TESTS, NOT AFTER THEM.
     *
     * It used to sit below the four calls that follow, and two of those
     * synthesise GFSK audio and run it through the real decoder. That is a few
     * seconds on an idle board and far longer once the radio is streaming and
     * the FT8/WSPR tasks are competing for core 1 - measured across three
     * boots on 2026-09-09 at 7.9 s, 46.3 s and 91.7 s.
     *
     * The operator is using the Tab5 long before that, so the restore was
     * arriving into a UI somebody was already navigating: on one of those boots
     * it read WSPR from NVS, set the mode, and 2 ms later the swipe handler on
     * taskLVGL saw WSPR and cycled it to Panadapter - which wrote Panadapter to
     * NVS. The screen showed WSPR, the stored value said Panadapter, and every
     * boot after that came up on the panadapter. Reported as "why is the Tab5
     * always waking up in Panadapter Mode".
     *
     * Nothing below is a dependency: these are verification, not
     * initialisation. What the restore actually needs is the FT8/spots
     * subsystems above, which have all run. */
    // Restore last UI mode (Panadapter/FT8/WSPR), persisted across reboots.
    // ⛔ Under display_lock: it moves LVGL widgets and shows whole views, and
    // this is the main task, not taskLVGL.
    /* The page itself went up before the backlight; this starts what needs
     * audio, dsp and cat. Late is fine - the page is already correct and fills
     * as data arrives. */
    ui_apply_saved_mode_start();

    ft8_arrl_fd_selftest();
    ft8_hash_selftest();
    ft8_sim_synth_selftest();
    ft8_arrl_fd_e2e_selftest();

    // Background firmware-update poller for the docs Reader page. Self-throttles
    // (first check ~30 s after boot, then every 6 h) and no-ops while WiFi is
    // down, so it's harmless on offline/POTA units.
    update_check_start();

    // #218: confirm this image so the bootloader stops treating it as on trial.
    // With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a freshly OTA'd firmware boots
    // PENDING_VERIFY and reverts on the next reset unless it says it is good.
    // Reaching here means display, settings, USB, the UI and the network all
    // came up, which is the only working definition of "this image is fine"
    // available from inside it. No-op on a cable-flashed image.
    ota_update_mark_valid();
}

