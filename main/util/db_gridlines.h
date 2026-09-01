#pragma once
/* Round dBm gridline values for the spectrum's right-edge scale.
 *
 * Portable on purpose (no ESP deps) so test/db_gridlines_harness.c can link the
 * REAL function rather than a copy of it. The arithmetic looks trivial and is
 * not: the step choice, the strictly-inside edge rule and the descending order
 * all have off-by-one traps, and the whole point of the change is that a value
 * the operator picked must be described honestly.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Fill `out` with at most `max_n` round gridline values strictly inside
 * (lo, hi), HIGHEST FIRST (out[0] is the topmost line on screen, and the one
 * that carries the " dBm" suffix). Returns how many were written.
 *
 * The step is the smallest candidate that still fits within max_n lines, so a
 * narrow range gets finer lines rather than fewer. Returns 0 for a degenerate
 * range (hi <= lo) - callers must treat 0 as "draw no scale", never as an error
 * to paper over with a default.
 */
int db_gridlines_build(float lo, float hi, int max_n, float *out);

#ifdef __cplusplus
}
#endif
