#include "status.h"
#include "battery.h"
#include "wifi.h"
#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui.h"
#include "esp_app_desc.h"

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

// Dot prefix removed: U+25CF isn't in the Montserrat build (renders as tofu)
// and LVGL 9 dropped inline recolor so it carried no signal value. WiFi symbol alone suffices.
static const char *rssi_dot_prefix(int rssi, bool connected)
{
    (void)rssi; (void)connected;
    return "";
}

static void status_task(void *arg)
{
    (void)arg;
    char left[96];
    char ssid_buf[64];
    char suffix_buf[80];

    // We use coloured-text formatting in the right label only; the static label
    // style needs recolor enabled, but the runtime API lv_label_set_recolor()
    // is what we need. We set it from the UI side. Here we just format strings.

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // --- LEFT: battery icon + percentage (+ charge glyph) ---
        int  level    = battery_get_level();
        int  mv       = battery_get_mv();
        bool charging = battery_is_charging();
        if (level < 0) {
            snprintf(left, sizeof(left), "%s --%%", LV_SYMBOL_BATTERY_EMPTY);
        } else if (mv < 0) {
            snprintf(left, sizeof(left), "%s %d%%%s",
                     battery_glyph(level), level,
                     charging ? "  " LV_SYMBOL_CHARGE : "");
        } else {
            snprintf(left, sizeof(left), "%s %d%% (%d.%dV)%s",
                     battery_glyph(level), level, mv / 1000, (mv / 100) % 10,
                     charging ? "  " LV_SYMBOL_CHARGE : "");
        }

        // --- CENTER: UTC time HH:MM:SS ---
        time_t now = time(NULL);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        bool time_valid = tm_utc.tm_year > 100;  // sane only after SNTP sync (year > 2000)

        // --- RIGHT: WiFi symbol + SSID, then jitter-free RSSI, then "dBm  IP" ---
        const char *ssid = wifi_get_ssid();
        int rssi = wifi_get_rssi_dbm();
        const char *ip = wifi_get_ip();
        bool connected = wifi_is_connected();
        const char *dot = rssi_dot_prefix(rssi, connected);
        bool show_rssi = connected && ssid[0];
        if (show_rssi) {
            snprintf(ssid_buf, sizeof(ssid_buf), "%s%s %s ", dot, LV_SYMBOL_WIFI, ssid);
            snprintf(suffix_buf, sizeof(suffix_buf), "dBm  %s", ip);
        } else {
            snprintf(ssid_buf, sizeof(ssid_buf), "%s%s off", dot, LV_SYMBOL_WIFI);
            suffix_buf[0] = '\0';
        }

        ui_set_bottom_left(left);
        ui_set_bottom_clock(tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, time_valid);
        ui_set_bottom_wifi(ssid_buf, show_rssi, rssi, suffix_buf);
    }
}

void status_bar_start(void)
{
    ui_set_bottom_version(esp_app_get_description()->version);
    xTaskCreate(status_task, "status", 4096, NULL, 2, NULL);
}
