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
#include "psram_task.h"

static const char *TAG = "usb_replug";

esp_err_t usb_replug(uint32_t off_ms)
{
    if (off_ms < 200)  off_ms = 200;
    if (off_ms > 8000) off_ms = 8000;

    // ORDER MATTERS - VBUS cycle FIRST, root-port cycle LAST. The first
    // version did port-off -> VBUS-off -> wait -> VBUS-on -> port-on, and
    // was caught on hardware sabotaging a healthy reconnect: the host lib
    // auto-repowers the root port right after our power-off (its own port
    // recovery - which is also why our later power-on returns
    // ESP_ERR_INVALID_STATE "already powered"), so enumeration raced ahead
    // WHILE VBUS was still cut, and the QMX - answering a descriptor
    // request as its VBUS died - returned 8 of 16 bytes -> ENUM
    // CHECK_SHORT_DEV_DESC FAILED -> the host abandoned the device with no
    // retry, permanently. With VBUS restored and stable BEFORE any port
    // action, whatever enumeration follows sees a solid bus.
    ESP_LOGW(TAG, "replug: VBUS off (%lums)", (unsigned long)off_ms);
    bsp_set_usb_5v_en(false);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
    bsp_set_usb_5v_en(true);
    vTaskDelay(pdMS_TO_TICKS(150));   // VBUS rise + device-side settle

    esp_err_t err_off = usb_host_lib_set_root_port_power(false);
    if (err_off != ESP_OK)
        ESP_LOGW(TAG, "root port power off: %s", esp_err_to_name(err_off));
    vTaskDelay(pdMS_TO_TICKS(50));
    // The lib may have re-powered the port on its own during recovery -
    // ESP_ERR_INVALID_STATE ("already powered") is success for our purpose.
    esp_err_t err_on = usb_host_lib_set_root_port_power(true);
    ESP_LOGW(TAG, "replug: done (port on: %s)", esp_err_to_name(err_on));
    return (err_on == ESP_ERR_INVALID_STATE) ? ESP_OK : err_on;
}

// Stale-QMX detector. Hardware findings 2026-08-03 (full story in memory
// project_qmx_reenumerate_after_reboot + TODO #74): after SOME Tab5 warm
// reboots the QMX's own USB stack answers every fresh enumeration with 8 of
// the 16 requested device-descriptor bytes (ENUM: CHECK_SHORT_DEV_DESC
// FAILED) - persistently, across host bus resets, root-port power cycles,
// and USB5V_EN cuts up to 8 s. Nothing the Tab5 can do clears it; only a
// QMX power cycle (or cable replug - untested) does. Automatic replugging
// was therefore REMOVED: it cannot cure the wedge, and interrupting a
// healthy first enumeration (which usually succeeds after a reboot) risks
// INDUCING it. What the Tab5 can do is tell the operator plainly, instead
// of sitting on a dead-looking screen: a device is attached that never
// became a QMX (CDC) or a mouse (HID) -> that is exactly the wedge
// signature -> toast a clear instruction.
#include "cat.h"
#include "usb_hid_mouse.h"
#include "ui.h"
#include "diag_log.h"

#define DET_CHECK_MS   15000
#define DET_RETOAST_MS 300000  // remind every 5 min while it persists

static void usb_stale_detect_task(void *arg)
{
    (void)arg;
    // The wedge's only reliable signal is the driver's enumeration-failure
    // log line (tallied by diag_log's vprintf hook): the failed device is
    // freed with no retry, so usb_host_lib_info() afterwards shows the same
    // empty bus as "nothing plugged in". Baseline bookkeeping: while
    // anything usable is connected (QMX or mouse), adopt the current tally
    // - so old failures never nag after a later successful connect or an
    // ordinary unplug; only failures with nothing usable connected do.
    uint32_t baseline = 0;
    int      stale_checks = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DET_CHECK_MS));

        uint32_t fails = diag_log_usb_enum_failures();
        if (cat_is_ready() || usb_hid_mouse_present()) {
            baseline = fails;
            stale_checks = 0;
            continue;
        }
        if (fails <= baseline) { stale_checks = 0; continue; }

        // Require the condition on TWO consecutive checks before warning: a
        // healthy fresh connect can tick one benign enumeration failure a
        // few seconds before CAT finishes opening (hardware-observed on a
        // QMX power-up: fail at 4.5 s, CDC ready at ~19 s - the single-check
        // version toasted right into that window). A real wedge lasts
        // forever, so 15 s of patience costs nothing.
        if (++stale_checks < 2) continue;

        ESP_LOGW(TAG, "USB enumeration failed (%lu since boot) and QMX is not "
                      "connected - stale QMX USB state, needs a QMX power cycle",
                 (unsigned long)fails);
        ui_toast("QMX USB is stuck - power-cycle the QMX to reconnect");
        stale_checks = 0;
        vTaskDelay(pdMS_TO_TICKS(DET_RETOAST_MS - DET_CHECK_MS));
    }
}

void usb_replug_watchdog_start(void)
{
    psram_task_create(usb_stale_detect_task, "usb_stale_det", 4096, NULL,
                      2, tskNO_AFFINITY);
}
