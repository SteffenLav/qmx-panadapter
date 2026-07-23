// USB HID mouse support (Phase 1: enumeration + report logging).
//
// The QMX occupies the ESP32-P4's single USB OTG host (UAC audio + CDC-ACM
// CAT). A mouse therefore has to share that host via a powered USB hub, which
// needs CONFIG_USB_HOST_HUBS_SUPPORTED=y and the espressif/usb_host_hid driver
// layered on alongside the existing UAC + CDC-ACM classes.
//
// Phase 1 only installs the HID host, enumerates a boot-protocol mouse, and
// logs its reports + an accumulated cursor position — no LVGL yet. This proves
// the risky part (does a mouse coexist with the QMX on a hub?) before the
// cursor/indev work in Phase 2. The cursor-state getter below is here already
// so Phase 2 only needs to add the LVGL pointer indev.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Install the USB HID host driver (spawns its own background task). Call once,
// after bsp_usb_host_start(). Harmless if no mouse is ever attached.
void usb_hid_mouse_init(void);

// True while a boot-protocol mouse is enumerated and streaming.
bool usb_hid_mouse_present(void);

// Latest accumulated cursor position (landscape logical space, 0..1279 / 0..719)
// and raw button bitmask (bit0 = left). Safe from any task. For Phase 2's
// LVGL pointer read_cb.
void usb_hid_mouse_get(int *x, int *y, uint8_t *buttons);

#ifdef __cplusplus
}
#endif
