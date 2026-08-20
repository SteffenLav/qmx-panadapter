#include "status.h"
#include "net/update_check.h"
#include "net/ota_update.h"
#include "battery.h"
#include "wifi.h"
#include "time_sync.h"
#include "diag_log.h"
#include "ft8_test.h"
#include "settings.h"
#include "bsp/m5stack_tab5.h"
#include "sd_archive.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psram_task.h"
#include "lvgl.h"
#include "ui.h"
#include "bt_hid_mouse.h"
#include "hid_cursor.h"
#include "esp_app_desc.h"
#include "esp_log.h"

static const char *TAG = "status";

// Bench-only: drive the bottom bar's WiFi zone from canned values instead of
// the real link, so the connected layout can be checked without an AP. MUST be
// 0 in any build that leaves the bench - it reports a WiFi link that isn't there.
#define BENCH_WIFI_BAR 0

// SD free/max is only re-queried every SD_POLL_INTERVAL_S (not every 1Hz
// tick) - it touches the physical SDMMC host that WiFi's SDIO link also
// shares, and this project has been bitten three times before by exactly
// that class of hazard (see CLAUDE.md's SD/WiFi SDMMC notes). Free space
// doesn't change fast enough to need second-by-second polling anyway.
#define SD_POLL_INTERVAL_S 20
static uint64_t s_sd_free_b = 0, s_sd_total_b = 0;
static bool     s_sd_ok = false;
static int      s_sd_poll_countdown = 0;  // 0 = poll on the next tick

// Battery care: when settings.charge_limit_en is on, cut charging once the
// pack reaches charge_limit_pct and resume it once the level has dropped
// CHARGE_LIMIT_HYSTERESIS_PCT points below that, so it doesn't rapid-cycle
// right at the threshold. s_charge_cutoff_active latches the cutoff so the
// GPIO write (bsp_set_charge_en) only happens on the transition edges, not
// every tick. `level`/`mv` below are already IR-drop compensated by
// battery_get_level()/battery_get_mv() while charging - see the long
// comment there for why this decision (and the displayed %/icon/voltage)
// needs to track true resting SoC, not momentarily-loaded terminal voltage.
#define CHARGE_LIMIT_HYSTERESIS_PCT 5
static bool s_charge_cutoff_active = false;

// Pick an LVGL battery glyph based on charge level (0-100).
static const char *battery_glyph(int level)
{
    if (level < 0)  return LV_SYMBOL_BATTERY_EMPTY;
    if (level < 20) return LV_SYMBOL_BATTERY_EMPTY;
    if (level < 40) return LV_SYMBOL_BATTERY_1;
    if (level < 60) return LV_SYMBOL_BATTERY_2;
    if (level < 80) return LV_SYMBOL_BATTERY_3;
    return LV_SYMBOL_BATTERY_FULL;
}

// Pale green (full), pale yellow (~half), pale red (low, blinks off every
// other second via blink_on).
#define BATT_COLOR_FULL  0xA0FFA0
#define BATT_COLOR_HALF  0xFFF0A0
#define BATT_COLOR_LOW   0xFF9090

static uint32_t battery_color(int level)
{
    if (level < 0)  return BATT_COLOR_HALF;
    if (level < 30) return BATT_COLOR_LOW;
    if (level < 60) return BATT_COLOR_HALF;
    return BATT_COLOR_FULL;
}


// #218: the bottom-bar version label is CENTRE-aligned at a fixed offset and
// grows both ways, so it collides with the SD dot and the clock if the text
// gets long. A release version ("v1.8.9") is short; a dev build
// ("v1.8.8-9-g7f4451a-dirty") is not, and it was overlapping both neighbours.
// Keep only "vX.Y.Z" for display - the full string is still in the boot log,
// /api/status and the diagnostic download, where it matters.
static void short_ver(const char *in, char *out, size_t out_sz)
{
    if (!in || !out || !out_sz) { if (out && out_sz) out[0] = 0; return; }
    size_t n = 0, dashes = 0;
    while (in[n] && n < out_sz - 1) {
        if (in[n] == '-' && ++dashes >= 1) break;   // stop at the first -N-g...
        out[n] = in[n];
        n++;
    }
    out[n] = 0;
}


// ---------------------------------------------------------------------------
// #218: what a tap on the bottom-bar update line means.
//
// TWO-TAP ARM, and that is not decoration. The bottom edge already owns a
// swipe gesture (it opens Memory Channels) and its own code deliberately makes
// a pure tap inert "so reaching for the swipe grip can't accidentally retune".
// Downloading uses the operator's data and restarting interrupts receiving -
// and with the QMX attached a restart leaves the radio needing a power cycle
// (#74). So the first tap only CHANGES THE TEXT to ask, and the second acts.
// It disarms itself, so a forgotten first tap does nothing.
//
// Nothing is ever downloaded without an explicit tap: the first state offered
// is "download", not a download already in progress.
// ---------------------------------------------------------------------------
#define UPDATE_ARM_US   (5 * 1000000LL)

static int64_t s_update_arm_until = 0;

static bool update_armed(void)
{
    return s_update_arm_until > 0 && esp_timer_get_time() < s_update_arm_until;
}

static void update_line_tap(void)
{
    int  pct = 0;
    char msg[128], latest[32];
    ota_state_t st = ota_update_get_state(&pct, msg, sizeof(msg));

    if (st == OTA_RUNNING) return;          // nothing sensible to do mid-download

    if (!update_armed()) {                  // first tap: only ask
        s_update_arm_until = esp_timer_get_time() + UPDATE_ARM_US;
        return;
    }
    s_update_arm_until = 0;                 // second tap: act, and disarm

    if (st == OTA_DONE) {
        ESP_LOGW("status", "operator confirmed restart into the new firmware");
        esp_restart();
    }

    update_check_get_latest(latest, sizeof(latest));
    if (!latest[0]) return;

    char url[192];
    snprintf(url, sizeof(url),
             "https://github.com/SteffenLav/qmx-panadapter/releases/download/%s/qmx_panadapter.bin",
             latest);
    char err[96];
    if (!ota_update_start(url, err, sizeof(err))) {
        // The refusal reason matters more than the failure - "transmitting" is
        // something the operator can act on.
        ESP_LOGW("status", "update refused: %s", err);
        ui_toast(err);
    }
}

static void status_task(void *arg)
{
    (void)arg;
    char left[96];
    char ssid_buf[64];
    char suffix_buf[80];
    bool blink_on = true;

    // We use coloured-text formatting in the right label only; the static label
    // style needs recolor enabled, but the runtime API lv_label_set_recolor()
    // is what we need. We set it from the UI side. Here we just format strings.

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // --- #218: the bottom-bar version label doubles as the update line ---
        //
        // One writer for one label, and it lives here because this is the only
        // 1 Hz bottom-bar refresh - the update poller runs hours apart and could
        // never show live download progress. The wording is deliberately the
        // same as the browser's, so the two screens read alike.
        {
            char vline[96];
            uint32_t vcol = 0x808080;              // running version, nothing to say
            char running[32], over_s[32], latest_s[32];
            short_ver(esp_app_get_description()->version, running, sizeof(running));

            int  opct = 0;
            char omsg[128], over[32], latest[32];
            ota_state_t ost = ota_update_get_state(&opct, omsg, sizeof(omsg));
            ota_update_get_target_version(over, sizeof(over));
            update_check_get_latest(latest, sizeof(latest));
            short_ver(over,   over_s,   sizeof(over_s));
            short_ver(latest, latest_s, sizeof(latest_s));

            bool armed = update_armed();

            if (ost == OTA_RUNNING) {
                // Name BOTH versions - "45%" alone does not say what you are
                // getting, which is the whole point of showing it here.
                if (running[0] && over_s[0])
                    snprintf(vline, sizeof(vline), "%s " LV_SYMBOL_RIGHT " %s %d%%",
                             running, over_s, opct);
                else
                    snprintf(vline, sizeof(vline), "updating %d%%", opct);
                vcol = 0xFFA040;
            } else if (ost == OTA_DONE) {
                if (armed) snprintf(vline, sizeof(vline), "restart now?");
                else if (over_s[0])
                    snprintf(vline, sizeof(vline), "%s - touch to update", over_s);
                else snprintf(vline, sizeof(vline), "touch to update");
                vcol = 0x8FE0A0;
            } else if (ost == OTA_FAILED) {
                snprintf(vline, sizeof(vline), armed ? "retry download?" : "update failed");
                vcol = 0xFF6060;
            } else if (update_check_available() && latest_s[0]) {
                if (armed) snprintf(vline, sizeof(vline), "download %s?", latest_s);
                else snprintf(vline, sizeof(vline), "%s " LV_SYMBOL_RIGHT " %s download",
                              running, latest_s);
                vcol = 0xFFA040;
            } else {
                snprintf(vline, sizeof(vline), "%s", running);
            }
            ui_set_update_line(vline, vcol);
        }

        // --- LEFT: battery icon (colored by level) + percentage text ---
        int  level    = battery_get_level();
        int  mv       = battery_get_mv();
        bool charging = battery_is_charging();

        qmx_settings_t cfg;
        settings_load_all(&cfg);

        // Battery care: stop charging at a user-set percentage. Uses
        // level_for_limit (IR-drop compensated while actively charging - see
        // CHARGE_IR_DROP_MV above), NOT the raw displayed level, so the
        // decision tracks true SoC instead of the momentarily-loaded
        // terminal voltage.
        if (battery_present()) {
            if (cfg.charge_limit_en) {
                if (!s_charge_cutoff_active && level >= (int)cfg.charge_limit_pct) {
                    bsp_set_charge_en(false);
                    s_charge_cutoff_active = true;
                    ESP_LOGI(TAG, "battery care: charging stopped at %d%% (limit %u%%)",
                             level, (unsigned)cfg.charge_limit_pct);
                } else if (s_charge_cutoff_active &&
                           level < (int)cfg.charge_limit_pct - CHARGE_LIMIT_HYSTERESIS_PCT) {
                    bsp_set_charge_en(true);
                    s_charge_cutoff_active = false;
                    ESP_LOGI(TAG, "battery care: charging resumed at %d%% (limit %u%%)",
                             level, (unsigned)cfg.charge_limit_pct);
                }
            } else if (s_charge_cutoff_active) {
                // Feature turned off mid-cutoff - restore normal charging.
                bsp_set_charge_en(true);
                s_charge_cutoff_active = false;
            }
        }

        // Resource-monitor overlay: only bother formatting when it's shown -
        // the drawer's own snprintf can wait, this one runs unconditionally
        // every second so skip the work when nobody's watching it. The
        // "< 16KB" crash-risk line is a static reminder of this project's own
        // documented failure floor (see CLAUDE.md's internal-RAM-exhaustion
        // history), not a live value.
        if (cfg.resmon_en) {
            size_t ram_free   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t ram_min    = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t psram_max  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

            if (s_sd_poll_countdown <= 0) {
                s_sd_ok = sd_archive_get_free_bytes(&s_sd_free_b, &s_sd_total_b);
                s_sd_poll_countdown = SD_POLL_INTERVAL_S;
            }
            s_sd_poll_countdown--;

            char rbuf[220];
            int n = snprintf(rbuf, sizeof(rbuf),
                     "RAM min/free: %u/%u KB\n"
                     "WiFi & USB crash risk < 16KB\n"
                     "PSRAM free/max: %u/%u MB\n",
                     (unsigned)(ram_min / 1024), (unsigned)(ram_free / 1024),
                     (unsigned)(psram_free / (1024 * 1024)), (unsigned)(psram_max / (1024 * 1024)));
            if (n > 0 && (size_t)n < sizeof(rbuf)) {
                if (s_sd_ok) {
                    snprintf(rbuf + n, sizeof(rbuf) - n, "SD free/max: %.0f/%.0f GB",
                             (double)s_sd_free_b / (1024.0 * 1024.0 * 1024.0),
                             (double)s_sd_total_b / (1024.0 * 1024.0 * 1024.0));
                } else {
                    snprintf(rbuf + n, sizeof(rbuf) - n, "SD free/max: no card");
                }
            }
            ui_set_resource_monitor_text(rbuf);
        }

        if (level < 0) {
            snprintf(left, sizeof(left), "--%%");
        } else if (mv < 0) {
            snprintf(left, sizeof(left), "%d%%%s", level,
                     charging ? "  " LV_SYMBOL_CHARGE : "");
        } else {
            snprintf(left, sizeof(left), "%d%% (%d.%dV)%s",
                     level, mv / 1000, (mv / 100) % 10,
                     charging ? "  " LV_SYMBOL_CHARGE : "");
        }
        const char *batt_icon = (level < 0) ? LV_SYMBOL_BATTERY_EMPTY : battery_glyph(level);

        // --- CENTER: UTC time HH:MM:SS + time-source indicator ---
        time_t now = time(NULL);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        bool time_valid = tm_utc.tm_year > 100;  // sane only after sync (year > 2000)

        const char *clk_suffix;
        // Effective (current-authority) source, not the last one-off writer - so
        // a stray manual/FT8 nudge doesn't leave the label stuck on FT8 while
        // SNTP/GPS is really in charge.
        switch (time_sync_get_effective_source()) {
            case TIME_SOURCE_SNTP:   clk_suffix = " UTC(NTP)"; break;
            // QMX source: GPS when auto-detected as GPS-disciplined, else the
            // plain-QMX RTC (naive offline fallback).
            case TIME_SOURCE_QMX:    clk_suffix = time_sync_qmx_gps_confirmed() ? " UTC(GPS)" : " UTC(QMX)"; break;
            case TIME_SOURCE_RTC:    clk_suffix = " UTC(RTC)"; break;
            case TIME_SOURCE_MANUAL: clk_suffix = " UTC(MAN)"; break;
            case TIME_SOURCE_FT8:
                // Marked "FT8" historically, but the sync can come from either
                // protocol's slot timing now that FT4's offset calc is fixed -
                // label it by the sub-mode actually active, not the constant name.
                clk_suffix = (ft8_op_mode_get() == FT8_OP_MODE_FT4) ? " UTC(FT4)" : " UTC(FT8)";
                break;
            default:                 clk_suffix = " UTC";      break;
        }

        // --- RIGHT: strength fan (drawn by ui.c from rssi) + SSID, then the IP.
        // No dBm number any more - the fan carries the strength, and the space
        // it used to take goes to the SSID, which needs it far more.
        const char *ssid = wifi_get_ssid();
        int rssi = wifi_get_rssi_dbm();
        const char *ip = wifi_get_ip();
        bool connected = wifi_is_connected() && ssid[0];

#if BENCH_WIFI_BAR
        // Bench hook (see BENCH_WIFI_BAR at the top of this file): fakes a
        // connected link so the layout that only exists when associated - the
        // four fan levels, a deliberately over-long SSID, the right-edge IP -
        // can be eyeballed on a desk with no access point in range.
        static int bench_tick = 0;
        static const int bench_dbm[4] = { -95, -85, -70, -55 };  // 0, 1, 2, 3 lit
        rssi      = bench_dbm[(bench_tick++ / 3) % 4];           // one level per 3 s
        ssid      = "LongHouseholdSSID-5GHz";
        ip        = "192.168.123.123";
        connected = true;
#endif
        if (connected) {
            snprintf(ssid_buf, sizeof(ssid_buf), "%s", ssid);
            snprintf(suffix_buf, sizeof(suffix_buf), "%s", ip);
        } else {
            snprintf(ssid_buf, sizeof(ssid_buf), "off");
            suffix_buf[0] = '\0';
        }

        // No battery pack attached (cheaper SKU run from USB): show a static
        // struck-through battery and skip the level/blink logic, so the icon
        // doesn't flicker empty<->full on the erratic rail voltage.
        if (!battery_present()) {
            ui_set_bottom_battery_absent();
        } else {
            // Low battery: blink the icon (off every other second). Percentage
            // text stays as-is.
            blink_on = !blink_on;
            if (level >= 0 && level < 30 && !blink_on) {
                ui_set_bottom_battery("", battery_color(level), left);
            } else {
                ui_set_bottom_battery(batt_icon, battery_color(level), left);
            }
        }
        ui_set_bottom_clock(tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, time_valid, clk_suffix);
        ui_set_bottom_wifi(ssid_buf, connected, rssi, suffix_buf);
        // Bluetooth, next to it. "Started" and "a mouse is on the other end"
        // are separate facts and the glyph shows both.
        {
            qmx_settings_t bs;
            settings_load_all(&bs);
            ui_set_bottom_bt(bs.bt_mouse_en && bt_hid_mouse_started(),
                             hid_cursor_present());
        }
    }
}

void status_bar_start(void)
{
    ui_set_bottom_version(esp_app_get_description()->version);
    ui_set_update_tap_cb(update_line_tap);   // #218
    // The bottom-bar SD-backup dot is synced once in app_main (after ui_init)
    // and driven live by the sd_archive task on mount/unmount.
    psram_task_create(status_task, "status", 4096, NULL, 2, tskNO_AFFINITY);
}
