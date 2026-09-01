// hid_keycode.c - HID keyboard usage -> the text token ui.c already speaks.
// See hid_keycode.h for why this exists and why it is US-layout only.
//
// Portable: no ESP-IDF headers, so test/hid_keycode_harness.c can link the real
// function rather than a copy of it. A mirrored copy of a lookup table is worse
// than no test at all - it keeps passing while the device gets it wrong.

#include "hid_keycode.h"
#include <string.h>

// HID modifier byte, from the HID spec's keyboard boot report.
#define HIDM_LCTRL   0x01
#define HIDM_LSHIFT  0x02
#define HIDM_LALT    0x04
#define HIDM_RCTRL   0x10
#define HIDM_RSHIFT  0x20
#define HIDM_RALT    0x40

bool hid_keycode_is_modifier(uint8_t usage)
{
    return usage >= 0xE0 && usage <= 0xE7;
}

// Usages 0x1E-0x27 are the number row, in the order 1 2 3 4 5 6 7 8 9 0 - note
// that ZERO IS LAST, which is the single easiest thing to get wrong here.
static const char k_digits[10]         = { '1','2','3','4','5','6','7','8','9','0' };
static const char k_digits_shifted[10] = { '!','@','#','$','%','^','&','*','(',')' };

// The punctuation block, 0x2D-0x38 with two gaps handled below.
typedef struct { uint8_t usage; char plain; char shifted; } punct_t;
static const punct_t k_punct[] = {
    { 0x2D, '-',  '_'  },   // minus
    { 0x2E, '=',  '+'  },   // equal
    { 0x2F, '[',  '{'  },
    { 0x30, ']',  '}'  },
    { 0x31, '\\', '|'  },
    { 0x32, '\\', '|'  },   // non-US hash/tilde; same keycap position on many boards
    { 0x33, ';',  ':'  },
    { 0x34, '\'', '"'  },
    { 0x35, '`',  '~'  },
    { 0x36, ',',  '<'  },
    { 0x37, '.',  '>'  },
    { 0x38, '/',  '?'  },
    { 0x64, '\\', '|'  },   // non-US backslash, present on most European boards
};

// Named keys. The spelling is a CONTRACT with ui.c's strcasecmp chain - see the
// header. Anything not listed here produces nothing rather than a guess.
typedef struct { uint8_t usage; const char *name; } named_t;
static const named_t k_named[] = {
    { 0x28, "enter"     },
    { 0x58, "enter"     },   // keypad Enter - the same request
    { 0x29, "esc"       },
    { 0x2A, "backspace" },
    { 0x2B, "tab"       },
    { 0x4C, "del"       },   // Delete Forward
    { 0x4F, "right"     },
    { 0x50, "left"      },
    { 0x51, "down"      },
    { 0x52, "up"        },
    { 0x4B, "pgup"      },
    { 0x4E, "pgdn"      },
};

// The keypad digits and its operators, so a numeric field can be typed on the
// pad. Keypad NumLock state is not modelled: these are the NumLock-on usages,
// which is what a keyboard with no NumLock key (most compact ones) always
// sends. 0x62 is keypad 0 and 0x59-0x61 are 1-9, zero last again.
static bool keypad_char(uint8_t usage, char *out)
{
    if (usage >= 0x59 && usage <= 0x61) { *out = (char)('1' + (usage - 0x59)); return true; }
    switch (usage) {
    case 0x62: *out = '0'; return true;
    case 0x63: *out = '.'; return true;
    case 0x54: *out = '/'; return true;
    case 0x55: *out = '*'; return true;
    case 0x56: *out = '-'; return true;
    case 0x57: *out = '+'; return true;
    default:   return false;
    }
}

static void put1(hid_key_event_t *out, char c)
{
    out->text[0] = c;
    out->text[1] = '\0';
}

bool hid_keycode_translate(uint8_t usage, uint8_t hid_mods, hid_key_event_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    // 0x00 is "no key in this slot" and 0x01-0x03 are the rollover / POST-fail
    // codes a keyboard sends when it cannot report everything being held. All
    // four must be dropped: typing them would turn a fistful of keys into
    // garbage characters.
    if (usage <= 0x03) return false;
    if (hid_keycode_is_modifier(usage)) return false;

    const bool shift = (hid_mods & (HIDM_LSHIFT | HIDM_RSHIFT)) != 0;

    // Translate the modifier byte explicitly. Only Ctrl and Alt reach ui.c,
    // because those are the only two its shortcut table acts on; Shift is
    // consumed here by choosing the character, and GUI has no meaning on this
    // device. RightAlt is reported as Alt rather than as AltGr - without a
    // national layout there is no AltGr level to reach anyway.
    if (hid_mods & (HIDM_LCTRL | HIDM_RCTRL)) out->mods |= HID_KEY_MOD_CTRL;
    if (hid_mods & (HIDM_LALT  | HIDM_RALT))  out->mods |= HID_KEY_MOD_ALT;

    // Letters. Ctrl/Alt shortcuts are matched on a LOWERCASE letter in ui.c
    // (it tolower()s text[0] itself), so case here only decides what gets
    // typed, and a modified key never reaches the typing path.
    if (usage >= 0x04 && usage <= 0x1D) {
        put1(out, (char)((shift ? 'A' : 'a') + (usage - 0x04)));
        return true;
    }

    if (usage >= 0x1E && usage <= 0x27) {
        put1(out, shift ? k_digits_shifted[usage - 0x1E] : k_digits[usage - 0x1E]);
        return true;
    }

    if (usage == 0x2C) { put1(out, ' '); return true; }   // space

    for (size_t i = 0; i < sizeof(k_punct) / sizeof(k_punct[0]); i++) {
        if (k_punct[i].usage == usage) {
            put1(out, shift ? k_punct[i].shifted : k_punct[i].plain);
            return true;
        }
    }

    {
        char c;
        if (keypad_char(usage, &c)) { put1(out, c); return true; }
    }

    for (size_t i = 0; i < sizeof(k_named) / sizeof(k_named[0]); i++) {
        if (k_named[i].usage == usage) {
            // Refuse rather than truncate. A silently shortened token would
            // not match ui.c's strcasecmp chain and the key would do nothing,
            // which is indistinguishable from the bug this whole file fixes.
            size_t n = strlen(k_named[i].name);
            if (n + 1 > sizeof(out->text)) return false;
            memcpy(out->text, k_named[i].name, n + 1);
            return true;
        }
    }

    return false;   // Function keys, Home/End, media keys: nothing to type
}
