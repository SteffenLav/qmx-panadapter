/* See pan_view.h for why this is portable and what it is guarding against. */
#include "pan_view.h"

/* Round a signed value to nearest, away from zero on a tie. Integer only: the
 * whole point of this file is that the mapping is exact and reproducible, and
 * float rounding differs between the host harness and the device toolchain. */
static int64_t div_round(int64_t num, int64_t den)
{
    if (den == 0) return 0;
    if ((num < 0) != (den < 0)) return -(((-num) + den / 2) / den);
    return (num + den / 2) / den;
}

static int cfg_ok(const pan_view_cfg_t *c)
{
    return c && c->sample_rate_hz > 0 && c->n_bins >= 2 && (c->n_bins % 2) == 0 &&
           c->screen_w > 0 && c->zoom >= 1.0f;
}

void pan_view_resolve(const pan_view_cfg_t *c, int64_t want_lo_hz, pan_view_t *v)
{
    if (!v) return;
    v->ok = 0;
    v->cap_lo_hz = v->cap_hi_hz = v->lo_hz = v->hi_hz = 0;
    v->span_hz = 0;
    if (!cfg_ok(c)) return;

    /* The LO sits if_offset_hz below the dial, and the complex spectrum spans
     * +/- half the sample rate around it. */
    int64_t lo_osc = c->dial_hz - (int64_t)c->if_offset_hz;
    int32_t half   = c->sample_rate_hz / 2;
    v->cap_lo_hz = lo_osc - half;
    v->cap_hi_hz = lo_osc + half;

    /* zoom is a float and may be fractional (x1.5), so this one division is
     * done in double and rounded once. Everything after it is integer. */
    int32_t span = (int32_t)((double)c->sample_rate_hz / (double)c->zoom + 0.5);
    if (span < 1) span = 1;
    if (span > c->sample_rate_hz) span = c->sample_rate_hz;

    int64_t lo = (want_lo_hz == PAN_VIEW_CENTRE)
                     ? c->dial_hz - span / 2
                     : want_lo_hz;

    /* Clamp only when asked. Order matters: pushing the top down first and then
     * the bottom up means a span equal to the capture width lands exactly on it
     * rather than oscillating. */
    if (c->clamp_to_capture) {
        if (lo + span > v->cap_hi_hz) lo = v->cap_hi_hz - span;
        if (lo < v->cap_lo_hz)        lo = v->cap_lo_hz;
    }

    v->lo_hz   = lo;
    v->hi_hz   = lo + span;
    v->span_hz = span;
    v->ok      = 1;
}

int64_t pan_view_x_to_hz(const pan_view_cfg_t *c, const pan_view_t *v, int x)
{
    if (!cfg_ok(c) || !v || !v->ok) return 0;
    /* Column CENTRE, so column 0 is half a pixel in rather than exactly on the
     * left edge - the same convention the trace is drawn with. */
    return v->lo_hz + div_round((int64_t)(2 * x + 1) * v->span_hz, 2LL * c->screen_w);
}

int pan_view_hz_to_x(const pan_view_cfg_t *c, const pan_view_t *v, int64_t hz)
{
    if (!cfg_ok(c) || !v || !v->ok || v->span_hz <= 0) return 0;
    return (int)div_round((hz - v->lo_hz) * c->screen_w, v->span_hz);
}

int pan_view_x_to_bin(const pan_view_cfg_t *c, const pan_view_t *v, int x)
{
    if (!cfg_ok(c) || !v || !v->ok) return PAN_VIEW_NO_DATA;

    int64_t hz = pan_view_x_to_hz(c, v, x);
    if (hz < v->cap_lo_hz || hz >= v->cap_hi_hz) return PAN_VIEW_NO_DATA;

    /* Absolute -> baseband, then baseband -> bin. NO modulo wrap: a column
     * outside the capture window has already returned above, and anything that
     * still lands outside the bin range is a bug to surface, not to fold. */
    int64_t base = hz - (c->dial_hz - (int64_t)c->if_offset_hz);
    int64_t k    = div_round(base * c->n_bins, c->sample_rate_hz);

    int32_t half = c->n_bins / 2;
    /* k == +half is the Nyquist bin, index n/2, and it is a REAL bin holding
     * real data - the standard +/- ambiguity, not a wrap. Rejecting it left the
     * last column of a zoom-1 view dead, and clamping it to half-1 would have
     * told a small lie instead. Both signs fold to the same index. */
    if (k < -half || k > half) return PAN_VIEW_NO_DATA;
    if (k < 0) k += c->n_bins;
    return (int)k;
}

int pan_view_cursor_range(const pan_view_cfg_t *c, float *lo_frac, float *hi_frac)
{
    if (!cfg_ok(c)) return 0;
    double span = (double)c->sample_rate_hz / (double)c->zoom;
    if (span <= 0.0) return 0;

    /* A still view [V, V+span] must stay inside [dial-below, dial+above], which
     * bounds where the dial can be relative to V:
     *     dial >= V + span - above      and      dial <= V + below
     * Expressed as a fraction of the view, and intersected with "on screen". */
    double above = (double)c->sample_rate_hz / 2.0 - (double)c->if_offset_hz;
    double below = (double)c->sample_rate_hz / 2.0 + (double)c->if_offset_hz;

    double lo = (span - above) / span;
    double hi = below / span;
    if (lo < 0.0) lo = 0.0;
    if (hi > 1.0) hi = 1.0;
    if (hi < lo)  hi = lo;

    if (lo_frac) *lo_frac = (float)lo;
    if (hi_frac) *hi_frac = (float)hi;
    return 1;
}
