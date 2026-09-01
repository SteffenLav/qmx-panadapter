#pragma once
#include <stdbool.h>

// Shut the USB host down in an orderly way, so the QMX is told the party is over
// instead of discovering it.
//
// WHY THIS EXISTS
// Re-flashing the ESP32-P4 makes the USB host vanish mid-transfer: no bus reset,
// no suspend, no disconnect - the host simply stops existing, while UAC
// isochronous audio and CDC-ACM are both streaming. After that the QMX
// intermittently answers the next enumeration with an EMPTY DATA STAGE
// (GET_DESCRIPTOR(DEVICE) wLength=8 at address 0 returning zero bytes), and
// stays that way until it is power-cycled. Reproduced on QMX 1_03_002 and
// 1_04_004, and it survives VBUS being cut for 8 seconds.
//
// Roger AD5DZ and Stan, on the QRP Labs list, both asked the obvious question:
// why not shut down cleanly before reflashing, instead of asking the radio's USB
// stack to survive the host disappearing? They are right, and it had not been
// tried. This is that.
//
// WHAT ORDERLY MEANS HERE, in the order it has to happen:
//   1. the radio goes back to RX     - a host that dies mid-burst leaves it keyed
//   2. CAT closes                    - poll-task-aware, or the close races a retry
//   3. the audio stream stops        - alt setting 0, the device's cue to stop
//                                      producing isochronous packets
//   4. VBUS drops                    - a real disconnect the device can see
//
// WHAT IT CANNOT DO
// It cannot help if the operator just re-flashes: esptool resets the chip from
// outside and the firmware gets no warning. That is why this is offered as a
// deliberate action ("Prepare for flashing" in the drawer, or the web UI) AND
// registered as an esp_restart() shutdown handler, so every reboot the firmware
// itself initiates is already orderly.

// Do it. Blocking, bounded to roughly two seconds. Safe to call with nothing
// open, and safe to call twice. Returns true if a USB device was actually torn
// down (i.e. there was something to shut down).
bool usb_shutdown_graceful(void);

// Register usb_shutdown_graceful() to run on esp_restart(). Call once at start-up.
void usb_shutdown_install_handler(void);
