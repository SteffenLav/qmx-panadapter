#pragma once
#include <stdbool.h>
#include <stdint.h>

// Shared pointer state for every mouse the Tab5 can have.
//
// There are two transports and there will likely never be more than two: USB
// (usb_hid_mouse.c - works only when the QMX is unplugged, because a hub puts
// both behind a Transaction Translator that ESP-IDF 5.4.4 does not implement)
// and Bluetooth LE (bt_hid_mouse.c - the only way to have a mouse AND the
// radio at once, since it does not touch the USB host at all).
//
// Both produce the same thing: signed deltas and a button bitmask. Keeping the
// accumulation here means LVGL's pointer indev reads ONE place and does not
// care which mouse is on - and a future third transport is a producer, not a
// change to the UI.

#ifdef __cplusplus
extern "C" {
#endif

// Apply one report's movement. Deltas are in mouse units; the result is
// clamped to the landscape logical screen. Safe from any task.
void hid_cursor_apply(int dx, int dy, uint8_t buttons);

// Latest position (0..1279 / 0..719) and button bitmask (bit0 = left).
void hid_cursor_get(int *x, int *y, uint8_t *buttons);

// Whether ANY mouse is currently connected - what the UI uses to decide
// whether to draw a cursor at all. Each transport reports its own state.
typedef enum { HID_CURSOR_SRC_USB = 0, HID_CURSOR_SRC_BLE, HID_CURSOR_SRC_COUNT } hid_cursor_src_t;
void hid_cursor_set_present(hid_cursor_src_t src, bool present);
bool hid_cursor_present(void);

#ifdef __cplusplus
}
#endif
