#include "status.h"
#include "battery.h"
#include "wifi.h"
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

extern void ui_set_fps_text(const char *text);

static void status_task(void *arg)
{
    char buf[160];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        int  level    = battery_get_level();
        bool charging = battery_is_charging();
        const char *ssid = wifi_get_ssid();
        int rssi = wifi_get_rssi_dbm();
        const char *ip = wifi_get_ip();
        bool connected = wifi_is_connected();

        char bat_part[40];
        if (level < 0) {
            snprintf(bat_part, sizeof(bat_part), "BAT --%%");
        } else {
            snprintf(bat_part, sizeof(bat_part), "BAT %d%%%s",
                     level, charging ? " " LV_SYMBOL_CHARGE : "");
        }

        char wifi_part[80];
        if (connected && ssid[0]) {
            snprintf(wifi_part, sizeof(wifi_part), "WiFi %s %ddBm %s", ssid, rssi, ip);
        } else {
            snprintf(wifi_part, sizeof(wifi_part), "WiFi off");
        }

        snprintf(buf, sizeof(buf), "%s   %s", bat_part, wifi_part);
        ui_set_fps_text(buf);
    }
}

void status_bar_start(void)
{
    xTaskCreate(status_task, "status", 4096, NULL, 2, NULL);
}