#pragma once
#include <stdint.h>
#include "esp_err.h"

// Emulate a physical USB-A unplug/replug: DWC root-port power cycle +
// USB5V_EN (VBUS) cut for off_ms (clamped 200..8000 ms). Requires the USB
// host library to be installed. Safe while devices are connected - they
// see an ordinary disconnect. Returns the root-port power-on result.
esp_err_t usb_replug(uint32_t off_ms);

// Background stale-QMX detector: when a USB device is attached but has not
// become an open QMX (CDC) or mouse (HID) for ~30 s - the signature of the
// QMX's stale-USB-stack wedge, which no host-side action can clear - log +
// toast a "power-cycle the QMX" instruction (repeated every 5 min while it
// persists). Deliberately does NOT auto-replug (see usb_replug.c).
// Call once after the USB host is installed.
void usb_replug_watchdog_start(void);
