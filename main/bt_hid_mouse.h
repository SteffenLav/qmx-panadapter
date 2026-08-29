#pragma once
#include <stdbool.h>

// BLE HID mouse over the C6 controller. See bt_hid_mouse.c for why Bluetooth
// is the only route to a pointer that works WITH the QMX connected.
//
// Stage 1 scans only - it proves BLE coexists with WiFi on the shared SDIO
// link before any pairing code exists. Gated on the bt_mouse_en setting, off
// by default: this starts a second radio subsystem on this board's most
// fragile link, so it must be something the operator opts into.
void bt_hid_mouse_init(void);

// Is the NimBLE stack actually up? This is the RADIO, not the setting - the
// bottom-bar glyph is driven from it so that unticking the box cannot grey out
// an icon for a radio that is still transmitting (#270, Don N2VGU).
bool bt_hid_mouse_started(void);

/* True when a connected BLE device declared a KEYBOARD in its report map.
 * The on-screen keyboard uses this to stay out of the way (#273). */
bool bt_hid_keyboard_active(void);

// What bt_mouse_en said when this boot began, i.e. whether the radio was ever
// going to come up this session. Differs from bt_hid_mouse_started() only
// during the seconds NimBLE spends waiting for the C6 link, which is exactly
// when a "restart pending?" test would otherwise get the wrong answer.
bool bt_hid_mouse_enabled_at_boot(void);
