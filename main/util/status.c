#include "status.h"
#include "display.h"
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

// How long "Failed - tap retries" blinks red/white before quietly reverting
// to the plain version line. Long enough to be seen even away from the bar
// for a few seconds, short enough that an abandoned failure does not sit in
// red forever. Tap-and-hold still retries after this - see the OTA_FAILED
// branch in status_task().
#define OTA_FAILED_SHOW_S 3
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
// #218: what a long press on the bottom-bar update line means.
//
// LONG PRESS, not a tap, and that is the operator's design. The band-plan strip
// is 22 px and sits directly on top of this bar, and a tap on IT retunes - so a
// finger reaching for the update line and landing slightly high moved the dial.
// A hold cannot be triggered by brushing past, it announces itself with
// "release to confirm" while the finger is still down, and lifting early
// cancels it. It also composes with the swipe already on this strip, because
// that one is a DRAG: hold still to update, drag up for Memory Channels.
//
// Nothing is ever downloaded without that deliberate act: the first state
// OFFERS a download rather than being one already in progress.
// ---------------------------------------------------------------------------
static void update_line_tap(void)
{
    // Called only after a completed LONG PRESS (ui.c), which is the
    // confirmation: it cannot happen by brushing past, it showed "release to
    // confirm" while the finger was down, and lifting early cancels. So there
    // is no second gate here - a two-tap arm on top would just be a hidden
    // state the operator has to remember.
    int  pct = 0;
    static char msg[128], latest[32];
    ota_state_t st = ota_update_get_state(&pct, msg, sizeof(msg));

    if (st == OTA_RUNNING) return;          // nothing sensible to do mid-download

    if (st == OTA_DONE) {
        ESP_LOGW("status", "operator confirmed restart into the new firmware");

        // Say what is happening, then GO DARK BEFORE RESTARTING.
        //
        // The operator saw "a clear cyan screen for 2-3 seconds" and called it
        // intrusive - rightly. display_init() sets the backlight to 0 precisely
        // so the panel's uninitialised content is never shown, but that call is
        // ~2-3 s into boot; across esp_restart() the backlight simply stays on
        // from the previous run and lights up a panel with nothing in it.
        // Nobody noticed before because a reboot was a rare event; #218 makes it
        // a normal one, so it has to look deliberate.
        ui_update_line_force("restarting...", 0x8FE0A0);
        vTaskDelay(pdMS_TO_TICKS(500));      // long enough to read
        display_set_brightness(0);
        vTaskDelay(pdMS_TO_TICKS(80));       // let the panel actually go dark
        esp_restart();
    }

    update_check_get_latest(latest, sizeof(latest));

    // NOTHING TO INSTALL -> the long press means "check now" instead.
    //
    // Without this the gesture simply did nothing whenever the device was up to
    // date, which is the state it is in almost all the time - so on the Tab5
    // itself there was no way to act on an announcement at all. A tester who
    // reads the release post and walks over to the radio should not need a
    // browser to ask the question, on a device whose whole point is working
    // without a laptop.
    //
    // The check runs on update_check's own task; this only asks. The 1 Hz
    // refresh above repaints the line either way, so "checking..." is replaced
    // by the version again, or by the cyan offer.
    if (!latest[0] || !update_check_available()) {
        ESP_LOGI("status", "operator asked for an update check");
        ui_update_line_force("checking...", 0x40D8E0);
        update_check_now();
        return;
    }

    static char url[192];
    snprintf(url, sizeof(url),
             "https://github.com/SteffenLav/qmx-panadapter/releases/download/%s/qmx_panadapter.bin",
             latest);
    static char err[96];
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
    // #<pending>: how long "Failed - tap retries" has been showing. A
    // correctly-registered long-press followed by an instantly-failed
    // download (the exact v1.9.0/v1.9.1 OTA bug) LOOKED identical to the
    // press not registering at all - the diag log proved the press fired
    // every time, but a static red line at the far end of the bottom bar,
    // outside where the operator was watching (their own thumb, mid-press),
    // was easy to miss entirely. Blinking makes it impossible to miss even
    // glancing back a few seconds later; the auto-revert after
    // OTA_FAILED_SHOW_S keeps a genuinely abandoned failure from sitting in
    // red forever - tap-and-hold still retries afterward regardless, since
    // that reads ota_update_get_state() fresh and the backend's FAILED state
    // is untouched by this purely cosmetic timeout.
    uint32_t ota_failed_ticks = 0;
    uint32_t last_fail_seq    = 0;   // last ota_update_get_fail_seq() we've seen

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
            static char vline[96];   // two versions plus an arrow - 64 truncates
            // The running version, with nothing to report. Was 0x808080, a
            // mid-grey noticeably dimmer than everything else on this bar -
            // the operator's words were "grey (dim) and not white-ish". It is
            // not a warning and not an afterthought, it is simply which
            // firmware this is, so it reads like the rest of the bar. The
            // colours below still carry the states that DO mean something.
            // 0xC0C0C0 is UI_COLOR_TEXT_SECONDARY, spelled literally because
            // this file deliberately does not depend on ui_theme.h - every
            // other state below is a literal for the same reason.
            uint32_t vcol = 0xC0C0C0;
            // ⛔ STATIC, NOT STACK. status_task's stack is 4096 bytes (crash
            // dump gave bounds 0x481b97b4-0x481ba7b0 = 4092), and the ~384
            // bytes of buffers here - added for #218 - overflowed it: "Stack
            // protection fault ... task status", 20 minutes in. Exactly the
            // class CLAUDE.md warns about ("a multi-hundred-byte local is a bug
            // until proven otherwise"), and the remedy it prescribes: a
            // file-local static is safe because status_task is the ONLY caller
            // and there is exactly one of it.
            // The RUNNING version is shown VERBATIM, not shortened - a dev
            // build's "-N-gHASH[-dirty]" suffix is exactly the fact that was
            // hidden here before, and hiding it is what let the dev Tab5 run
            // a drifted build (git HEAD one commit past the v1.9.1 tag) while
            // both screens still read a clean "v1.9.1", indistinguishable from
            // the real release. over_s/latest_s stay shortened - those always
            // come from a published release tag, never a local build, so
            // there is nothing to hide there in the first place.
            static char running[48], over_s[32], latest_s[32];
            snprintf(running, sizeof(running), "%s", esp_app_get_description()->version);

            int  opct = 0;
            static char omsg[128], over[32], latest[32];
            ota_state_t ost = ota_update_get_state(&opct, omsg, sizeof(omsg));
            // A NEW failure resets the pulse counter regardless of what ost
            // was doing in between - see ota_update_get_fail_seq()'s own
            // comment: a fast failure (bad hostname, under 100ms measured)
            // can complete entirely between two 1 Hz ticks with no
            // observable OTA_RUNNING in the middle, so ost alone cannot
            // tell "brand new failure" from "still the same one as before".
            {
                uint32_t seq = ota_update_get_fail_seq();
                if (seq != last_fail_seq) {
                    last_fail_seq    = seq;
                    ota_failed_ticks = 0;
                }
            }
            ota_update_get_target_version(over, sizeof(over));
            update_check_get_latest(latest, sizeof(latest));
            short_ver(over,   over_s,   sizeof(over_s));
            short_ver(latest, latest_s, sizeof(latest_s));


            // Wording and colours are the operator's, chosen to fit at the
            // ORIGINAL montserrat_24 - every state below is ~20 characters,
            // where "touch to update" was 24 and overlapped the clock.
            // Set true only by the pulsing OTA_FAILED branch, which already
            // called ui_set_update_line_failed() itself - the plain
            // ui_set_update_line() call at the end of this block would
            // otherwise immediately stop the pulse it just started.
            bool skip_plain_update_line = false;
            if (ost == OTA_RUNNING) {
                ota_failed_ticks = 0;
                if (running[0] && over_s[0])
                    snprintf(vline, sizeof(vline), "%s " LV_SYMBOL_RIGHT " %s  %d%%",
                             running, over_s, opct);
                else
                    snprintf(vline, sizeof(vline), "updating  %d%%", opct);
                vcol = 0xFFA040;                       // amber - working
            } else if (ost == OTA_DONE) {
                ota_failed_ticks = 0;
                if (over_s[0]) snprintf(vline, sizeof(vline), "%s - tap updates", over_s);
                else              snprintf(vline, sizeof(vline), "tap updates");
                vcol = 0x8FE0A0;                       // light green - ready
            } else if (ost == OTA_FAILED && ota_failed_ticks < OTA_FAILED_SHOW_S) {
                // A correctly-registered long-press followed by an instantly
                // failed download (v1.9.0/v1.9.1's own OTA bug) was
                // indistinguishable from the press not registering at all -
                // the diag log proved the press fired every time; a static
                // red line was just easy to miss. A 1 Hz red/white colour
                // swap turned out to be far too subtle over a short window
                // (changes at most 2-3 times - a glance mid-cycle just reads
                // "red"), so this now PULSES (ui_set_update_line_failed(),
                // a fast opacity animation, not tied to this 1 Hz tick) for
                // OTA_FAILED_SHOW_S seconds, unmistakable even at a glance.
                // "Server busy" rather than "Failed": the operator did
                // nothing wrong, and this IS what happened (a connection
                // that could not be reached), not a vague failure.
                ota_failed_ticks++;
                snprintf(vline, sizeof(vline), "Server busy - tap retries");
                ui_set_update_line_tappable(true);
                ui_set_update_line_failed(vline);
                skip_plain_update_line = true;
            } else if (ost == OTA_FAILED) {
                // Given up being loud about it, but tap-and-hold still
                // retries from here - update_line_tap() reads the backend
                // state fresh and it is untouched by this display-only
                // timeout, it just no longer LOOKS different from normal.
                snprintf(vline, sizeof(vline), "%s", running);
            } else if (update_check_available() && latest_s[0]) {
                ota_failed_ticks = 0;
                snprintf(vline, sizeof(vline), "%s " LV_SYMBOL_RIGHT " %s  tap?",
                              running, latest_s);
                vcol = 0x40D8E0;                       // cyan - offered, nothing fetched
            } else {
                ota_failed_ticks = 0;
                snprintf(vline, sizeof(vline), "%s", running);
            }
            // Tell the UI whether there is anything to tap. While true the WHOLE
            // bottom bar accepts the tap and the band-plan strip ignores presses
            // in its lowest pixels above the label - so reaching for this cannot
            // nudge the dial (the strip is 22 px, sits on the bar, and a tap on
            // it retunes).
            // Tappable in every state EXCEPT mid-download, because the long
            // press now always means something: install, restart, retry - or,
            // when there is nothing to install, CHECK NOW (see update_line_tap).
            // Previously it was inert whenever the device was up to date, which
            // is nearly all the time, so on the Tab5 itself there was no way to
            // act on a release announcement without a browser.
            //
            // ⚠ THE COST, and it is why this was gated before: while tappable,
            // the lowest 8 px of the band-plan strip across this label's own
            // x-range stop accepting a press, so a miss aimed at the bar cannot
            // nudge the dial. That exclusion is now permanent rather than only
            // while an update is pending. It is 8 px of a 22 px strip over a
            // couple of hundred pixels of width - small, but real, and this is
            // the line to change back if tap-to-tune ever feels worse near the
            // bottom bar.
            if (!skip_plain_update_line) {
                ui_set_update_line_tappable(ost != OTA_RUNNING);
                ui_set_update_line(vline, vcol);
            }
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
    // 4096 was the historic size, and #218 added real work to this task -
    // composing the update line, two version shortenings, ota/update_check
    // queries and a wider snprintf. It crashed TWICE within an hour on 4 KB:
    // first "Stack protection fault" outright, then "spinlock_acquire
    // (lock->count == 0)" on a log call, which is what earlier corruption tends
    // to look like by the time it is noticed.
    //
    // The buffers are static now, so this is deliberate headroom rather than a
    // fix for a known overflow - and it is nearly free, because the stack lives
    // in PSRAM (28 MB spare) while the TCB stays internal. On a board whose task
    // stacks are documented as "TINY", being generous with a background task is
    // the cheap side of the trade.
    psram_task_create(status_task, "status", 8192, NULL, 2, tskNO_AFFINITY);
}
