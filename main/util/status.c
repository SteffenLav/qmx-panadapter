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
    char left[64];
    char center[32];
    char right[128];

    // We use coloured-text formatting in the right label only; the static label
    // style needs recolor enabled, but the runtime API lv_label_set_recolor()
    // is what we need. We set it from the UI side. Here we just format strings.

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // --- LEFT: battery icon + percentage (+ charge glyph) ---
        int  level    = battery_get_level();
        bool charging = battery_is_charging();
        if (level < 0) {
            snprintf(left, sizeof(left), "%s --%%", LV_SYMBOL_BATTERY_EMPTY);
        } else {
            snprintf(left, sizeof(left), "%s %d%%%s",
                     battery_glyph(level), level,
                     charging ? "  " LV_SYMBOL_CHARGE : "");
        }

        // --- CENTER: UTC time HH:MM:SS ---
        time_t now = time(NULL);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        if (tm_utc.tm_year > 100) {  // sane only after SNTP sync (year > 2000)
            snprintf(center, sizeof(center), "%02d:%02d:%02d UTC",
                     tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
        } else {
            snprintf(center, sizeof(center), "--:--:-- UTC");
        }

        // --- RIGHT: coloured dot + WiFi symbol + SSID + RSSI + IP ---
        const char *ssid = wifi_get_ssid();
        int rssi = wifi_get_rssi_dbm();
        const char *ip = wifi_get_ip();
        bool connected = wifi_is_connected();
        const char *dot = rssi_dot_prefix(rssi, connected);
        if (connected && ssid[0]) {
            snprintf(right, sizeof(right), "%s%s %s %ddBm  %s",
                     dot, LV_SYMBOL_WIFI, ssid, rssi, ip);
        } else {
            snprintf(right, sizeof(right), "%s%s off", dot, LV_SYMBOL_WIFI);
        }

        ui_set_bottom_left(left);
        ui_set_bottom_center(center);
        ui_set_bottom_right(right);
    }
}

void status_bar_start(void)
{
    xTaskCreate(status_task, "status", 4096, NULL, 2, NULL);
}
