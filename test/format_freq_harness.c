/* Host test for main/util/format_freq.c - #302.
 *
 *   gcc -std=c11 -Wall -Wextra -I main/util -o format_freq_harness \
 *       test/format_freq_harness.c main/util/format_freq.c && ./format_freq_harness
 *
 * ⚠ NOT YET RUN: the bench machine has no host C compiler (see the note on
 * test/adif_check_harness.c). Run it when one exists.
 *
 * The case that matters most is the second separator. Both styles put a
 * DECIMAL POINT between kHz and Hz - 14,074.000 and 14.074.000 are the same
 * number written two ways. Printing 14,074,000 would be Hz, i.e. a different
 * number on screen, which is the whole risk of this change.
 */
#include <stdio.h>
#include <string.h>
#include "format_freq.h"

static int fails;
static void expect(uint32_t hz, freq_style_t st, const char *want)
{
    char got[16];
    format_freq_hz(hz, st, got, sizeof(got));
    if (strcmp(got, want) == 0) { printf("  ok   %u -> %s\n", hz, got); return; }
    printf("  FAIL %u: got '%s' want '%s'\n", hz, got, want);
    fails++;
}

int main(void)
{
    printf("format_freq harness\n");

    /* The format the Tab5 has always printed - this refactor must be invisible. */
    expect(14074000, FREQ_STYLE_DOTS,  "14.074.000");
    expect(14074000, FREQ_STYLE_COMMA, "14,074.000");

    expect(7074000,  FREQ_STYLE_DOTS,  "7.074.000");
    expect(1838100,  FREQ_STYLE_DOTS,  "1.838.100");
    expect(50313000, FREQ_STYLE_DOTS,  "50.313.000");
    expect(50313000, FREQ_STYLE_COMMA, "50,313.000");

    /* Zero padding: 7.005.000 must not collapse to 7.5.0 */
    expect(7005000,  FREQ_STYLE_DOTS,  "7.005.000");
    expect(7000005,  FREQ_STYLE_DOTS,  "7.000.005");
    expect(0,        FREQ_STYLE_DOTS,  "0.000.000");

    /* Truncation is terminated, never overrun. */
    {
        char small[6];
        size_t n = format_freq_hz(14074000, FREQ_STYLE_DOTS, small, sizeof(small));
        if (n >= sizeof(small) || small[sizeof(small) - 1] != '\0') {
            printf("  FAIL truncation not terminated\n"); fails++;
        } else printf("  ok   truncated safely -> '%s'\n", small);
    }
    if (format_freq_hz(1, FREQ_STYLE_DOTS, NULL, 16) != 0) {
        printf("  FAIL NULL out should return 0\n"); fails++;
    } else printf("  ok   NULL out returns 0\n");

    printf(fails ? "\nFAILED (%d)\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
