/* Host test for freq_gridlines_build() - the round values on the spectrum axis.
 *
 * Build (from the repo root):
 *   gcc -I main/util -o freq_gridlines_harness test/freq_gridlines_harness.c \
 *       main/util/freq_gridlines.c && ./freq_gridlines_harness
 *
 * Why this exists: the axis printed whatever frequency fell at five fixed pixel
 * positions, so every label carried one-hertz resolution and ticked as the
 * operator tuned - "14.006.070" counting up and down at x16. On a display whose
 * whole point is standing still that is unusable. Ticks must sit on round
 * values and stay there.
 *
 * It links the REAL function, not a copy.
 */
#include <stdio.h>
#include <inttypes.h>
#include "freq_gridlines.h"

static int fails = 0;
static void ok(const char *what, int cond)
{
    if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

/* Every value must be a multiple of the step, inside the window, ascending. */
static void check(const char *what, int64_t lo, int64_t hi, int max_n)
{
    int64_t v[8];
    int32_t step = 0;
    int n = freq_gridlines_build(lo, hi, max_n, v, &step);

    printf("  %-26s %8" PRId64 "..%-8" PRId64 " -> %d tick(s), step %" PRId32 " Hz\n",
           what, lo, hi, n, step);
    ok("at least one tick", n > 0);
    ok("within max_n", n <= max_n);
    if (n <= 0) return;
    ok("step reported", step > 0);
    for (int i = 0; i < n; i++) {
        ok("multiple of the step", v[i] % step == 0);
        ok("inside the window",    v[i] >= lo && v[i] <= hi);
        if (i) ok("ascending", v[i] > v[i - 1]);
    }
    /* The step must be the FINEST that fits - a coarser one than necessary
     * wastes the axis. Check that halving the count would have overflowed. */
    ok("no finer step would have fitted",
       (hi - lo) / step + 1 <= max_n);
}

int main(void)
{
    printf("spans the panadapter actually uses (5 labels)\n");
    check("x1   48 kHz", 14050000, 14098000, 5);
    check("x2   24 kHz", 14062000, 14086000, 5);
    check("x4   12 kHz", 14068000, 14080000, 5);
    check("x8    6 kHz", 14071000, 14077000, 5);
    check("x16   3 kHz", 14004570, 14007570, 5);
    check("x24   2 kHz", 14005000, 14007000, 5);

    printf("\nthe point of the exercise: labels do NOT move as you tune\n");
    {
        /* Slide the window one hertz at a time. Any tick that stays inside must
         * keep its exact value - that is what "frozen" means. The old axis
         * changed every label on every hertz. */
        int64_t prev[8]; int32_t st = 0;
        int pn = freq_gridlines_build(14004570, 14007570, 5, prev, &st);
        int moved = 0, checked = 0;
        for (int d = 1; d <= 400; d++) {
            int64_t cur[8]; int32_t st2 = 0;
            int cn = freq_gridlines_build(14004570 + d, 14007570 + d, 5, cur, &st2);
            for (int i = 0; i < pn; i++)
                for (int j = 0; j < cn; j++)
                    if (cur[j] > prev[i] - st && cur[j] < prev[i] + st) {
                        checked++;
                        if (cur[j] != prev[i]) moved++;
                    }
        }
        printf("  slid the window 400 Hz: %d overlapping ticks, %d changed value\n",
               checked, moved);
        ok("no surviving tick ever changes value", moved == 0);
    }

    printf("\nedges and nonsense\n");
    {
        int64_t v[8]; int32_t st;
        ok("hi == lo -> nothing",   freq_gridlines_build(14000000, 14000000, 5, v, &st) == 0);
        ok("hi < lo  -> nothing",   freq_gridlines_build(14000000, 13999000, 5, v, &st) == 0);
        ok("max_n 0  -> nothing",   freq_gridlines_build(14000000, 14048000, 0, v, &st) == 0);
        ok("NULL out -> nothing",   freq_gridlines_build(14000000, 14048000, 5, NULL, &st) == 0);
        ok("step cleared on refusal", (freq_gridlines_build(14000000, 14000000, 5, v, &st) == 0)
                                       && st == 0);
        /* A window narrower than one hertz cannot carry a grid. */
        ok("sub-hertz span -> nothing", freq_gridlines_build(14000000, 14000000, 5, v, &st) == 0);
        /* Exactly one step across: both ends land on a tick. */
        int n = freq_gridlines_build(14000000, 14001000, 5, v, &st);
        ok("both ends are ticks when they are round", n >= 2 && v[0] == 14000000);
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall pass\n", fails);
    return fails ? 1 : 0;
}
