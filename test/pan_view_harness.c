/* Host test for pan_view - the panadapter's one column/frequency/bin mapping.
 *
 * Build (from the repo root):
 *   gcc -I main/util -o pan_view_harness test/pan_view_harness.c \
 *       main/util/pan_view.c -lm && ./pan_view_harness
 *
 * Why this exists: #297, confirmed on air 2026-08-30. The old mapping took the
 * bin index modulo N, so at zoom 1 the right-hand quarter of the screen drew
 * real spectrum from 24-36 kHz BELOW the dial under an axis claiming
 * dial+12..+24 kHz. Tapping a signal there tuned ~48 kHz from where it was.
 *
 * The replacement is a handful of divisions, and every one of them has a trap:
 * the LO offset that makes the capture window asymmetric, the clamp order, the
 * column-centre convention, and the bin fold for negative baseband. None of
 * them are worth discovering on the glass with a radio attached.
 *
 * It links the REAL functions, not a copy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pan_view.h"

static int fails = 0;

static void okeq(const char *what, long long got, long long want)
{
    if (got != want) { printf("  FAIL %-46s got %lld want %lld\n", what, got, want); fails++; }
}
static void okneard(const char *what, double got, double want, double tol)
{
    double d = got - want; if (d < 0) d = -d;
    if (d > tol) { printf("  FAIL %-46s got %.4f want %.4f\n", what, got, want); fails++; }
}

static pan_view_cfg_t base(float zoom, int64_t dial, int32_t ifoff)
{
    pan_view_cfg_t c;
    memset(&c, 0, sizeof c);
    c.sample_rate_hz = 48000; c.n_bins = 1024; c.screen_w = 1280;
    c.dial_hz = dial; c.if_offset_hz = ifoff; c.zoom = zoom;
    return c;
}

int main(void)
{
    pan_view_t v;

    /* 1. The capture window is asymmetric about the dial, and that is the whole
     *    reason #297 exists. 14.074 with the usual 12 kHz IF. */
    printf("capture window\n");
    pan_view_cfg_t c1 = base(1.0f, 14074000, 12000);
    pan_view_resolve(&c1, PAN_VIEW_CENTRE, &v);
    okeq("cap_lo", v.cap_lo_hz, 14074000 - 36000);
    okeq("cap_hi", v.cap_hi_hz, 14074000 + 12000);
    okeq("ok", v.ok, 1);

    /* 2. At zoom 1 a dial-centred request is CLAMPED onto the capture window -
     *    the only 48 kHz window that exists - which puts the dial 75% across. */
    printf("zoom 1 clamps to the capture window\n");
    okeq("view lo", v.lo_hz, 14074000 - 36000);
    okeq("view hi", v.hi_hz, 14074000 + 12000);
    okeq("span", v.span_hz, 48000);
    okeq("dial x (75%% of 1280)", pan_view_hz_to_x(&c1, &v, 14074000), 960);

    /* 3. THE #297 REGRESSION TEST. Sitting at 14.100, an FT8 signal at 14.0755
     *    is inside the capture window and must appear in the LEFT part of the
     *    screen. Nothing may be drawn above the ceiling at dial+12 kHz. */
    printf("#297 - no wrap above the ceiling\n");
    pan_view_cfg_t c3 = base(1.0f, 14100000, 12000);
    pan_view_resolve(&c3, PAN_VIEW_CENTRE, &v);
    okeq("cap_hi is dial+12k", v.cap_hi_hz, 14112000);
    okeq("view hi never exceeds cap_hi", v.hi_hz, 14112000);
    int x_ft8 = pan_view_hz_to_x(&c3, &v, 14075500);
    if (x_ft8 < 0 || x_ft8 >= 1280) { printf("  FAIL FT8 off screen at x=%d\n", x_ft8); fails++; }
    if (x_ft8 > 640) { printf("  FAIL FT8 in the RIGHT half at x=%d - that is the wrap\n", x_ft8); fails++; }
    /* Every column must be real data or explicitly none - never a wrapped bin. */
    int nodata = 0;
    for (int x = 0; x < 1280; x++) {
        int b = pan_view_x_to_bin(&c3, &v, x);
        if (b == PAN_VIEW_NO_DATA) { nodata++; continue; }
        if (b < 0 || b >= 1024) { printf("  FAIL bin %d out of range at x=%d\n", b, x); fails++; break; }
        /* The bin must map back to the frequency the axis claims for that column. */
        int64_t hz = pan_view_x_to_hz(&c3, &v, x);
        int64_t base_hz = hz - (c3.dial_hz - c3.if_offset_hz);
        /* Index 512 is Nyquist and means BOTH +24000 and -24000, so accept
         * whichever sign is nearer - anything else is a genuine mismatch. */
        double f1 = (double)b * 48000.0 / 1024.0;
        double f2 = (double)(b - 1024) * 48000.0 / 1024.0;
        double d1 = f1 - (double)base_hz, d2 = f2 - (double)base_hz;
        if (d1 < 0) d1 = -d1;
        if (d2 < 0) d2 = -d2;
        okneard("bin<->hz agree", d1 < d2 ? d1 : d2, 0.0, 24.0);
        if (fails) break;
    }
    okeq("no column is 'no data' once clamped", nodata, 0);

    /* 4. Zoom 4 centred on the dial fits entirely inside the capture window, so
     *    it is NOT clamped and the dial sits mid-screen. */
    printf("zoom 4 is unclamped\n");
    pan_view_cfg_t c4 = base(4.0f, 14074000, 12000);
    pan_view_resolve(&c4, PAN_VIEW_CENTRE, &v);
    okeq("span", v.span_hz, 12000);
    okeq("view lo", v.lo_hz, 14074000 - 6000);
    okeq("dial x (centre)", pan_view_hz_to_x(&c4, &v, 14074000), 640);

    /* 5. An arbitrary requested left edge is honoured when it fits, and clamped
     *    when it does not. This is what a still view needs. */
    printf("arbitrary viewport, clamped\n");
    pan_view_resolve(&c4, 14060000, &v);
    okeq("honoured", v.lo_hz, 14060000);
    pan_view_resolve(&c4, 14200000, &v);          /* far above the ceiling */
    okeq("clamped to cap_hi-span", v.lo_hz, 14074000 + 12000 - 12000);
    pan_view_resolve(&c4, 14000000, &v);          /* far below the floor */
    okeq("clamped to cap_lo", v.lo_hz, 14074000 - 36000);

    /* 6. x -> Hz -> x round-trips, and the two ends land where the axis says. */
    printf("round trip\n");
    pan_view_resolve(&c4, PAN_VIEW_CENTRE, &v);
    for (int x = 0; x < 1280; x += 97) {
        int64_t hz = pan_view_x_to_hz(&c4, &v, x);
        int back = pan_view_hz_to_x(&c4, &v, hz);
        if (back < x - 1 || back > x + 1) { printf("  FAIL round trip x=%d -> %lld -> %d\n", x, hz, back); fails++; }
    }

    /* 7. CW: the offset moves the whole mapping, because the dial then maps to
     *    12 kHz + the CW pitch in baseband. A 700 Hz pitch shifts the capture
     *    window down by 700 Hz. */
    printf("CW pitch shifts the window\n");
    pan_view_cfg_t c7 = base(1.0f, 14030000, 12700);
    pan_view_resolve(&c7, PAN_VIEW_CENTRE, &v);
    okeq("cap_lo", v.cap_lo_hz, 14030000 - 36700);
    okeq("cap_hi", v.cap_hi_hz, 14030000 + 11300);

    /* 8. The #298 cursor range - the table in the plan, derived not hardcoded. */
    printf("cursor range per zoom\n");
    struct { float z; double lo, hi; } want[] = {
        { 1.0f, 0.75, 0.75 }, { 2.0f, 0.50, 1.00 },
        { 3.0f, 0.25, 1.00 }, { 4.0f, 0.00, 1.00 }, { 8.0f, 0.00, 1.00 },
    };
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        pan_view_cfg_t cc = base(want[i].z, 14074000, 12000);
        float lo = -1, hi = -1;
        okeq("range ok", pan_view_cursor_range(&cc, &lo, &hi), 1);
        char n[64];
        snprintf(n, sizeof n, "zoom %.0f cursor lo", (double)want[i].z);
        okneard(n, lo, want[i].lo, 0.001);
        snprintf(n, sizeof n, "zoom %.0f cursor hi", (double)want[i].z);
        okneard(n, hi, want[i].hi, 0.001);
    }

    /* 9. Degenerate configs must say "draw nothing", never divide by zero or
     *    hand back a plausible-looking window. */
    printf("degenerate configs\n");
    pan_view_cfg_t bad = base(1.0f, 14074000, 12000);
    bad.zoom = 0.0f;   pan_view_resolve(&bad, PAN_VIEW_CENTRE, &v); okeq("zoom 0",   v.ok, 0);
    bad = base(1.0f, 14074000, 12000); bad.n_bins = 1023;
    pan_view_resolve(&bad, PAN_VIEW_CENTRE, &v); okeq("odd n_bins", v.ok, 0);
    bad = base(1.0f, 14074000, 12000); bad.screen_w = 0;
    pan_view_resolve(&bad, PAN_VIEW_CENTRE, &v); okeq("screen_w 0", v.ok, 0);
    bad = base(1.0f, 14074000, 12000); bad.sample_rate_hz = 0;
    pan_view_resolve(&bad, PAN_VIEW_CENTRE, &v); okeq("rate 0",    v.ok, 0);
    okeq("x_to_bin on a dead view", pan_view_x_to_bin(&bad, &v, 10), PAN_VIEW_NO_DATA);

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall pass\n", fails);
    return fails ? 1 : 0;
}
