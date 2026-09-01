#include "db_gridlines.h"
#include <math.h>

/* Candidates in increasing coarseness. 5/10/20/25/50/100 covers every range the
 * dB Range sliders can produce; the last one is the fallback if even it does not
 * fit, which cannot happen for any reachable slider range but is handled anyway.
 */
static const float k_steps[] = { 5.0f, 10.0f, 20.0f, 25.0f, 50.0f, 100.0f };
#define K_NSTEPS ((int)(sizeof(k_steps) / sizeof(k_steps[0])))

/* Guard against a tick landing exactly on an edge, where it would be drawn
 * under the frame and its label clamped on top of the neighbouring one. Small
 * relative to any real dB step, large enough to absorb float error from the
 * ceilf() below. */
#define EDGE_EPS 0.001f

static int count_ticks(float lo, float hi, float step)
{
    float first = ceilf((lo + EDGE_EPS) / step) * step;
    int n = 0;
    for (float v = first; v <= hi - EDGE_EPS; v += step) n++;
    return n;
}

int db_gridlines_build(float lo, float hi, int max_n, float *out)
{
    if (!out || max_n <= 0 || !(hi > lo)) return 0;   /* NaN-safe: !(hi > lo) */

    float step = k_steps[K_NSTEPS - 1];
    for (int i = 0; i < K_NSTEPS; i++) {
        if (count_ticks(lo, hi, k_steps[i]) <= max_n) { step = k_steps[i]; break; }
    }

    float first = ceilf((lo + EDGE_EPS) / step) * step;
    int n = count_ticks(lo, hi, step);
    if (n > max_n) n = max_n;

    /* Descending, so out[0] is the top of the screen. */
    for (int i = 0; i < n; i++) out[i] = first + (float)(n - 1 - i) * step;
    return n;
}
