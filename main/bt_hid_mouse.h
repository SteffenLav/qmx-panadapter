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
bool bt_hid_mouse_started(void);
