#include "format_freq.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

freq_style_t g_freq_style = FREQ_STYLE_DOTS;

/* Copy `src` into `out` (size `out_sz`), always NUL-terminating, returning how
   many characters landed. Truncation is a short string, never an overrun. */
static size_t copy_bounded(char *out, size_t out_sz, const char *src)
{
    size_t i = 0;
    while (src[i] && i + 1 < out_sz) { out[i] = src[i]; i++; }
    out[i] = 0;
    return i;
}


size_t format_freq_hz(uint32_t hz, freq_style_t style, char *out, size_t out_sz)
{
    if (!out || out_sz < 2) { if (out && out_sz) out[0] = '\0'; return 0; }

    unsigned long mhz =  hz / 1000000UL;
    unsigned long khz = (hz / 1000UL) % 1000UL;
    unsigned long rem =  hz % 1000UL;

    /* ⛔ BOTH separators change together, and the first version of this got it
     * wrong. It printed 14,074.000 - but a decimal point before the last three
     * digits says the number is kHz, while the label beside it says Hz. The
     * value IS Hz, so under Don N2VGU's rule ("commas separating kilo, mega,
     * etc., and a decimal point after units") an integer number of Hz is
     * 14,074,000 - thousands marks all the way down and no decimal at all,
     * exactly as 14.074.000 is in the dotted style.
     *
     * That mislabelling is also why the FT8 preset button looked out of step:
     * it reads "14.074 MHz", where the dot is a genuine DECIMAL POINT and is
     * correct in both conventions. The preset was right; this was wrong. */
    const char *sep = (style == FREQ_STYLE_COMMA) ? "," : ".";

    /* Formatted into a local of KNOWN size, then copied under the caller's
     * bound. Writing straight into `out` makes -Werror=format-truncation
     * complain it cannot prove the room exists, and silencing that warning
     * would throw away a real check everywhere else. */
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lu%s%03lu%s%03lu", mhz, sep, khz, sep, rem);
    if (n < 0) { out[0] = '\0'; return 0; }
    return copy_bounded(out, out_sz, tmp);
}

size_t format_freq_mhz_khz(uint32_t hz, freq_style_t style, char *out, size_t out_sz)
{
    if (!out || out_sz < 2) { if (out && out_sz) out[0] = '\0'; return 0; }
    unsigned long mhz =  hz / 1000000UL;
    unsigned long khz = (hz / 1000UL) % 1000UL;
    const char *sep = (style == FREQ_STYLE_COMMA) ? "," : ".";
    /* Formatted into a local of KNOWN size, then copied under the caller's
     * bound. Writing straight into `out` makes -Werror=format-truncation
     * complain it cannot prove the room exists, and silencing that warning
     * would throw away a real check everywhere else. */
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lu%s%03lu", mhz, sep, khz);
    if (n < 0) { out[0] = '\0'; return 0; }
    return copy_bounded(out, out_sz, tmp);
}

int format_freq_selftest(void)
{
    static const char *TAG = "freqfmt";
    struct { uint32_t hz; freq_style_t st; const char *want; } C[] = {
        { 14074000, FREQ_STYLE_DOTS,  "14.074.000" },
        { 14074000, FREQ_STYLE_COMMA, "14,074,000" },
        {  7074000, FREQ_STYLE_DOTS,   "7.074.000" },
        {  1838100, FREQ_STYLE_DOTS,   "1.838.100" },
        { 50313000, FREQ_STYLE_COMMA, "50,313,000" },
        {  7005000, FREQ_STYLE_DOTS,   "7.005.000" },   /* zero padding */
        {  7000005, FREQ_STYLE_DOTS,   "7.000.005" },
        {        0, FREQ_STYLE_DOTS,   "0.000.000" },
    };
    struct { uint32_t hz; freq_style_t st; const char *want; } S[] = {
        { 14074000, FREQ_STYLE_DOTS,  "14.074" },
        { 14074000, FREQ_STYLE_COMMA, "14,074" },
        {  1838100, FREQ_STYLE_DOTS,   "1.838" },
    };
    int bad = 0;
    char got[24];
    for (size_t i = 0; i < sizeof(C) / sizeof(C[0]); i++) {
        format_freq_hz(C[i].hz, C[i].st, got, sizeof(got));
        if (strcmp(got, C[i].want)) {
            ESP_LOGE(TAG, "FAIL %lu -> '%s' want '%s'", (unsigned long)C[i].hz, got, C[i].want);
            bad++;
        }
    }
    for (size_t i = 0; i < sizeof(S) / sizeof(S[0]); i++) {
        format_freq_mhz_khz(S[i].hz, S[i].st, got, sizeof(got));
        if (strcmp(got, S[i].want)) {
            ESP_LOGE(TAG, "FAIL short %lu -> '%s' want '%s'", (unsigned long)S[i].hz, got, S[i].want);
            bad++;
        }
    }
    /* Truncation stays terminated rather than overrunning the caller. */
    { char small[6]; format_freq_hz(14074000, FREQ_STYLE_DOTS, small, sizeof(small));
      if (small[sizeof(small) - 1] != 0) { ESP_LOGE(TAG, "FAIL truncation unterminated"); bad++; } }
    if (format_freq_hz(1, FREQ_STYLE_DOTS, NULL, 16) != 0) {
        ESP_LOGE(TAG, "FAIL NULL out should return 0"); bad++;
    }
    ESP_LOGW(TAG, "format_freq self-test: %s (%d failure(s))", bad ? "FAILED" : "all passed", bad);
    return bad;
}
