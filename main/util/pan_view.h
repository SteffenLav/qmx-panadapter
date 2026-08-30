#pragma once
/* The panadapter's ONE mapping between screen columns, absolute frequency and
 * FFT bins.
 *
 * Portable on purpose (no ESP deps) so test/pan_view_harness.c can link the REAL
 * functions rather than a copy. The arithmetic is small and the traps are not:
 *
 *  - The QMX's LO sits `if_offset_hz` BELOW the dial, so the spectrum it can
 *    hear is NOT centred on the dial. At the usual 12 kHz it runs
 *    dial-36 kHz .. dial+12 kHz. A view centred on the dial therefore asks for
 *    12 kHz that does not exist, and the old code filled it by taking the bin
 *    index modulo N - drawing real spectrum from 24-36 kHz BELOW the dial in the
 *    right-hand quarter, under an axis claiming dial+12..+24 (#297, confirmed on
 *    air 2026-08-30). `pan_view_x_to_bin()` returns PAN_VIEW_NO_DATA there
 *    instead. It must never wrap.
 *
 *  - The axis, the VFO cursor, the passband edges, the RIT marker, the spots
 *    lane, the band-plan strip and tap-to-tune must all agree. They used to
 *    derive the geometry separately, which is how the v1.8.1 tap-to-tune bug
 *    happened. One mapping, or they drift again.
 *
 * Everything here is stateless: the caller owns the viewport's left edge and
 * passes it in, so the same functions serve today's dial-centred display and a
 * still one (#298).
 */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAN_VIEW_NO_DATA  (-1)      /* column falls outside what the radio hears */
#define PAN_VIEW_CENTRE   INT64_MIN /* want_lo_hz: centre the view on the dial   */

typedef struct {
    int32_t sample_rate_hz;   /* 48000 */
    int32_t n_bins;           /* FFT size, e.g. 1024 - must be even */
    int32_t screen_w;         /* pixels across the spectrum, e.g. 1280 */
    int64_t dial_hz;          /* the frequency the operator is tuned to */
    int32_t if_offset_hz;     /* baseband Hz the dial maps to - ui_get_if_offset_hz() */
    float   zoom;             /* 1.0 = the whole sample rate on screen */

    /* Hold the view inside the capture window? DEFAULT (false) is the shipping
     * behaviour: the view stays where it is asked to be and any column outside
     * what the radio hears comes back as PAN_VIEW_NO_DATA, to be drawn as
     * visibly empty. Clamping is what drags the view along with the dial, and a
     * view dragged by the dial is not a still one - the operator's call,
     * 2026-08-30: "Let it go blank - or it is not a moving vfo."
     *
     * Set true only where a deliberate re-frame wants to land somewhere useful. */
    bool    clamp_to_capture;
} pan_view_cfg_t;

typedef struct {
    int64_t cap_lo_hz, cap_hi_hz;  /* what the radio can hear, absolute */
    int64_t lo_hz, hi_hz;          /* what is on screen, clamped into the above */
    int32_t span_hz;               /* hi_hz - lo_hz */
    int     ok;                    /* 0 = degenerate config, draw nothing */
} pan_view_t;

/* Resolve the viewport. `want_lo_hz` is the REQUESTED left edge, or
 * PAN_VIEW_CENTRE to centre on the dial. The result is always clamped inside
 * the capture window, so a caller can ask for anything and still get a view
 * that is entirely real spectrum. At zoom 1 the clamp pins it to the capture
 * window exactly, which puts the dial 75% across - that is not a bug, it is the
 * only 48 kHz window that exists. */
void pan_view_resolve(const pan_view_cfg_t *c, int64_t want_lo_hz, pan_view_t *v);

/* Column centre -> absolute Hz. Defined for any x; callers may pass values
 * outside [0, screen_w) to ask "where would this be". */
int64_t pan_view_x_to_hz(const pan_view_cfg_t *c, const pan_view_t *v, int x);

/* Absolute Hz -> column. Deliberately NOT clamped: a caller drawing the VFO
 * cursor needs to know it is off-screen so it can draw an edge arrow instead of
 * parking the cursor on the edge, which would read as "tuned here". */
int pan_view_hz_to_x(const pan_view_cfg_t *c, const pan_view_t *v, int64_t hz);

/* Column -> FFT bin index in [0, n_bins), or PAN_VIEW_NO_DATA if that column is
 * outside the capture window. Bins are in the usual FFT order: 0..n/2-1 are
 * positive baseband, n/2..n-1 are negative. */
int pan_view_x_to_bin(const pan_view_cfg_t *c, const pan_view_t *v, int x);

/* Where the VFO cursor can sit inside a STILL view, as fractions of the view
 * width (#298). The ceiling at dial + if_offset_hz means a fixed view has to sit
 * high enough to stay inside the capture window, which stops the cursor short of
 * the left edge at low zoom: pinned at 0.75 at zoom 1, the right half at zoom 2,
 * the whole width from zoom 4. Returns 0 and leaves the outputs alone on a
 * degenerate config. */
int pan_view_cursor_range(const pan_view_cfg_t *c, float *lo_frac, float *hi_frac);

#ifdef __cplusplus
}
#endif
