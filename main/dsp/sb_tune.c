// sb_tune.c - see sb_tune.h for why the edge and not the peak, and for why
// this file is not in main/CMakeLists.txt yet.

#include "sb_tune.h"

#include <math.h>
#include <string.h>

#define SB_TUNE_MAX_BINS 128

// Insertion sort, same shape as ui.c's cw_peak_in() uses for its own median.
// The windows here are tens of bins, so an O(n^2) sort on the stack is cheaper
// than anything cleverer and has no allocation to fail.
static void sort_asc(float *v, int n)
{
    for (int i = 1; i < n; i++) {
        float k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
}

sb_tune_result_t sb_tune_find_edge(const float *win_db, int n, int centre_i,
                                   float bin_hz, bool upper,
                                   const sb_tune_cfg_t *cfg,
                                   float *edge_off_hz, float *snr_db)
{
    if (!win_db || !cfg || n < 8 || n > SB_TUNE_MAX_BINS) return SB_TUNE_NO_SIGNAL;
    if (centre_i < 0 || centre_i >= n) return SB_TUNE_NO_SIGNAL;
    if (bin_hz <= 0.0f) return SB_TUNE_NO_SIGNAL;

    float sorted[SB_TUNE_MAX_BINS];
    memcpy(sorted, win_db, (size_t)n * sizeof(float));
    sort_asc(sorted, n);

    /* ⛔ A LOW PERCENTILE, NOT THE MEDIAN, AND THIS IS THE WHOLE REASON THE
     * FIRST SNR GATE DID NOTHING.
     *
     * ui.c's cw_peak_in() uses the median and is right to: a CW carrier is a
     * couple of bins in a wide window, so half the window is noise. SSB is
     * not. An SSB signal is ~2.6 kHz wide and this window is +/-2 kHz, so the
     * signal fills about 55 of 87 bins and THE MEDIAN LANDS INSIDE IT. The
     * gate was then comparing the signal against itself: measured on the host,
     * acceptance was ~80 % at 10 dB and ~80 % at 40 dB - no discrimination at
     * all, while the sd stayed above 200 Hz. The gate looked present and did
     * nothing, which is worse than having none.
     *
     * The 10th percentile sits below the occupied fraction with room to spare
     * (32 of 87 bins are outside a 2.6 kHz signal), so it is in the noise
     * wherever the carrier happens to be - which matters, because we have not
     * found the carrier yet when this is computed. */
    const float noise = sorted[n / 10];

    /* The in-band reference is the median of the strongest few bins. A single
     * peak bin is one syllable's formant and moves every 100 ms; the whole
     * point of the edge is that it does not. */
    int nb = cfg->inband_bins;
    if (nb < 1) nb = 1;
    if (nb > n)  nb = n;
    const float inband = sorted[n - 1 - (nb - 1) / 2];

    const float snr = inband - noise;
    if (snr_db) *snr_db = snr;
    if (snr < cfg->min_snr_db) return SB_TUNE_NO_SIGNAL;

    /* The threshold sits below the in-band reference, and must stay clear of
     * the noise - down there it finds the window's own skirt or a neighbouring
     * station rather than this signal's edge.
     *
     * ⛔ THIS USED TO CLAMP, AND THE CLAMP WAS THE BUG. It read
     * `floor_guard = noise + 0.5f * snr` and pulled the threshold UP to it,
     * which quietly turned a weak signal into a confident wrong answer:
     * measured on the host at 10 and 20 dB SNR the edge came out near -50 Hz
     * regardless of the far station's filter, with sd over 200 Hz, and
     * SB_TUNE_NO_SIGNAL was never returned once. An aid that moves the
     * operator's dial on that is indistinguishable from the unexplained
     * automatic corrections Sam W7STF already reports in CW.
     *
     * Refusing is the honest answer. The caller can keep listening - ui.c's
     * CW tune already has the bounded wait for exactly this. */
    const float thresh = inband - cfg->edge_drop_db;
    if (thresh < noise + SB_TUNE_FLOOR_MARGIN_DB) return SB_TUNE_NO_SIGNAL;

    /* Walk inward from the carrier side: for USB the carrier is BELOW the
     * occupied band, so start at the low end of the window and find the first
     * bin that is in-band. For LSB start at the high end. The first crossing
     * is the edge; later crossings are gaps between syllables. */
    const int step  = upper ? 1 : -1;
    const int start = upper ? 0 : n - 1;

    int hit = -1;
    for (int i = start; i >= 0 && i < n; i += step) {
        if (win_db[i] >= thresh) { hit = i; break; }
    }
    if (hit < 0) return SB_TUNE_EDGE_NOT_FOUND;

    /* A crossing in the very first bin examined means the band is already
     * in-band at the window edge, so the real edge is outside the window and
     * any number computed here would be the window's boundary, not the
     * signal's. Refusing is the honest answer - the caller widens the window
     * or gives up. */
    if (hit == start) return SB_TUNE_EDGE_NOT_FOUND;

    /* Linear interpolation in dB between the last out-of-band bin and the
     * first in-band one. Linear and not parabolic on purpose: ui.c's parabola
     * fits the top of a peak, which is locally quadratic. An edge is a ramp,
     * and a parabola through three points on a ramp puts the vertex outside
     * the bracket. */
    const int   prev = hit - step;
    const float a = win_db[prev], b = win_db[hit];
    float frac = 0.5f;
    if (b != a) {
        frac = (thresh - a) / (b - a);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
    }
    const float edge_i = (float)prev + frac * (float)step;

    if (edge_off_hz) *edge_off_hz = (edge_i - (float)centre_i) * bin_hz;
    return SB_TUNE_OK;
}

/* Squared residual of the half-open bin range [a,b) about its own mean, from
 * prefix sums of x and x^2. Keeping it here rather than in a statement-
 * expression macro means no GCC extension and one place to read the algebra. */
static double sse_about_mean(const double *s1, const double *s2, int a, int b)
{
    const int    na = b - a;
    if (na <= 0) return 0.0;
    const double su = s1[b] - s1[a];
    const double sq = s2[b] - s2[a];
    const double sse = sq - (su * su) / (double)na;
    return sse > 0.0 ? sse : 0.0;   /* rounding can make an exact fit negative */
}

/* Smallest number of bins on either side of the step. A changepoint computed
 * from three bins is noise with an opinion; eight is enough for the means to
 * mean something at 23-47 Hz per bin. */
#define SB_FIT_MIN_SIDE 8

sb_tune_result_t sb_tune_find_edge_fit(const float *win_db, int n, int centre_i,
                                       float bin_hz, bool upper,
                                       const sb_tune_cfg_t *cfg,
                                       float *edge_off_hz, float *snr_db)
{
    if (!win_db || !cfg || n < 8 || n > SB_TUNE_MAX_BINS) return SB_TUNE_NO_SIGNAL;
    if (centre_i < 0 || centre_i >= n) return SB_TUNE_NO_SIGNAL;
    if (bin_hz <= 0.0f) return SB_TUNE_NO_SIGNAL;
    if (n < 2 * SB_FIT_MIN_SIDE + 1) return SB_TUNE_NO_SIGNAL;

    /* Work in an array ordered so index 0 is the end FURTHEST from the signal
     * and increasing index walks into it. For USB the carrier is below the
     * occupied band, so the window's low end is the noise end; for LSB it is
     * the high end. One code path, one mapping back at the finish. */
    float v[SB_TUNE_MAX_BINS];
    for (int i = 0; i < n; i++) v[i] = upper ? win_db[i] : win_db[n - 1 - i];

    /* Prefix sums make the squared-residual of every candidate split O(1), so
     * the whole scan is O(n) rather than O(n^2). */
    double s1[SB_TUNE_MAX_BINS + 1], s2[SB_TUNE_MAX_BINS + 1];
    s1[0] = 0.0; s2[0] = 0.0;
    for (int i = 0; i < n; i++) {
        s1[i + 1] = s1[i] + (double)v[i];
        s2[i + 1] = s2[i] + (double)v[i] * (double)v[i];
    }

    int    best_e   = -1;
    double best_sse = 0.0;
    for (int e = SB_FIT_MIN_SIDE; e <= n - SB_FIT_MIN_SIDE; e++) {
        const double sse = sse_about_mean(s1, s2, 0, e)
                         + sse_about_mean(s1, s2, e, n);
        if (best_e < 0 || sse < best_sse) { best_sse = sse; best_e = e; }
    }
    if (best_e < 0) return SB_TUNE_EDGE_NOT_FOUND;

    const double mean_lo = (s1[best_e] - s1[0]) / (double)best_e;
    const double mean_hi = (s1[n] - s1[best_e]) / (double)(n - best_e);
    const double step    = mean_hi - mean_lo;

    if (snr_db) *snr_db = (float)step;

    /* The step must be upward - noise then signal. A downward best split means
     * the window does not hold a carrier-side edge at all (the signal is the
     * wrong side, or there is no signal), and reporting its position would be
     * a confident answer to a question that was not asked. */
    if (step < (double)cfg->min_step_db) return SB_TUNE_NO_SIGNAL;

    /* Sub-bin position from the SSE curve's own shape. The curve is smooth and
     * locally quadratic near its minimum, so a parabola through the three
     * points brackets the true split - unlike the dB-domain parabola ui.c
     * fits to a PEAK, this one is fitted to the COST, which is what has a
     * minimum here. Skipped at the scan limits, where there is no neighbour. */
    double delta = 0.0;
    if (best_e > SB_FIT_MIN_SIDE && best_e < n - SB_FIT_MIN_SIDE) {
        const double a = sse_about_mean(s1, s2, 0, best_e - 1)
                       + sse_about_mean(s1, s2, best_e - 1, n);
        const double b = best_sse;
        const double c = sse_about_mean(s1, s2, 0, best_e + 1)
                       + sse_about_mean(s1, s2, best_e + 1, n);
        const double den = a - 2.0 * b + c;
        if (den != 0.0) {
            delta = 0.5 * (a - c) / den;
            if (delta >  0.5) delta =  0.5;
            if (delta < -0.5) delta = -0.5;
        }
    }
    #undef SSE_OF

    const double e_f = (double)best_e + delta;
    /* Map back out of the walk-into-the-signal ordering. The step sits BETWEEN
     * v[e-1] and v[e], i.e. at e - 0.5 in bin centres. */
    const double idx = upper ? (e_f - 0.5)
                             : ((double)(n - 1) - (e_f - 0.5));

    if (edge_off_hz) *edge_off_hz = (float)((idx - (double)centre_i) * (double)bin_hz);
    return SB_TUNE_OK;
}

unsigned sb_tune_snap(unsigned freq_hz, unsigned grid_hz)
{
    if (grid_hz == 0) return freq_hz;
    const unsigned half = grid_hz / 2;
    return ((freq_hz + half) / grid_hz) * grid_hz;
}
