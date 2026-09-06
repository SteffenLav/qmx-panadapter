// One place that turns a frequency in Hz into text (#302).
//
// Don N2VGU: "I am used to reading/writing frequencies with commas separating
// kilo, mega, and a decimal point after units... It is settable on many other
// instruments and my beloved HP RPN calculators." We print 14.074.000 where he
// would read 14,074.000.
//
// ⛔ THE HELPER IS THE POINT, not the setting. That format string appeared NINE
// times in ui.c alone and ~23 more in index.html, and changing them by
// find-and-replace is exactly how two of them come to disagree - the same
// lesson as pan_view and the FT8 slot gates. So every caller goes through here
// whether or not a separator style ever becomes a setting.
//
// Portable: no ESP-IDF, no LVGL, no allocation. test/format_freq_harness.c
// links the real function.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    FREQ_STYLE_DOTS = 0,   // 14.074.000  - what the Tab5 has always printed
    FREQ_STYLE_COMMA,      // 14,074.000  - MHz, comma thousands, dot decimal
} freq_style_t;

// Writes "<MHz><sep><kHz 3><sep><Hz 3>" into `out`. Always NUL-terminates, and
// never writes past `out_sz`. Needs 15 bytes for any frequency this radio can
// reach; returns the number of characters written (excluding the NUL), or 0 if
// `out` is NULL or too small to hold anything useful.
size_t format_freq_hz(uint32_t hz, freq_style_t style, char *out, size_t out_sz);

// The style every caller uses unless it has a reason not to. A single writable
// global rather than a settings dependency, so this file stays portable and
// testable; main sets it once at boot if a stored preference ever exists.
extern freq_style_t g_freq_style;
