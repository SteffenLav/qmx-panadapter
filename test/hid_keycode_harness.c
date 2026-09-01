// hid_keycode_harness.c - host-side verification of hid_keycode_translate()
// (main/hid_keycode.c), which turns a Bluetooth keyboard's HID usages into the
// text tokens ui.c's kbd_text_cb() already acts on.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main -o test/hid_keycode.exe test/hid_keycode_harness.c main/hid_keycode.c
//   ./test/hid_keycode.exe
//
// WHY THIS EXISTS
//   Don N2VGU's Rii i4 keyboard/touchpad points and scrolls but types nothing
//   (#273). The fix is a lookup table, and a lookup table is exactly the kind of
//   code that is wrong in one entry and right everywhere else - the number row
//   alone has zero at the END (usage 0x27), which is the single easiest thing to
//   get backwards, and getting it backwards means every "0" in a frequency, a
//   callsign or a grid square is silently a "9".
//
//   It also pins the NAMED TOKENS against ui.c. Those strings are a contract
//   with that file's strcasecmp chain: spell "backspace" as "bksp" and the key
//   compiles, runs, and does nothing - indistinguishable from the bug being
//   fixed. Case 6 reads the tokens out of ui.c itself rather than restating
//   them, so a rename there fails HERE instead of in someone's hands.
//
// MUTATION RESULTS (2026-08-28), because a test that cannot fail proves nothing
//   Caught: number row read as '0'+n (4 failures) - the exact off-by-one that
//   turns every "0" into a "9"; the HID modifier byte passed straight through
//   instead of translated (9); "backspace" renamed to "bksp" (1); Shift leaking
//   into mods (7); keypad 1-9 off by one (2).
//
//   SURVIVED, and it is an EQUIVALENT MUTANT rather than a gap: relaxing the
//   `usage <= 0x03` rollover guard to `usage == 0x00` changes nothing, because
//   no table in hid_keycode.c maps 0x01-0x03 and they fall through to the final
//   `return false` anyway. The guard is defence-in-depth for a future table that
//   might, and the rollover cases below still pass with it removed - which is
//   the proof, not an excuse. Do not "strengthen" the test to kill this one; it
//   would only be testing an implementation detail.
//
// WHY IT COMPILES THE REAL FILE
//   Same reason as test/hid_map_harness.c and test/spot_sig_harness.c: a
//   mirrored copy of a table would drift, and a drifted copy keeps passing while
//   the device gets it wrong.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "hid_keycode.h"

static int fails;

#define HIDM_LSHIFT 0x02
#define HIDM_LCTRL  0x01
#define HIDM_LALT   0x04
#define HIDM_RSHIFT 0x20
#define HIDM_RALT   0x40

static void expect(uint8_t usage, uint8_t mods, const char *want_text, uint8_t want_mods,
                   const char *what)
{
    hid_key_event_t ev;
    bool ok = hid_keycode_translate(usage, mods, &ev);
    if (!want_text) {
        if (ok) {
            printf("FAIL %-34s usage 0x%02X mods 0x%02X -> \"%s\" (expected nothing)\n",
                   what, usage, mods, ev.text);
            fails++;
        }
        return;
    }
    if (!ok) {
        printf("FAIL %-34s usage 0x%02X mods 0x%02X -> nothing (expected \"%s\")\n",
               what, usage, mods, want_text);
        fails++;
        return;
    }
    if (strcmp(ev.text, want_text) != 0 || ev.mods != want_mods) {
        printf("FAIL %-34s usage 0x%02X mods 0x%02X -> \"%s\"/0x%02X "
               "(expected \"%s\"/0x%02X)\n",
               what, usage, mods, ev.text, ev.mods, want_text, want_mods);
        fails++;
    }
}

// ---------------------------------------------------------------------------
// Case 6 reads ui.c and checks every token this file can emit is one that file
// actually tests for. Anything unreadable is reported, never assumed passing.
static const char *k_all_tokens[] = {
    "enter", "esc", "backspace", "tab", "del",
    "right", "left", "down", "up", "pgup", "pgdn",
};

static void check_tokens_against_ui(void)
{
    FILE *f = fopen("main/ui/ui.c", "rb");
    if (!f) {
        printf("SKIP case 6: main/ui/ui.c not readable from here "
               "(run the harness from the repo root)\n");
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); printf("SKIP case 6: seek failed\n"); return; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); printf("SKIP case 6: ui.c empty\n"); return; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); printf("SKIP case 6: out of memory\n"); return; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';

    for (size_t i = 0; i < sizeof(k_all_tokens) / sizeof(k_all_tokens[0]); i++) {
        // ui.c matches these as strcasecmp(text, "enter") and friends, so the
        // quoted token is what to look for.
        char needle[32];
        snprintf(needle, sizeof needle, "\"%s\"", k_all_tokens[i]);
        if (!strstr(buf, needle)) {
            printf("FAIL case 6: ui.c has no %s - a %s key would do nothing\n",
                   needle, k_all_tokens[i]);
            fails++;
        }
    }
    free(buf);
}

int main(void)
{
    // --- 1. Letters, and the shift that only changes the character ----------
    expect(0x04, 0,           "a", 0, "usage 0x04 = a");
    expect(0x1D, 0,           "z", 0, "usage 0x1D = z");
    expect(0x04, HIDM_LSHIFT, "A", 0, "shift+0x04 = A");
    expect(0x1D, HIDM_RSHIFT, "Z", 0, "right-shift also shifts");

    // --- 2. THE NUMBER ROW, AND ZERO IS LAST --------------------------------
    // If 0x1E..0x27 is ever read as '0'+n, every one of these is off by one and
    // "0" becomes "9". That is a wrong frequency, a wrong grid and a wrong
    // callsign, silently.
    expect(0x1E, 0, "1", 0, "usage 0x1E = 1");
    expect(0x1F, 0, "2", 0, "usage 0x1F = 2");
    expect(0x26, 0, "9", 0, "usage 0x26 = 9");
    expect(0x27, 0, "0", 0, "usage 0x27 = 0  <- zero is LAST");
    expect(0x1E, HIDM_LSHIFT, "!", 0, "shift+1 = !");
    expect(0x27, HIDM_LSHIFT, ")", 0, "shift+0 = )");

    // --- 3. Named keys: the contract with ui.c ------------------------------
    expect(0x28, 0, "enter",     0, "Return");
    expect(0x58, 0, "enter",     0, "keypad Enter is also enter");
    expect(0x29, 0, "esc",       0, "Escape");
    expect(0x2A, 0, "backspace", 0, "Backspace (longest token)");
    expect(0x2B, 0, "tab",       0, "Tab");
    expect(0x4C, 0, "del",       0, "Delete Forward");
    expect(0x4F, 0, "right",     0, "arrow right");
    expect(0x50, 0, "left",      0, "arrow left");
    expect(0x51, 0, "down",      0, "arrow down");
    expect(0x52, 0, "up",        0, "arrow up");
    expect(0x4B, 0, "pgup",      0, "Page Up");
    expect(0x4E, 0, "pgdn",      0, "Page Down");

    // --- 4. Modifiers reach ui.c in ITS convention, not HID's ---------------
    // ui.c has Ctrl=0x01 and Alt=0x04. HID agrees for the LEFT ones by
    // coincidence; the right-hand ones (0x10, 0x40) do NOT, and passing the HID
    // byte straight through would make right-Ctrl+R do nothing at all.
    expect(0x15, HIDM_LCTRL, "r", HID_KEY_MOD_CTRL, "Ctrl+R (left)");
    expect(0x15, 0x10,       "r", HID_KEY_MOD_CTRL, "Ctrl+R (RIGHT ctrl)");
    expect(0x10, HIDM_LALT,  "m", HID_KEY_MOD_ALT,  "Alt+M (left)");
    expect(0x10, HIDM_RALT,  "m", HID_KEY_MOD_ALT,  "Alt+M (RIGHT alt)");
    // Shift must NOT appear in mods - ui.c has no Shift shortcut layer, and a
    // stray bit there would stop a shifted letter being typed at all (it would
    // be taken for a modified key and swallowed).
    expect(0x15, HIDM_LSHIFT, "R", 0, "Shift+R types R and sets no mod");
    // Ctrl+Shift+R is still Ctrl+R to the shortcut table.
    expect(0x15, HIDM_LCTRL | HIDM_LSHIFT, "R", HID_KEY_MOD_CTRL, "Ctrl+Shift+R");

    // --- 5. What must produce NOTHING ---------------------------------------
    // Rollover is the dangerous one: hold six keys on a cheap membrane keyboard
    // and every slot becomes 0x01. Typed, that is six characters of garbage
    // into whatever field has focus.
    expect(0x00, 0, NULL, 0, "0x00 = no key in this slot");
    expect(0x01, 0, NULL, 0, "0x01 = ErrorRollOver  <- must not type");
    expect(0x02, 0, NULL, 0, "0x02 = POSTFail");
    expect(0x03, 0, NULL, 0, "0x03 = ErrorUndefined");
    expect(0xE0, 0, NULL, 0, "0xE0 = LeftCtrl usage itself");
    expect(0xE7, 0, NULL, 0, "0xE7 = RightGUI usage itself");
    expect(0x3A, 0, NULL, 0, "F1 has nothing to type");
    expect(0x39, 0, NULL, 0, "CapsLock has nothing to type");

    // --- 6. Punctuation, space, keypad --------------------------------------
    expect(0x2C, 0, " ", 0, "space");
    expect(0x2D, 0, "-", 0, "minus");
    expect(0x2D, HIDM_LSHIFT, "_", 0, "shift+minus = underscore");
    expect(0x37, 0, ".", 0, "period  <- needed for a frequency");
    expect(0x36, 0, ",", 0, "comma");
    expect(0x38, 0, "/", 0, "slash   <- needed for a portable callsign");
    expect(0x62, 0, "0", 0, "keypad 0");
    expect(0x59, 0, "1", 0, "keypad 1");
    expect(0x61, 0, "9", 0, "keypad 9");
    expect(0x63, 0, ".", 0, "keypad period");

    // --- 7. Every token fits the output buffer ------------------------------
    for (size_t i = 0; i < sizeof(k_all_tokens) / sizeof(k_all_tokens[0]); i++) {
        hid_key_event_t probe;
        if (strlen(k_all_tokens[i]) + 1 > sizeof(probe.text)) {
            printf("FAIL token \"%s\" does not fit hid_key_event_t.text[%u]\n",
                   k_all_tokens[i], (unsigned)sizeof(probe.text));
            fails++;
        }
    }

    check_tokens_against_ui();

    if (fails == 0) printf("hid_keycode: ALL CASES PASS\n");
    else            printf("hid_keycode: %d FAILURE(S)\n", fails);
    return fails ? 1 : 0;
}
