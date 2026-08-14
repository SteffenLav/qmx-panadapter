/* Host test for db_gridlines_build() - the dBm gridline values on the spectrum's
 * right-edge scale.
 *
 * Build (from the repo root):
 *   gcc -I main/util -o db_gridlines_harness test/db_gridlines_harness.c \
 *       main/util/db_gridlines.c -lm && ./db_gridlines_harness
 *
 * Why this exists: the gridlines used to be the hardcoded set
 * { -40, -60, -80, -100, -120 }, which silently assumed the default -130..-30
 * range. Samuel W7STF ran -118/-13 and the labels no longer described his scale
 * (2026-08-14). The replacement is small but has real traps - step selection,
 * the strictly-inside edge rule, descending order - and none of them are worth
 * discovering on the glass with a wedged radio attached.
 *
 * It links the REAL function, not a copy. A harness that mirrors the code under
 * test only ever proves the mirror.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "db_gridlines.h"

static int g_fail = 0;

static void expect(const char *name, float lo, float hi,
                   const float *want, int want_n)
{
    float got[8];
    int n = db_gridlines_build(lo, hi, 5, got);
    int ok = (n == want_n);
    for (int i = 0; ok && i < n; i++) {
        if (fabsf(got[i] - want[i]) > 0.01f) ok = 0;
    }
    printf("%-46s %s  got:", name, ok ? "PASS" : "FAIL");
    for (int i = 0; i < n; i++) printf(" %g", (double)got[i]);
    if (!ok) {
        printf("   WANTED:");
        for (int i = 0; i < want_n; i++) printf(" %g", (double)want[i]);
        g_fail++;
    }
    printf("\n");
}

static void expect_n(const char *name, float lo, float hi, int max_n, int want_n)
{
    float got[8];
    int n = db_gridlines_build(lo, hi, max_n, got);
    int ok = (n == want_n);
    /* Every value must be strictly inside and in descending order. */
    for (int i = 0; ok && i < n; i++) {
        if (!(got[i] > lo && got[i] < hi)) ok = 0;
        if (i && !(got[i] < got[i - 1]))   ok = 0;
    }
    printf("%-46s %s  n=%d\n", name, ok ? "PASS" : "FAIL", n);
    if (!ok) g_fail++;
}

int main(void)
{
    /* THE ONE THAT MATTERS MOST: at the shipped default range the derived set
     * must be byte-for-byte the old hardcoded one, or this "fix" is a visible
     * change for every operator who never touched the sliders. */
    const float def[] = { -40.0f, -60.0f, -80.0f, -100.0f, -120.0f };
    expect("default range -130..-30 == the old hardcoded set", -130.0f, -30.0f, def, 5);

    /* Samuel's range. -100..-20 on a 20 dB step: 5 lines, all inside. */
    const float sam[] = { -20.0f, -40.0f, -60.0f, -80.0f, -100.0f };
    expect("Samuel W7STF's -118..-13", -118.0f, -13.0f, sam, 5);

    /* A narrow range must get FINER lines, not fewer. */
    const float narrow[] = { -100.0f, -105.0f, -110.0f, -115.0f, -120.0f };
    expect("narrow -122..-98 uses the 5 dB step", -122.0f, -98.0f, narrow, 5);

    /* A very wide range must coarsen rather than overflow the label array.
     * 25 dB would give 7 ticks inside (-200, 0), so it steps up to 50 and yields
     * -150/-100/-50. (I first wrote 4 here and the harness caught me, which is
     * the whole argument for having it.) */
    expect_n("very wide -200..0 stays within 5 labels", -200.0f, 0.0f, 5, 3);

    /* Wider than even the coarsest step can cover in max_n lines. Not reachable
     * from the dB Range sliders, but this is where the `n > max_n` clamp is the
     * only thing standing between us and a write past the caller's array - and a
     * mutation run proved no other case exercised it. Caller buffers are sized
     * max_n exactly (DB_SCALE_MAX_LBLS), so a miss here is a stack smash. */
    expect_n("absurdly wide -1000..0 clamps to max_n", -1000.0f, 0.0f, 5, 5);

    /* Edge rule: a tick landing exactly on a boundary is excluded, because it
     * would be drawn under the frame with its label clamped onto its neighbour. */
    const float edge[] = { -40.0f, -60.0f, -80.0f, -100.0f };
    expect("ticks exactly on both edges are excluded", -120.0f, -20.0f, edge, 4);

    /* Degenerate inputs must yield 0 - "draw no scale" - never a stale or
     * invented default. */
    expect_n("degenerate hi == lo", -80.0f, -80.0f, 5, 0);
    expect_n("inverted hi < lo",   -30.0f, -130.0f, 5, 0);
    expect_n("max_n = 0",          -130.0f, -30.0f, 0, 0);
    {
        float got[8];
        int n = db_gridlines_build(-130.0f, -30.0f, 5, NULL);
        printf("%-46s %s  n=%d\n", "NULL out buffer", n == 0 ? "PASS" : "FAIL", n);
        if (n != 0) g_fail++;
        n = db_gridlines_build(NAN, -30.0f, 5, got);
        printf("%-46s %s  n=%d\n", "NaN low bound", n == 0 ? "PASS" : "FAIL", n);
        if (n != 0) g_fail++;
    }

    /* max_n is a hard bound whatever the range. */
    for (int m = 1; m <= 5; m++) {
        float got[8];
        int n = db_gridlines_build(-130.0f, -30.0f, m, got);
        if (n > m) { printf("max_n bound violated at m=%d (n=%d)\n", m, n); g_fail++; }
    }

    printf("\n%s\n", g_fail ? "FAILURES ABOVE" : "all checks passed");
    return g_fail ? 1 : 0;
}
