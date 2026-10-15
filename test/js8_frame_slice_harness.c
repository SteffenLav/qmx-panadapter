/* js8_frame_slice_harness - the per-frame character counts must slice the
 * message back up EXACTLY.
 *
 * WHY THIS EXISTS. js8_ftx.c prints the words going out with each frame
 * ("3/7 D RAINING HERE I") instead of a bare frame number, which is what Ryan
 * KK6DZB asked for after a real QSO. It does that by slicing the normalised
 * text with the counts js8_jsc_text_to_frames_used() hands back.
 *
 * ⛔ THE COUNTS ARE NOT A DIVISION, AND THAT IS THE WHOLE POINT. Both codings
 * are tried for every frame and the one carrying more characters wins, so
 * frames hold different amounts - measured here at 14, 15, 16, 21 and 24
 * characters inside a single message. Anything that re-derived the split by
 * dividing the length by the frame count would look right on a short message
 * and quietly print the wrong words on a long one.
 *
 * ⛔ AND IT FAILS SILENTLY IF IT EVER DRIFTS. A wrong slice is still a
 * plausible-looking string; nothing crashes, nothing logs, and the operator
 * simply reads words that are not the ones on the air. That is the kind of
 * defect this file is here to catch, because no amount of looking at the
 * screen would.
 *
 * Build and run:
 *   gcc -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -o slice test/js8_frame_slice_harness.c \
 *       components/ft8_lib/ft8/js8_jsc_encode.c \
 *       components/ft8_lib/ft8/js8_jsc.c \
 *       components/ft8_lib/ft8/js8_jsc_tables.c -lm && ./slice
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "js8_jsc.h"

#define MAX_FRAMES 12

/* Real traffic, not lorem ipsum: what an operator actually sends, plus the
 * long one that forces the multi-frame path the display change is for. */
static const char *kCases[] = {
    "QSL",
    "HELLO WORLD",
    "THANKS FOR THE CALL",
    "RIG IS A QMX RUNNING 5 WATTS INTO A DIPOLE",
    "73 AND GOOD DX",
    "TEMP IS 12C AND RAINING HERE IN DENMARK",
    "THE WEATHER IS ZZQX TODAY",          /* a word with no dictionary entry */
    "QRV 14078 AT 1200Z",
    "THIS IS A LONGER MESSAGE THAT WILL NOT FIT INTO ONE SINGLE FRAME AND "
    "THEREFORE HAS TO BE SPLIT ACROSS SEVERAL OF THEM IN SEQUENCE",
};

/* The same arithmetic js8_ftx.c does, deliberately duplicated HERE and nowhere
 * else: this harness exists to check that rule, so it has to state it. */
static int check_one(const char *text, int *widest)
{
    char norm[JS8_JSC_TEXT_MAX * 4];
    if (js8_jsc_normalize(text, norm, sizeof norm) <= 0) {
        printf("  [FAIL] normalise returned nothing for '%s'\n", text);
        return 1;
    }

    uint8_t frames[MAX_FRAMES * 9];
    uint8_t itypes[MAX_FRAMES];
    int     used[MAX_FRAMES] = { 0 };
    int n = js8_jsc_text_to_frames_used(norm, frames, itypes, MAX_FRAMES, used);
    if (n <= 0) {
        printf("  [FAIL] no frames for '%s'\n", norm);
        return 1;
    }

    printf("  %d frame%s  '%s'\n", n, n == 1 ? " " : "s", norm);

    char   rebuilt[sizeof norm];
    size_t out = 0;
    size_t from = 0;
    const size_t nlen = strlen(norm);
    int bad = 0;

    for (int i = 0; i < n; i++) {
        size_t take = (from < nlen) ? (size_t)used[i] : 0;
        if (from + take > nlen) take = nlen - from;
        printf("     %d/%d %.*s\n", i + 1, n, (int)take, norm + from);
        if (take > (size_t)*widest) *widest = (int)take;
        if (out + take < sizeof rebuilt) {
            memcpy(rebuilt + out, norm + from, take);
            out += take;
        }
        from += take;
    }
    rebuilt[out] = '\0';

    /* The two things that must hold, stated separately so a failure says
     * WHICH one broke rather than just "mismatch". */
    if (from != nlen) {
        printf("  [FAIL] the frames consumed %zu characters of %zu\n", from, nlen);
        bad++;
    }
    if (strcmp(rebuilt, norm) != 0) {
        printf("  [FAIL] the slices do not rebuild the message\n"
               "         want '%s'\n         got  '%s'\n", norm, rebuilt);
        bad++;
    }
    return bad;
}

int main(void)
{
    printf("JS8 per-frame slice harness - the counts must rebuild the message\n\n");

    int bad = 0;
    int widest = 0;
    for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++)
        bad += check_one(kCases[i], &widest);

    /* ⚠ THE DISPLAY FIELD IS 32 BYTES (ft8_tx_request_t::display_text) and
     * js8_ftx.c writes "NN/NN " plus the slice into it. Six characters of
     * prefix, so a frame carrying more than 25 would be truncated on screen.
     * snprintf makes that safe rather than dangerous, but it is still a word
     * the operator would not see - so the margin is reported, not assumed. */
    printf("\n  widest frame: %d characters; \"12/12 \" + %d = %d of 32 bytes\n",
           widest, widest, 6 + widest);
    if (6 + widest > 31) {
        printf("  [FAIL] a frame no longer fits the 32-byte display field\n");
        bad++;
    }

    printf("\njs8_frame_slice_harness: %s\n", bad ? "FAIL" : "pass");
    return bad ? 1 : 0;
}
