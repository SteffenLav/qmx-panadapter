#pragma once
#include <stdint.h>
#include "esp_err.h"

// Emulate a physical USB-A unplug/replug: DWC root-port power cycle +
// USB5V_EN (VBUS) cut for off_ms (clamped 200..8000 ms). Requires the USB
// host library to be installed. Safe while devices are connected - they
// see an ordinary disconnect. Returns the root-port power-on result.
esp_err_t usb_replug(uint32_t off_ms);

// Background watchdog: replug the port whenever NOTHING has enumerated for
// ~30 s (never fires while any device - QMX or mouse - is enumerated).
// Call once after the USB host is installed.
void usb_replug_watchdog_start(void);
