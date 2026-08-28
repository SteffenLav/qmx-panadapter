#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Translate HID keyboard usages (Usage Page 0x07) into the exact thing the
// Tab5's UI keyboard bridge already takes: a short text token plus a modifier
// byte in the UI's own convention.
//
// WHY THIS EXISTS
//   Don N2VGU paired a Rii i4 mini keyboard/touchpad. Point, tap and scroll all
//   work - the mouse half goes through hid_report_map.c and hid_cursor.c - and
//   the keys do nothing at all, because nothing on the device turns a HID usage
//   into a character. "There are a LOT of BT keyboard/touchpads available, many
//   are conveniently small", and for anyone without the snap-on keyboard it is
//   the only way to type on a Tab5 (#273).
//
// WHY THE OUTPUT LOOKS LIKE THIS
//   ui.c's kbd_text_cb(text, mods, arg) is the entire typing and shortcut path
//   already, written for the snap-on keyboard: a one-character `text` is a
//   literal character, a longer one is a named key ("enter", "esc", "tab",
//   "backspace", "del", "left", "right", "up", "down", "pgup", "pgdn"), and
//   `mods` carries Ctrl/Alt for the shortcut table. Producing that shape means a
//   Bluetooth keyboard inherits every field, every modal's Enter/Esc, the focus
//   walk and all 20-odd shortcuts without one line of UI work.
//
//   The named tokens are matched with strcasecmp in ui.c, so their spelling is a
//   contract with that file. test/hid_keycode_harness.c pins them.
//
// ⚠ US LAYOUT, DELIBERATELY AND ONLY
//   A HID keyboard sends POSITIONS, not characters - usage 0x1B is "the key
//   where Z sits on a US board", which is Y on a German one and W on a French
//   one. The layout lives in the host, so a national layout would have to be a
//   setting with a table per language, and picking one silently would be worse
//   than none. US is what an unconfigured HOGP host does; say so in the manual
//   rather than pretending otherwise. Letters, digits and Enter/Esc/arrows are
//   identical across the Latin layouts, so the shortcut table and every numeric
//   field work regardless - it is the punctuation that differs.

// KEEP IN STEP WITH ui.c. Ctrl is bit 0 and Alt is bit 2 there, read off the
// snap-on keyboard's own hardware rather than assumed. HID's standard modifier
// byte happens to agree (bit0 LeftCtrl, bit2 LeftAlt) - a coincidence, not a
// guarantee, so the mapping below is explicit rather than a pass-through.
#define HID_KEY_MOD_CTRL  0x01
#define HID_KEY_MOD_ALT   0x04

typedef struct {
    // One character, or a named key token; always NUL-terminated. Sized for
    // the longest token this can emit, "backspace" (9 + NUL); the translator
    // refuses rather than truncates if a longer one is ever added, and the
    // harness checks every token fits.
    char    text[12];
    uint8_t mods;      // HID_KEY_MOD_* - Ctrl/Alt only, which is all ui.c acts on
} hid_key_event_t;

// usage:    a byte from the keycode array of a HID keyboard input report
// hid_mods: that report's modifier byte (bit0 LCtrl, 1 LShift, 2 LAlt, 3 LGui,
//           4 RCtrl, 5 RShift, 6 RAlt, 7 RGui)
//
// Returns false for a usage this does not produce anything for - 0x00 (no key),
// 0x01-0x03 (the rollover/POST error codes a keyboard sends when it cannot
// report every key held), the modifier usages themselves, and everything with
// no sensible text. A false return must be DROPPED, never typed: a keyboard
// reporting rollover would otherwise spray characters.
bool hid_keycode_translate(uint8_t usage, uint8_t hid_mods, hid_key_event_t *out);

// Is this usage one of the eight modifier keys (0xE0-0xE7)? They arrive in the
// modifier byte, not the keycode array, but some keyboards put them in both.
bool hid_keycode_is_modifier(uint8_t usage);
