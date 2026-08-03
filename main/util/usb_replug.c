// USB-A "replug" - emulate a physical unplug/replug of the USB-A port
// without touching the cable. Two levers pulled together:
//   1. usb_host_lib_set_root_port_power(false/true): the DWC root port is
//      taken through a real detach -> attach -> debounce -> bus-reset cycle,
//      so the host-side port FSM re-runs connection detection from scratch
//      (a warm ESP32 reboot otherwise leaves the QMX's stale device state
//      invisible - the long-standing "QMX needs a power cycle after every
//      Tab5 reboot").
//   2. bsp_set_usb_5v_en(false/true): the PI4IO expander's USB5V_EN (P3)
//      cuts the port's VBUS meanwhile, for any device that does watch VBUS.
//      (Hardware-tested alone it does NOT wake a wedged QMX - the QMX is
//      self-powered and appears not to re-attach on VBUS restore - which is
//      why the root-port cycle is the primary lever.)
//
// Used by the boot path (main.c) and the hidden /api/cmd "usb_replug"
// action. Safe with devices connected and streaming: powering the root
// port off delivers ordinary disconnect events, the exact code path a
// physical unplug already exercises.

#include "usb_replug.h"

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "m5stack_tab5.h"

static const char *TAG = "usb_replug";

esp_err_t usb_replug(uint32_t off_ms)
{
    if (off_ms < 200)  off_ms = 200;
    if (off_ms > 8000) off_ms = 8000;

    ESP_LOGW(TAG, "replug: root port power OFF + VBUS off (%lums)",
             (unsigned long)off_ms);
    esp_err_t err_off = usb_host_lib_set_root_port_power(false);
    if (err_off != ESP_OK)
        ESP_LOGW(TAG, "root port power off: %s", esp_err_to_name(err_off));

    bsp_set_usb_5v_en(false);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
    bsp_set_usb_5v_en(true);
    vTaskDelay(pdMS_TO_TICKS(100));   // VBUS rise before the port looks again

    esp_err_t err_on = usb_host_lib_set_root_port_power(true);
    ESP_LOGW(TAG, "replug: root port power ON: %s", esp_err_to_name(err_on));
    return err_on;
}
