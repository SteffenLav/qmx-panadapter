// One place that turns a frequency in Hz into text (#302).
//
// Don N2VGU: "I am used to reading/writing frequencies with commas separating
// kilo, mega, and a decimal point after units... It is settable on many other
// instruments and my beloved HP RPN calculators." We print 14.074.000 where he
// would read 14,074,000.
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

/* ⭐ THE DEFAULT IS NOT A NATIONAL CONVENTION, AND THAT IS WHY IT IS THE DEFAULT.
 *
 * Icom, Yaesu, Kenwood and the QMX all print a frequency with SEVERAL PERIODS
 * used purely as visual grouping - 14.260.00, 7.040.00 - regardless of local
 * grammar. The dots are structural anchors between MHz, kHz and Hz, not a
 * decimal point. That is a radio-display convention, it is what the operator
 * sees on the QMX's own LCD beside the Tab5, and matching it is worth more than
 * matching anyone's written grammar.
 *
 * The alternative is US written grammar, where a comma is the thousands
 * separator and a period the decimal. An INTEGER number of Hz therefore takes
 * commas the whole way down and no decimal at all: 14,074,000.
 *
 * ⛔ Do NOT "fix" the comma style to 14,074.000. A decimal point before the
 * last three digits says the value is kHz, while the label beside it says Hz -
 * that is what the first version of this printed, and it is wrong.
 *
 * Note what is NOT here: European grammar (comma as the DECIMAL, 14,074 MHz).
 * Nobody asked for it, and on a display that shows Hz it would collide with the
 * US reading of the same glyph. */
typedef enum {
    FREQ_STYLE_DOTS = 0,   // 14.074.000 - the radio-display convention (default)
    FREQ_STYLE_COMMA,      // 14,074,000 - US grammar: comma thousands, no decimal
} freq_style_t;

// Writes "<MHz><sep><kHz 3><sep><Hz 3>" into `out`. Always NUL-terminates, and
// never writes past `out_sz`. Needs 15 bytes for any frequency this radio can
// reach; returns the number of characters written (excluding the NUL), or 0 if
// `out` is NULL or too small to hold anything useful.
size_t format_freq_hz(uint32_t hz, freq_style_t style, char *out, size_t out_sz);

// The style every caller uses unless it has a reason not to. A single writable
// global rather than a settings dependency, so this file stays portable and
// testable; main sets it once at boot if a stored preference ever exists.
/* The SHORTER form, MHz and kHz with no Hz digits and no unit - the frequency
 * axis, the band list, the WSPR band rows. Same separator question as above:
 * with no unit printed, that mark IS a thousands separator and must follow the
 * style, or the axis says 14.074 while the top bar says 14,074,000.
 *
 * ⛔ NOT for anything that prints " MHz" after it. There the dot is a DECIMAL
 * POINT - 14.074 MHz is fourteen point oh-seven-four megahertz - and turning it
 * into a comma would read as fourteen thousand MHz, a different number. Those
 * call sites are deliberately left alone. */
size_t format_freq_mhz_khz(uint32_t hz, freq_style_t style, char *out, size_t out_sz);

extern freq_style_t g_freq_style;

/* The harness cases, run ON THE DEVICE, returning the number that failed.
   This bench has no host C compiler, and a formatter that prints the wrong
   number is worse than one that does not exist - the first version of the
   comma style printed 14,074.000, a decimal point that says kHz beside a label
   that says Hz, and nobody could have executed a test to catch it.
   Reached via /api/cmd {"action":"freq_fmt_test"}. */
int format_freq_selftest(void);
