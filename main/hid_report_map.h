#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Parse a USB HID Report Map (the descriptor a BLE mouse publishes at
// characteristic 0x2A4B) far enough to decode its movement reports.
//
// WHY THIS EXISTS
//   BLE mice do not agree on a byte layout. bt_hid_mouse.c used to assume ONE -
//   the 12-bit packed layout captured off a Logitech M240 - for any report of five
//   bytes or more. On a mouse that sends 16-bit movement instead (the other common
//   choice) that assumption decodes small movements into numbers in the thousands,
//   and since the cursor is clamped to the screen the result is a pointer that
//   races sideways and sticks to the top edge. Samuel W7STF reported exactly that.
//
//   The descriptor says what the layout actually is, and the device already reads
//   it - it was throwing the bytes away and logging only their length. So this is
//   not a heuristic replacing a guess; it is using the answer the mouse already
//   gave us.
//
// WHAT IT DOES NOT DO
//   It is not a general HID parser. It walks the item stream tracking the global
//   state that matters (report ID, report size, report count, usage page) and
//   returns the location of the FIRST input report carrying both Usage(X) and
//   Usage(Y). Collections, feature/output items, padding and vendor items are
//   skipped rather than modelled. That is enough for a mouse and nothing more is
//   claimed.

typedef struct {
    bool    valid;          // false => nothing usable was found; keep the fallbacks
    uint8_t report_id;      // 0 = reports carry no ID byte
    uint16_t x_bit, y_bit;  // bit offset of X and Y within the report payload
    uint8_t  x_bits, y_bits;// their width in bits (8, 12 and 16 are all real)
    bool     have_wheel;
    uint16_t wheel_bit;
    uint8_t  wheel_bits;
    uint16_t total_bits;    // declared payload size, for a sanity check on arrival
} hid_mouse_layout_t;

// Returns true and fills out on success. Safe on truncated or malformed input:
// it never reads past len and gives up rather than guessing.
bool hid_report_map_parse(const uint8_t *desc, size_t len, hid_mouse_layout_t *out);

// Extract a signed field of bits_wide bits starting at bit_off (LSB-first within
// each byte, which is how HID packs them), sign-extended to int. Returns 0 if the
// field would run past the end of the report.
int hid_field_signed(const uint8_t *report, size_t len, uint16_t bit_off, uint8_t bits_wide);
