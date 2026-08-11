// See usb_shutdown.h.

#include "usb_shutdown.h"

#include "cat.h"
#include "audio.h"
#include "ft8_tx.h"

#include "usb/usb_host.h"
#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_shutdown";

static volatile bool s_done = false;

bool usb_shutdown_graceful(void)
{
    // Idempotent: the drawer button and the restart handler can both fire for one
    // reboot, and tearing down twice would close handles that are already gone.
    if (s_done) {
        ESP_LOGI(TAG, "already shut down");
        return false;
    }
    s_done = true;

    bool had_device = cat_is_ready() || audio_uac_active();
    ESP_LOGW(TAG, "orderly USB shutdown starting (device present: %s)",
             had_device ? "yes" : "no");

    // 1. Nothing armed may fire once the link is going away. Disarm before the
    //    CAT close, so the slot loop cannot start a burst into a closing handle.
    ft8_tx_disarm();

    // 2. CAT: sends TA0;RX; so the radio is never left keyed, then closes with
    //    the poll-task-aware sequence (see cat_usb_shutdown).
    cat_usb_shutdown();

    // 3. Audio: alt setting 0 tells the QMX to stop producing isochronous
    //    packets. This is the step the vanishing host never performs, and the
    //    one most likely to be what leaves the radio's stack stuck.
    audio_usb_shutdown();

    // 4. Drop VBUS. Now that both interfaces are closed, this is a disconnect the
    //    device can actually observe, rather than a power cut mid-transfer - the
    //    distinction util/usb_replug.c learned the hard way, where cutting VBUS
    //    while a descriptor request was in flight produced the very ENUM failure
    //    we are trying to avoid.
    esp_err_t err = usb_host_lib_set_root_port_power(false);
    ESP_LOGI(TAG, "root port power off: %s", esp_err_to_name(err));

    // NOT cutting the physical 5 V here, and that is a measured decision.
    //
    // usb_host_lib_set_root_port_power() above is the DWC root port - a LOGICAL
    // disable. The real VBUS switch is the PI4IO expander's USB5V_EN (P3), which only
    // util/usb_replug.c ever touches, so this function has never removed power from the
    // connector. That looked like the missing piece for the #74 wedge and it was TRIED
    // on hardware 2026-08-11: interfaces closed properly, then VBUS off, held down
    // through the reflash AND the whole boot (the expander retains state; P3 goes high
    // again in bsp_io_expander_pi4ioe_init), so the radio got a genuine multi-second
    // VBUS session end followed by a fresh start - the closest thing to a physical
    // unplug this board can produce without a hand on the cable.
    //
    // The QMX still answered the next enumeration with an empty data stage. So it buys
    // nothing, and left in it would be a footgun: press "Prepare for flashing", change
    // your mind, and USB stays dead until a reboot with nothing on screen saying why.
    // Reverted. See TODO #74 for the six approaches falsified.

    // Let the device see the bus go idle before anything resets the SoC.
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_LOGW(TAG, "orderly USB shutdown complete - safe to reflash or power off");
    return had_device;
}

static void shutdown_handler(void)
{
    // Runs from esp_restart() with the scheduler still up, so the bounded waits
    // inside the teardown are fine. Covers every reboot the firmware initiates
    // (settings reset, network reset, an OTA); it cannot cover esptool, which
    // resets the chip from outside with no warning at all.
    usb_shutdown_graceful();
}

void usb_shutdown_install_handler(void)
{
    esp_err_t err = esp_register_shutdown_handler(shutdown_handler);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "could not register restart handler: %s", esp_err_to_name(err));
}
