/* See freq_gridlines.h for why this is portable and what it replaces. */
#include "freq_gridlines.h"

/* 1-2-5 per decade, 1 Hz to 25 MHz. Deliberately NOT including 25/250/2500:
 * a 2.5 kHz grid reads as an odd number on a frequency axis even though the
 * band plans use 25 Hz steps, and the label is what has to be legible here. */
static const int32_t k_steps[] = {
    1, 2, 5,
    10, 20, 50,
    100, 200, 500,
    1000, 2000, 5000,
    10000, 20000, 50000,
    100000, 200000, 500000,
    1000000, 2000000, 5000000,
    10000000, 20000000,
};

int freq_gridlines_build(int64_t lo_hz, int64_t hi_hz, int max_n,
                         int64_t *out, int32_t *out_step_hz)
{
    if (out_step_hz) *out_step_hz = 0;
    if (!out || max_n <= 0 || hi_hz <= lo_hz) return 0;

    int64_t span = hi_hz - lo_hz;

    for (unsigned i = 0; i < sizeof k_steps / sizeof k_steps[0]; i++) {
        int32_t step = k_steps[i];

        /* First multiple of `step` at or above lo. Integer floor-division that
         * works for negative values too - a frequency axis will not see them,
         * but a caller reusing this for an offset scale would, and a silent
         * off-by-one at zero is exactly the kind of thing that survives review. */
        int64_t q = lo_hz / step;
        if (lo_hz % step != 0 && lo_hz < 0) q -= 1;
        int64_t first = (q * step >= lo_hz) ? q * step : (q + 1) * step;

        if (first > hi_hz) continue;          /* nothing of this step in view */

        int64_t n = (hi_hz - first) / step + 1;
        if (n > max_n) continue;              /* too fine - try a coarser step */

        /* A single tick is not a grid; it gives the eye nothing to measure
         * against and lands wherever the window happens to sit. Prefer the next
         * finer step unless nothing finer fits, which the loop has already
         * established by arriving here. */
        for (int k = 0; k < (int)n; k++) out[k] = first + (int64_t)k * step;
        if (out_step_hz) *out_step_hz = step;
        return (int)n;
    }

    (void)span;
    return 0;   /* span wider than the coarsest step - nothing sensible to draw */
}
