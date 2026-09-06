#include "format_freq.h"

#include <stdio.h>

freq_style_t g_freq_style = FREQ_STYLE_DOTS;

size_t format_freq_hz(uint32_t hz, freq_style_t style, char *out, size_t out_sz)
{
    if (!out || out_sz < 2) { if (out && out_sz) out[0] = '\0'; return 0; }

    unsigned long mhz =  hz / 1000000UL;
    unsigned long khz = (hz / 1000UL) % 1000UL;
    unsigned long rem =  hz % 1000UL;

    /* The two styles differ ONLY in the first separator. The second is a
     * decimal point in both readings: 14,074.000 is MHz then kHz.Hz, and
     * 14.074.000 is the same numbers with a different thousands mark. Getting
     * that wrong would print 14,074,000, which is Hz - a different number. */
    const char *sep1 = (style == FREQ_STYLE_COMMA) ? "," : ".";

    int n = snprintf(out, out_sz, "%lu%s%03lu.%03lu", mhz, sep1, khz, rem);
    if (n < 0) { out[0] = '\0'; return 0; }
    if ((size_t)n >= out_sz) return out_sz - 1;   /* truncated, still terminated */
    return (size_t)n;
}
