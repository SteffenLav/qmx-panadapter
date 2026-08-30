#pragma once
/* Round frequency values for the spectrum's bottom axis.
 *
 * Portable on purpose (no ESP deps) so test/freq_gridlines_harness.c can link
 * the REAL function. Same idea, and the same traps, as db_gridlines.c.
 *
 * Why it exists: the axis used to put five labels at fixed pixel positions and
 * print whatever frequency happened to fall there - centre + pan + span*(i-2)/4
 * - so every label carried one-hertz resolution and CHANGED as the operator
 * tuned. At x16 that reads as "14.006.070" ticking up and down digit by digit,
 * which is unusable on a display whose whole point is standing still. The
 * operator's requirement, 2026-08-30: "those freq labels show a resolution of
 * one Hz - they need to be frozen within the shown window".
 *
 * So the axis now works the way an instrument's does: ticks sit on ROUND
 * frequencies and the label stays put while the window moves under it.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fill `out` with at most `max_n` round frequencies inside [lo_hz, hi_hz],
 * ASCENDING, and return how many were written. The step is the smallest 1-2-5
 * value that still fits within max_n, so a narrow span gets a finer grid rather
 * than fewer lines.
 *
 * Returns 0 for a degenerate span (hi <= lo) - a caller must treat that as
 * "draw no axis", never as an error to paper over with a default.
 *
 * `out_step_hz` (optional) receives the step chosen, so the caller can decide
 * how much resolution the LABEL needs: printing hertz under a 1 kHz grid is
 * exactly the noise this replaces. */
int freq_gridlines_build(int64_t lo_hz, int64_t hi_hz, int max_n,
                         int64_t *out, int32_t *out_step_hz);

#ifdef __cplusplus
}
#endif
