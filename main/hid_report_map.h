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

// ---------------------------------------------------------------------------
// The KEYBOARD half of a combo device (#273, Don N2VGU's Rii i4).
//
// A keyboard/touchpad publishes ONE Report Map describing both, with a separate
// report ID for each. The mouse parse above finds the pointer report; this finds
// the keyboard one, so a notification can be routed to the right decoder instead
// of the keyboard's keystrokes being decoded as mouse movement or silently
// dropped for having the wrong report ID - which is precisely why his touchpad
// works and his keys do nothing.
//
// The two are told apart by the Input item's own Data/Variable/Array bit, not by
// a size heuristic: the modifier byte is 8 x 1-bit VARIABLE fields, the keycode
// slots are an ARRAY of 8-bit usages. That is what the HID spec says a keyboard
// is, so it is a definition rather than a guess.
typedef struct {
    bool     valid;
    uint8_t  report_id;   // 0 = this report carries no ID byte
    uint16_t mod_bit;     // bit offset of the modifier bitmap within the payload
    uint8_t  mod_bits;    // its width; 8 on every keyboard, but read not assumed
    uint16_t key_bit;     // bit offset of the first keycode slot
    uint8_t  key_bits;    // bits per slot (8)
    uint8_t  key_count;   // number of slots (6 on a boot-style report)
    uint16_t total_bits;  // declared payload size of THIS report
} hid_kbd_layout_t;

bool hid_report_map_parse_keyboard(const uint8_t *desc, size_t len, hid_kbd_layout_t *out);

// Extract a signed field of bits_wide bits starting at bit_off (LSB-first within
// each byte, which is how HID packs them), sign-extended to int. Returns 0 if the
// field would run past the end of the report.
int hid_field_signed(const uint8_t *report, size_t len, uint16_t bit_off, uint8_t bits_wide);

// ---------------------------------------------------------------------------
// Fallback decode, for when the Report Map could not be read or parsed.
//
// This is still a guess and is still the WRONG answer in principle - the right
// one is hid_report_map_parse() above. What it is not is a guess at ONE layout
// applied to every mouse, which is what it replaced: bt_hid_mouse.c treated any
// report of five bytes or more as the Logitech M240's 12-bit packed layout.
//
// Kevin KW6E's Microsoft Surface Arc sends NINE bytes with 16-bit movement, and
// his own diagnostic log proved what that costs. Run his real report
// `00 06 00 0b 00 ff ff 00 00` (X=+6, Y=+11) through the M240 arithmetic and it
// decodes as X=-1280, Y=0 - a huge jump in the wrong direction and no vertical
// movement at all. That is exactly what he described: "connects and scrolls
// perfectly, but moving the mouse pointer is erratic".
//
// So the length picks between layouts that have each been CAPTURED off real
// hardware. Nothing here is inferred from a datasheet.
// ---------------------------------------------------------------------------
typedef struct {
    int     dx, dy;     // movement this report, in mouse units
    int     wheel;      // 0 when the report carries none
    uint8_t buttons;    // raw first byte; callers read the low bits
} hid_mouse_move_t;

// Returns false for a report too short to hold movement at all.
bool hid_fallback_decode(const uint8_t *report, size_t len, hid_mouse_move_t *out);
