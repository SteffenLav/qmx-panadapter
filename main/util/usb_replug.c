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

// Root port only. Keeps VBUS up, so a self-powered radio never sees its supply
// disappear. Same port sequence as usb_replug()'s tail.
esp_err_t usb_replug_port_only(void)
{
    ESP_LOGW(TAG, "replug: root port only (VBUS left up)");
    esp_err_t err_off = usb_host_lib_set_root_port_power(false);
    if (err_off != ESP_OK)
        ESP_LOGW(TAG, "root port power off: %s", esp_err_to_name(err_off));
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t err_on = usb_host_lib_set_root_port_power(true);
    ESP_LOGW(TAG, "replug: done (port on: %s)", esp_err_to_name(err_on));
    return (err_on == ESP_ERR_INVALID_STATE) ? ESP_OK : err_on;
}

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
#include "audio.h"
#include "esp_timer.h"

#define DET_CHECK_MS   15000
#define DET_RETOAST_MS 300000  // remind every 5 min while it persists

static void usb_stale_detect_task(void *arg)
{
    (void)arg;
    // TWO distinct wedges, told apart by usb_host_lib_info().num_devices -
    // both hardware-verified 2026-08-03 (TODO #74/#75):
    //
    // ZOMBIE (Tab5-side, CURABLE): the QMX powered off mid-stream, the UAC
    // teardown failed ("Suspend Interface Failed" + EP command errors), the
    // device object is never freed - it occupies the single root port, so a
    // re-powered QMX is invisible and the CAT reopen spins on
    // usbh_devs_open ESP_ERR_INVALID_STATE forever. Signature: num_devices
    // > 0 with nothing opened. usb_replug() CURES this (proven live: freed
    // the zombie, QMX enumerated within a second, no reboot).
    //
    // DESCRIPTOR WEDGE (QMX-side, NOT curable from here): after some Tab5
    // warm reboots the QMX answers every enumeration with 8 of 16
    // descriptor bytes; the failed device is FREED, so the bus looks empty
    // - only the enum-failure tally from diag_log's vprintf hook sees it.
    // Replug proven useless (the QMX answers the same through bus resets,
    // port power cycles, VBUS cuts, even physical cable replugs); only a
    // QMX power cycle helps, so TELL the operator.
    //
    // The two hand off cleanly: replugging a descriptor-wedged QMX ends in
    // a failed enumeration -> device freed -> zombie branch goes quiet ->
    // enum branch toasts the power-cycle instruction.
    //
    // Baseline bookkeeping: while anything usable is connected (QMX or
    // mouse), adopt the current tally - so old failures never nag after a
    // later successful connect or an ordinary unplug. Every branch requires
    // TWO consecutive 15 s checks: a healthy connect can tick one benign
    // enum failure ~15 s before CDC finishes opening (hardware-observed),
    // and a mid-enumeration moment can look like "device present, nothing
    // open" for a few seconds.
    // Holdoffs are TIMESTAMPS, never sleeps: an early version slept this
    // task ~5 min after a toast, and a zombie created during that nap
    // (hardware-hit: operator power-cycled the QMX and ran the zombie test
    // inside the window) went unrecovered for the whole sleep. The 15 s
    // check cadence must never pause.
    uint32_t baseline = 0;
    int      wedge_checks = 0;    // enum-failure (QMX-side) branch
    int      zombie_checks = 0;   // device-present-nothing-open branch
    int      replug_attempts = 0; // per-episode cap - reset on any connect
    int      prev_devices = 0;
    int64_t  last_replug_us = 0;
    int64_t  last_toast_us  = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DET_CHECK_MS));

        uint32_t fails = diag_log_usb_enum_failures();
        // audio_uac_active() is load-bearing here, not belt-and-suspenders:
        // during a fresh connect UAC opens SECONDS before cat marks itself
        // ready (CDC open -> IQ handshake -> band scan), and a check landing
        // in that window once counted a CONNECTING QMX as a zombie and
        // replugged it mid-handshake (hardware-hit 2026-08-03) - killing a
        // healthy connect and wedging the port.
        if (cat_is_ready() || usb_hid_mouse_present() || audio_uac_active()) {
            baseline = fails;
            wedge_checks = 0;
            zombie_checks = 0;
            replug_attempts = 0;
            continue;
        }
        int64_t now = esp_timer_get_time();

        // Zombie branch: something is enumerated yet neither driver opened
        // it for 3 consecutive checks OF THE SAME DEVICE SET - a change in
        // num_devices restarts the window, so strikes counted against a
        // wedged device can never carry over to a freshly attached one
        // (that carry-over is exactly what caused the mid-connect replug
        // above). Unsupported devices (e.g. a USB stick) also land here -
        // replugging them is harmless. Max 3 replugs per episode: if three
        // didn't clear it, more won't either (hardware-observed: a port
        // stuck answering power-on with INVALID_STATE forever) - fall
        // through to the toast so the operator learns the state.
        usb_host_lib_info_t info;
        if (usb_host_lib_info(&info) == ESP_OK && info.num_devices > 0) {
            if (info.num_devices != prev_devices) {
                zombie_checks = 0;   // device set changed - fresh window
            } else if (++zombie_checks >= 3 &&
                       now - last_replug_us > 60000000LL) {
                zombie_checks = 0;

                // Is this actually a stuck PORT, or is it the RADIO refusing to
                // enumerate? Rising enum failures say the latter - #74, the
                // QMX-side descriptor wedge - and a replug against that is proven
                // useless: six approaches were falsified on hardware against a
                // really-wedged radio, this exact VBUS cut among them, and every
                // one got back the same empty data stage.
                //
                // Firing it anyway cuts VBUS for 2 s and visibly power-cycles the
                // operator's radio for no benefit. That happened on 2026-08-13 -
                // "it hooked the qmx off" - and it is the only thing this branch
                // achieved. num_devices is NON-zero after a failed enumeration,
                // which is why the wedge reaches a branch written for a zombie.
                //
                // So: leave the radio alone and say what is actually wrong. The
                // operator has to power-cycle the QMX either way, and the screen
                // already tells them so whenever CAT is down.
                if (fails > baseline) {
                    if (now - last_toast_us > (int64_t)DET_RETOAST_MS * 1000) {
                        ESP_LOGW(TAG, "USB device present but enumeration keeps failing "
                                      "(%lu since boot) - RADIO-side wedge, not a stuck "
                                      "port. Not replugging: it does not help and it "
                                      "power-cycles the radio. Power-cycle the QMX.",
                                 (unsigned long)fails);
                        last_toast_us = now;
                    }
                    prev_devices = info.num_devices;
                    continue;
                }

                if (replug_attempts < 3) {
                    replug_attempts++;
                    ESP_LOGW(TAG, "USB device present but never opened for %d s - "
                                  "zombie device state, replugging the port (%d/3)",
                             (DET_CHECK_MS * 3) / 1000, replug_attempts);
                    usb_replug(2000);
                    last_replug_us = now;
                    prev_devices = info.num_devices;
                    continue;
                }
                if (now - last_toast_us > (int64_t)DET_RETOAST_MS * 1000) {
                    // Hardware-observed: a QMX power cycle (physical detach)
                    // clears even the stuck-port state - it is the first
                    // thing to try, the Tab5 reboot only the fallback.
                    ESP_LOGW(TAG, "USB port stuck after %d replug attempts - "
                                  "needs a QMX power cycle (or Tab5 reboot)",
                             replug_attempts);
                    // NO on-screen toast (operator, v1.8.0: "irritating and for no
                    // use"). It fired every few minutes at a QMX that was simply
                    // switched off - an ordinary state, not a fault - and the
                    // screen already says "Now turn on or reboot your QMX/+"
                    // whenever CAT is down, which is the same instruction in the
                    // right place. The log line above keeps this diagnosable, and
                    // the recovery behaviour is unchanged.
                    last_toast_us = now;
                }
            }
            prev_devices = info.num_devices;
        } else {
            zombie_checks = 0;
            prev_devices = 0;
        }

        // ENUMERATION-FAILURE BRANCH - a hole worth closing, but NOT a fix for #74.
        //
        // ⚠ READ THIS BEFORE BELIEVING THE PARAGRAPH BELOW. On 2026-08-11 I told the
        // operator this branch's log-only behaviour was "the whole bug". It was not:
        // measured on hardware the same evening, the ZOMBIE branch above fires on this
        // wedge (num_devices is NON-zero after a failed enumeration - my reasoning that
        // it could not fire was simply wrong) and it replugged twice, and the QMX
        // answered both attempts with the same empty data stage. So the retry already
        // existed and does not help.
        //
        // This branch is still worth having: it covers an episode where num_devices IS
        // zero, which the zombie branch genuinely cannot see. It is a closed hole, not a
        // cure - do not cite it as one.
        //
        // What IS true: ESP-IDF's enum.c has no retry of its own - any error jumps to
        // ENUM_STAGE_CANCEL, frees the device and gives up. Linux retries up to three
        // times with escalating resets, and Espressif issue #17918 (open, no fix)
        // reports this failure class on other devices as timing-related. That is a real
        // robustness gap and this replug is our stand-in for it.
        //
        // What is NOT true is that it rescues a wedged QMX. Falsified on hardware, five
        // ways, against a really-wedged radio: reset hold 50 ms, reset hold 200 ms
        // (Linux's long-reset value), requesting 64 descriptor bytes instead of 8 the
        // way Windows and Linux do, VBUS off for 2 s plus a port power cycle, and
        // repeated retries. Every one produced the same answer - an EMPTY data stage, no
        // descriptor bytes at all, ~350 ms after port-on. The radio is not answering
        // address 0 in that state and no host-side timing changes that.
        //
        // Capped and backed off regardless, because a QMX that is merely switched off is
        // an ordinary state and must not cause endless port cycling.
        if (fails <= baseline) { wedge_checks = 0; continue; }
        if (++wedge_checks < 2) continue;
        wedge_checks = 0;

        if (replug_attempts < 3 && now - last_replug_us > 60000000LL) {
            replug_attempts++;
            ESP_LOGW(TAG, "USB enumeration failed (%lu since boot) and nothing is "
                          "connected - retrying the port (%d/3). ESP-IDF does not retry "
                          "enumeration itself; this stands in for that.",
                     (unsigned long)fails, replug_attempts);
            // PORT ONLY - no VBUS cut. Cutting VBUS here switched the operator's
            // radio off during the v1.8.2 release verification, from THIS branch,
            // after the zombie branch had already been gated for the same reason.
            // The port cycle is the part that stands in for ESP-IDF's missing enum
            // retry; the VBUS cut only ever added a power-cycled radio, and it is
            // proven not to clear the QMX-side wedge.
            usb_replug_port_only();
            last_replug_us = now;
            baseline = diag_log_usb_enum_failures();   // judge the NEXT attempt on its own
            continue;
        }

        if (now - last_toast_us < (int64_t)DET_RETOAST_MS * 1000) continue;
        ESP_LOGW(TAG, "USB enumeration still failing after %d port retries (%lu failures "
                      "since boot) - needs a QMX power cycle",
                 replug_attempts, (unsigned long)fails);
        // Log only, no toast - see the reason on the zombie branch above.
        last_toast_us = now;
    }
}

void usb_replug_watchdog_start(void)
{
    psram_task_create(usb_stale_detect_task, "usb_stale_det", 4096, NULL,
                      2, tskNO_AFFINITY);
}
