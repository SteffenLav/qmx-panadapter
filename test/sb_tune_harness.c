// sb_tune_harness.c - measures the SSB edge estimator's BIAS on the host. T90.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main -I main/dsp -o test/sb_tune_harness.exe test/sb_tune_harness.c main/dsp/sb_tune.c -lm
//   ./test/sb_tune_harness.exe
//
// WHY THIS EXISTS
//   T90 wants to pull a roughly-tuned SSB signal onto the right dial. The plan
//   is: estimate the suppressed-carrier end of the occupied band, then SNAP to
//   the operator's tune grid, because there is no absolute correct SSB dial -
//   only the frequency the other operator chose, and that is almost certainly
//   a round number.
//
//   The whole idea stands or falls on one quantity nobody had measured: HOW FAR
//   THE DETECTED EDGE SITS FROM THE ACTUAL CARRIER. Two systematic errors push
//   it, and both are of the same order as the grid step itself:
//
//     1. THEIR filter. Voice has no energy below ~300 Hz and the transmitting
//        station's own low cut is there too, so the edge is a few hundred Hz
//        inside the carrier. That number belongs to their rig and we cannot
//        know it. Measured here across 100 / 200 / 300 / 400 Hz.
//     2. OUR window. A bin is 48000/1024 = 46.875 Hz, and the default
//        Blackman-Harris main lobe is about four bins, so roughly 190 Hz of
//        the apparent slope is the analysis window, not the signal. Hann is
//        narrower and is measured alongside it.
//
//   If the TOTAL error stays inside half a snap step the grid rounds it away
//   and T90 works. If it does not, stage 1 cannot carry the snap and the voice
//   stage stops being optional. That verdict is this harness's only job, and it
//   costs no bench time and no QMX power cycle.
//
// WHAT IT MEASURES
//   1. Bias and spread of (detected edge - true carrier), swept over the far
//      station's low cut, the true dial error, pitch, SNR and both windows.
//   2. The number that actually decides it: after snapping, how often the dial
//      lands EXACTLY on the other operator's frequency.
//   3. A mutation check - a deliberately wrong estimator must be caught here,
//      or this harness is decoration.
//
// ⛔ WHAT IT DOES NOT MEASURE
//   Real speech. The voice here is a synthetic glottal harmonic stack with
//   formant shaping - right for an EDGE, because the edge is set by the
//   transmit filter and not by the talker, but it has none of real speech's
//   pauses, plosives or level swings. A real recording is a later validation
//   step, not a substitute for this sweep.
//
//   It also says nothing about the harmonic-restoration stage (T90's stage 2).
//   That one needs voiced speech and is a separate harness.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "sb_tune.h"

// Mirrors sb_tune.c's own cap; a capture wider than this is refused rather
// than silently truncated to a window that means something different.
#define HARNESS_MAX_BINS 128

// Matches main/dsp/dsp.h - a mismatch here would silently measure a different
// radio than the one we ship.
#define FFT_N        1024
#define SAMPLE_RATE  48000.0
#define BIN_HZ       (SAMPLE_RATE / (double)FFT_N)

#define SEARCH_HZ    2000.0
#define SPAN_BINS    ((int)(SEARCH_HZ / BIN_HZ + 0.5))
#define WIN_N        (2 * SPAN_BINS + 1)

/* ONE PLACE EVERY GATE TAKES ITS CONFIG FROM, so a sweep cannot grade one
 * estimator against a config another gate did not use.
 *
 * SB_TUNE_MIN_STEP / SB_TUNE_MIN_SNR override the shipped defaults for a run.
 * They exist so the acceptance gate can be SWEPT and read off a table rather
 * than nudged by eye - sb_tune.h says exactly that about edge_drop_db and the
 * same discipline belongs on the gate. Unset, this is SB_TUNE_CFG_DEFAULT
 * byte for byte, so a plain run grades what ships. */
static sb_tune_cfg_t harness_cfg(void)
{
    sb_tune_cfg_t c = SB_TUNE_CFG_DEFAULT;
    const char *e;
    if ((e = getenv("SB_TUNE_MIN_STEP")) && *e) c.min_step_db = (float)atof(e);
    if ((e = getenv("SB_TUNE_MIN_SNR"))  && *e) c.min_snr_db  = (float)atof(e);
    return c;
}

// ---------------------------------------------------------------------------
// A small complex FFT. The device uses esp-dsp, which is not on the host; this
// is a plain radix-2 and only has to agree with it to within the window's own
// smearing, which is the quantity under test.

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                double w = ang * (double)k;
                double wr = cos(w), wi = sin(w);
                double ur = re[i + k],           ui = im[i + k];
                double vr = re[i + k + len / 2], vi = im[i + k + len / 2];
                double tr = vr * wr - vi * wi;
                double ti = vr * wi + vi * wr;
                re[i + k] = ur + tr;  im[i + k] = ui + ti;
                re[i + k + len / 2] = ur - tr;
                im[i + k + len / 2] = ui - ti;
            }
        }
    }
}

// esp-dsp's dsps_wind_blackman_harris_f32 / dsps_wind_hann_f32 coefficients.
typedef enum { WIN_BH4 = 0, WIN_HANN = 1 } wintype_t;

static void make_window(double *w, int n, wintype_t t)
{
    for (int i = 0; i < n; i++) {
        double x = 2.0 * M_PI * (double)i / (double)(n - 1);
        if (t == WIN_HANN) {
            w[i] = 0.5 - 0.5 * cos(x);
        } else {
            w[i] = 0.35875 - 0.48829 * cos(x)
                 + 0.14128 * cos(2.0 * x) - 0.01168 * cos(3.0 * x);
        }
    }
}

// ---------------------------------------------------------------------------
// Deterministic noise, so a run is reproducible and a regression is real.

static uint64_t s_rng = 1;
static double urand(void)
{
    s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((s_rng >> 11) & 0xFFFFFF) / (double)0x1000000;
}
static double gauss(void)
{
    double u1 = urand(), u2 = urand();
    if (u1 < 1e-12) u1 = 1e-12;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

// ---------------------------------------------------------------------------
// One synthetic SSB capture.
//
// The dial sits at 0 in baseband. A USB station whose suppressed carrier is
// carrier_off_hz away puts its audio component at frequency f at baseband
// carrier_off_hz + f. Their transmit filter passes [low_cut, high_cut], and
// that filter IS what makes the edge - which is exactly why its value is swept.

typedef struct {
    double carrier_off_hz; // true carrier, relative to the dial
    double low_cut_hz;     // the far station's transmit filter
    double high_cut_hz;
    double f0_hz;          // voice pitch
    double snr_db;
    bool   upper;          // USB
    wintype_t win;
} scene_t;

static void synth_window(const scene_t *s, float *win_db)
{
    static double re[FFT_N], im[FFT_N], w[FFT_N];
    make_window(w, FFT_N, s->win);

    const double sig = pow(10.0, s->snr_db / 20.0);

    for (int i = 0; i < FFT_N; i++) { re[i] = 0.0; im[i] = 0.0; }

    // Glottal harmonic stack with a crude two-formant envelope. Amplitudes
    // fall as 1/k, which is the usual first approximation for voiced speech.
    for (int k = 1; k < 200; k++) {
        double f_audio = (double)k * s->f0_hz;
        if (f_audio < s->low_cut_hz || f_audio > s->high_cut_hz) continue;

        double env = 1.0 / (double)k;
        env *= 1.0 + 1.6 * exp(-pow((f_audio -  700.0) / 220.0, 2.0));
        env *= 1.0 + 1.1 * exp(-pow((f_audio - 1500.0) / 320.0, 2.0));

        // A touch of roll-off at the filter's own corners, because no real
        // filter is a brick wall and a brick wall would flatter the estimator.
        double tap = 1.0;
        double from_low = f_audio - s->low_cut_hz;
        if (from_low < 100.0) tap *= from_low / 100.0;
        double to_high = s->high_cut_hz - f_audio;
        if (to_high < 200.0) tap *= to_high / 200.0;

        double amp = sig * env * tap;
        double bb  = s->upper ? (s->carrier_off_hz + f_audio)
                              : (s->carrier_off_hz - f_audio);
        double ph  = 2.0 * M_PI * urand();
        for (int i = 0; i < FFT_N; i++) {
            double t = (double)i / SAMPLE_RATE;
            double a = 2.0 * M_PI * bb * t + ph;
            re[i] += amp * cos(a);
            im[i] += amp * sin(a);
        }
    }

    for (int i = 0; i < FFT_N; i++) {
        re[i] = (re[i] + gauss()) * w[i];
        im[i] = (im[i] + gauss()) * w[i];
    }

    fft(re, im, FFT_N);

    // Bin 0 is the dial. Negative baseband frequencies live in the top half,
    // so the window is gathered with a wrap - the same arithmetic ui.c's
    // cw_tune_attempt() does with ui_get_if_bin_shift().
    for (int i = 0; i < WIN_N; i++) {
        int b = ((i - SPAN_BINS) % FFT_N + FFT_N) % FFT_N;
        double p = re[b] * re[b] + im[b] * im[b];
        win_db[i] = (float)(10.0 * log10(p + 1e-20));
    }
}

// ---------------------------------------------------------------------------

/* The ACHIEVED per-bin SNR of a synthesised window, computed exactly the way
 * sb_tune.c computes it.
 *
 * ⛔ WHY THIS EXISTS. scene_t.snr_db is a TIME-DOMAIN amplitude knob, and it is
 * not the SNR the estimator sees. A 1024-point FFT concentrates each tone into
 * about one bin while the noise spreads across all of them, so the per-bin SNR
 * runs roughly 10*log10(1024) = 30 dB above the knob. Grading a gate against
 * the knob therefore graded it against the wrong axis, and the first "fails
 * below 25 dB" reading from this harness was quoting knob dB as if they were
 * signal-to-noise dB. They were not. Rows are labelled with THIS instead. */
static double achieved_snr_db(const float *win_db, int n)
{
    float v[WIN_N];
    memcpy(v, win_db, (size_t)n * sizeof(float));
    for (int i = 1; i < n; i++) {
        float k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
    return (double)v[n - 1 - 2] - (double)v[n / 10];
}

typedef struct {
    int    n;
    double sum, sumsq, min, max;
} stat_t;

static void stat_add(stat_t *s, double v)
{
    if (s->n == 0) { s->min = v; s->max = v; }
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    s->sum += v; s->sumsq += v * v; s->n++;
}
static double stat_mean(const stat_t *s) { return s->n ? s->sum / s->n : 0.0; }
static double stat_sd(const stat_t *s)
{
    if (s->n < 2) return 0.0;
    double m = stat_mean(s);
    double v = s->sumsq / s->n - m * m;
    return v > 0.0 ? sqrt(v) : 0.0;
}

static const double LOW_CUTS[] = { 100.0, 200.0, 300.0, 400.0 };
static const double DIAL_ERRS[] = { -400, -300, -200, -100, -50, 0,
                                     50, 100, 200, 300, 400 };
static const double F0S[]  = { 100.0, 125.0, 200.0 };
static const double SNRS[] = { 10.0, 20.0, 30.0 };

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

// Sweep everything for one window type and one edge_drop_db. inject_hz is the
// mutation hook: a non-zero value corrupts the estimate and the harness must
// notice.
/* Amplitude knobs that clear the Gate 1 SNR gate. Chosen from Gate 1's own
 * bucket table rather than guessed: the knob is not an SNR (see
 * achieved_snr_db), so "a usable signal" has to be identified by measurement. */
static const double STRONG_KNOBS[] = { 0.0, 5.0, 10.0, 15.0 };

static void sweep(wintype_t win, float edge_drop_db, double inject_hz,
                  stat_t *bias, int *refused, int *snap_hits, int *snap_tot,
                  unsigned grid_hz, const double *knobs, int nknobs,
                  bool upper)
{
    sb_tune_cfg_t cfg = harness_cfg();
    cfg.edge_drop_db = edge_drop_db;

    for (int a = 0; a < NELEM(LOW_CUTS); a++)
    for (int b = 0; b < NELEM(DIAL_ERRS); b++)
    for (int c = 0; c < NELEM(F0S); c++)
    for (int d = 0; d < nknobs; d++) {
        scene_t s = {
            .carrier_off_hz = DIAL_ERRS[b],
            .low_cut_hz     = LOW_CUTS[a],
            .high_cut_hz    = 2700.0,
            .f0_hz          = F0S[c],
            .snr_db         = knobs[d],
            .upper          = upper,
            .win            = win,
        };
        float wdb[WIN_N];
        s_rng = 1 + (uint64_t)(a * 1000 + b * 100 + c * 10 + d);
        synth_window(&s, wdb);

        float edge = 0.0f, snr = 0.0f;
        sb_tune_result_t r = sb_tune_find_edge_fit(wdb, WIN_N, SPAN_BINS,
                                               (float)BIN_HZ, s.upper,
                                               &cfg, &edge, &snr);
        if (r != SB_TUNE_OK) { (*refused)++; continue; }

        edge += (float)inject_hz;

        // The estimator reports the edge. The error that matters is how far
        // that is from the TRUE carrier.
        stat_add(bias, (double)edge - s.carrier_off_hz);

        /* THE DECIDING NUMBER.
         *
         * ⛔ THE FIRST VERSION OF THIS TEST WAS WRONG IN TWO WAYS and its
         * numbers were discarded. It built `theirs` by SNAPPING the station's
         * own carrier, which fakes the premise instead of testing it, and it
         * put the base dial on a 1 kHz multiple with every error inside half a
         * step - so at grid 1000 the station and the estimate both collapsed
         * onto one grid point and it reported 96 %. It also made the mutation
         * check impossible: with bucket equality, a whole-step injected error
         * merely relabels which cases hit, so the hit rate did not move.
         *
         * Built properly: the station sits ON the grid by construction, the
         * operator's dial is off by the swept amount, and success is measured
         * as landing within half a step of the station - a tolerance, which a
         * one-step error cannot survive.
         *
         * The calibration is FIXED at the common 300 Hz low cut, because on
         * air we do not know the far station's filter. Removing the sweep's
         * own mean bias instead would be grading the estimator on an answer
         * it cannot have. */
        /* The calibration's SIGN follows the sideband. On USB the occupied
         * band sits above the carrier so the edge reads high and 300 Hz comes
         * off; on LSB it sits below, the edge reads negative, and the same
         * 300 Hz goes back on. Getting this sign wrong would show up as an
         * LSB bias of about 600 Hz rather than 0, which is why Gate 3 checks
         * the magnitudes against USB and not just the hit rate. */
        const double assumed_low_cut = s.upper ? 300.0 : -300.0;
        double their_carrier = 14200000.0 + 3.0 * (double)grid_hz; // on the grid
        double dial_now      = their_carrier - s.carrier_off_hz;
        double est_carrier   = dial_now + edge - assumed_low_cut;
        unsigned snapped = sb_tune_snap((unsigned)(est_carrier + 0.5), grid_hz);
        if (fabs((double)snapped - their_carrier) < 1.0) (*snap_hits)++;
        (*snap_tot)++;
    }
}

/* ===================== GATE 4 =====================
 * Real signals, captured off the bench QMX on 40 m LSB with tools in
 * scratchpad/ws_capture.py. Path from SB_TUNE_REAL, since the capture is a
 * 700 KB data file and does not belong in the repo.
 *
 * WHAT THIS CAN AND CANNOT SHOW. The operator knows his dial (7.080500) but
 * nobody knows the far station's true carrier, so there is NO ground truth
 * here and no absolute bias is claimed. What synthetic voice could not give,
 * and this can:
 *   - REPEATABILITY. Does the estimate hold still across real speech, with
 *     its pauses, level swings and the plosives a harmonic stack has none of?
 *     A tuning aid that wanders between syllables is unusable however good
 *     its mean is.
 *   - REFUSAL ON REAL NOISE. Does the Gate 1 gate actually decline during the
 *     gaps between overs, or does it invent an edge out of band noise?
 *   - SURVIVAL IN A CONTEST. Adjacent stations a few hundred Hz away are
 *     exactly what the synthetic sweep never contained.
 *
 * ⛔ LSB, so the carrier-side edge is the HIGH edge of the occupied band. */
static void gate4(void)
{
    const char *path = getenv("SB_TUNE_REAL");
    if (!path) { printf("  SB_TUNE_REAL not set - gate 4 not run\n"); return; }
    FILE *f = fopen(path, "r");
    if (!f) { printf("  cannot open %s - gate 4 not run\n", path); return; }

    static char line[16384];
    double bin_hz = 0.0;
    int span_bins = 0, win_n = 0;
    long dial = 0;
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "# dial=%ld mode=%*s bin_hz=%lf span_bins=%d win_n=%d",
               &dial, &bin_hz, &span_bins, &win_n) != 4) {
        printf("  capture header not understood - gate 4 not run\n");
        fclose(f);
        return;
    }
    if (win_n > HARNESS_MAX_BINS) {
        printf("  capture window %d bins exceeds %d - refused\n",
               win_n, HARNESS_MAX_BINS);
        fclose(f);
        return;
    }
    printf("  dial %ld, %.4f Hz/bin, window %d bins (+/-%.0f Hz)\n",
           dial, bin_hz, win_n, span_bins * bin_hz);

    sb_tune_cfg_t cfg = harness_cfg();
    stat_t edges = {0}, snrs = {0};
    int frames = 0, ok = 0;

    while (fgets(line, sizeof(line), f)) {
        float w[HARNESS_MAX_BINS];
        char *p = strchr(line, ',');          /* past dial */
        if (!p) continue;
        p = strchr(p + 1, ',');               /* past sig */
        if (!p) continue;
        int got = 0;
        for (char *tok = strtok(p + 1, ",\n");
             tok && got < win_n;
             tok = strtok(NULL, ",\n")) {
            w[got++] = (float)atof(tok);
        }
        if (got != win_n) continue;
        frames++;
        float edge = 0.0f, snr = 0.0f;
        if (sb_tune_find_edge_fit(w, win_n, span_bins, (float)bin_hz, false,
                              &cfg, &edge, &snr) == SB_TUNE_OK) {
            ok++;
            stat_add(&edges, (double)edge);
            stat_add(&snrs, (double)snr);
        }
    }
    fclose(f);

    printf("  frames %d | accepted %d (%.1f %%) | refused %d\n",
           frames, ok, frames ? 100.0 * ok / frames : 0.0, frames - ok);
    if (ok) {
        printf("  accepted SNR : mean %.1f dB\n", stat_mean(&snrs));
        printf("  edge offset  : mean %+.1f Hz, sd %.1f Hz, min %+.1f, max %+.1f\n",
               stat_mean(&edges), stat_sd(&edges), edges.min, edges.max);
        printf("  the sd IS the repeatability on one real station.\n");
        printf("  no absolute bias is claimed - the far carrier is unknown.\n");
    }
}

int main(void)
{
    printf("SSB edge estimator - bias sweep (T90)\n");
    printf("FFT %d @ %.0f Hz -> %.3f Hz/bin, window %d bins (+/- %.0f Hz)\n\n",
           FFT_N, SAMPLE_RATE, BIN_HZ, WIN_N, SEARCH_HZ);

    const float drops[] = { 3.0f, 6.0f, 9.0f, 12.0f };
    const char *wname[] = { "Blackman-Harris", "Hann" };
    int fails = 0;

    /* ===================== GATE 2 =====================
     * Does edge + grid snap put the dial on the other operator's frequency?
     *
     * Criterion fixed before running: >= 90 % EXACT at a 500 Hz grid, counting
     * only cases that pass Gate 1's refusal. Anything the estimator declined
     * is not a wrong answer - it is no answer, and the operator's dial does
     * not move - so it is excluded from the hit rate and reported separately.
     *
     * The station is ON the grid by construction and the calibration is a
     * fixed 300 Hz, because on air the far station's filter is unknowable.
     * See the long note at the hit test for the two ways this was built wrong
     * the first time. */
    printf("=== GATE 2 (USB) / GATE 3 (LSB): does the snap land on their frequency?\n");
    for (int sb = 0; sb < 2; sb++) {
      const bool upper = (sb == 0);
      for (int w = 0; w <= 1; w++) {
        printf("  %s, %s\n", upper ? "USB" : "LSB", wname[w]);
        printf("    drop | refused | bias mean |     sd | snap@500 | snap@1k | verdict\n");
        for (int i = 0; i < NELEM(drops); i++) {
            stat_t st = {0};
            int refused = 0, h5 = 0, t5 = 0, h1 = 0, t1 = 0, r2 = 0;
            stat_t st2 = {0};
            sweep((wintype_t)w, drops[i], 0.0, &st,  &refused, &h5, &t5, 500,
                  STRONG_KNOBS, NELEM(STRONG_KNOBS), upper);
            sweep((wintype_t)w, drops[i], 0.0, &st2, &r2,      &h1, &t1, 1000,
                  STRONG_KNOBS, NELEM(STRONG_KNOBS), upper);
            double p5 = t5 ? 100.0 * h5 / t5 : 0.0;
            printf("    %4.0f | %7d | %7.1f Hz | %6.1f | %5.1f %% | %5.1f %% | %s\n",
                   (double)drops[i], refused, stat_mean(&st), stat_sd(&st),
                   p5, t1 ? 100.0 * h1 / t1 : 0.0,
                   p5 >= 90.0 ? "PASS" : "fail");
            if (p5 < 90.0) fails++;
        }
        printf("\n");
      }
    }

    /* GATE 3's REAL TEST. A hit rate alone would not catch a sideband bug: get
     * the scan direction or the calibration sign wrong and the hit rate
     * collapses, but so would a dozen other faults. The discriminating check
     * is that the edge sits the SAME DISTANCE from the carrier on both
     * sidebands, just mirrored - so |LSB| must equal |USB| per filter. */
    printf("=== GATE 3: LSB edge magnitudes must mirror USB (Hann, 6 dB)\n");
    printf("    low cut |   USB edge |   LSB edge | |diff| | verdict\n");
    for (int a = 0; a < NELEM(LOW_CUTS); a++) {
        double mag[2] = { 0.0, 0.0 };
        for (int sb = 0; sb < 2; sb++) {
            stat_t one = {0};
            sb_tune_cfg_t cfg = harness_cfg();
            cfg.edge_drop_db = 6.0f;
            for (int b = 0; b < NELEM(DIAL_ERRS); b++)
            for (int c = 0; c < NELEM(F0S); c++)
            for (int d = 0; d < NELEM(STRONG_KNOBS); d++) {
                scene_t s = { .carrier_off_hz = DIAL_ERRS[b],
                              .low_cut_hz = LOW_CUTS[a], .high_cut_hz = 2700.0,
                              .f0_hz = F0S[c], .snr_db = STRONG_KNOBS[d],
                              .upper = (sb == 0), .win = WIN_HANN };
                float wdb[WIN_N], edge = 0.0f;
                s_rng = 1 + (uint64_t)(a * 1000 + b * 100 + c * 10 + d);
                synth_window(&s, wdb);
                if (sb_tune_find_edge_fit(wdb, WIN_N, SPAN_BINS, (float)BIN_HZ,
                                      s.upper, &cfg, &edge, NULL) != SB_TUNE_OK)
                    continue;
                stat_add(&one, (double)edge - s.carrier_off_hz);
            }
            mag[sb] = stat_mean(&one);
        }
        double diff = fabs(fabs(mag[0]) - fabs(mag[1]));
        const char *v = (diff <= 20.0) ? "PASS" : "FAIL";
        if (diff > 20.0) fails++;
        printf("    %4.0f Hz | %+8.1f Hz | %+8.1f Hz | %5.1f | %s\n",
               LOW_CUTS[a], mag[0], mag[1], diff, v);
    }
    printf("\n");

    /* HOW FAR OFF CAN THE OPERATOR BE? An operational limit, not a pass/fail:
     * the snap can only rescue an estimate that is already inside half a step,
     * so this says what "roughly tuned" is allowed to mean. */
    printf("=== GATE 2b: tolerance to the operator's own mistuning (Hann, 6 dB)\n");
    printf("    dial off | snap@500 | snap@1k\n");
    for (int b = 0; b < NELEM(DIAL_ERRS); b++) {
        sb_tune_cfg_t cfg = harness_cfg();
        cfg.edge_drop_db = 6.0f;
        int h5 = 0, t5 = 0, h1 = 0, t1 = 0;
        for (int g = 0; g < 2; g++) {
            unsigned grid = g ? 1000 : 500;
            for (int a = 0; a < NELEM(LOW_CUTS); a++)
            for (int c = 0; c < NELEM(F0S); c++)
            for (int d = 0; d < NELEM(STRONG_KNOBS); d++) {
                scene_t s = { .carrier_off_hz = DIAL_ERRS[b],
                              .low_cut_hz = LOW_CUTS[a], .high_cut_hz = 2700.0,
                              .f0_hz = F0S[c], .snr_db = STRONG_KNOBS[d],
                              .upper = true, .win = WIN_HANN };
                float wdb[WIN_N], edge = 0.0f;
                s_rng = 1 + (uint64_t)(a * 1000 + b * 100 + c * 10 + d);
                synth_window(&s, wdb);
                if (sb_tune_find_edge_fit(wdb, WIN_N, SPAN_BINS, (float)BIN_HZ,
                                      s.upper, &cfg, &edge, NULL) != SB_TUNE_OK)
                    continue;
                double theirs = 14200000.0 + 3.0 * (double)grid;
                double dial   = theirs - s.carrier_off_hz;
                unsigned snapped = sb_tune_snap(
                    (unsigned)(dial + edge - 300.0 + 0.5), grid);
                if (g) { t1++; if (fabs((double)snapped - theirs) < 1.0) h1++; }
                else   { t5++; if (fabs((double)snapped - theirs) < 1.0) h5++; }
            }
        }
        printf("    %+6.0f Hz | %5.1f %% | %5.1f %%\n", DIAL_ERRS[b],
               t5 ? 100.0 * h5 / t5 : 0.0, t1 ? 100.0 * h1 / t1 : 0.0);
    }
    printf("\n");

    /* ===================== GATE 1 =====================
     * Does it REFUSE when it cannot tell?
     *
     * Criterion fixed before running: >=95 % refused at 10 and 20 dB SNR,
     * >=95 % accepted at 30 dB with sd <= 60 Hz. The point is not accuracy -
     * it is that a weak signal produces no answer at all, because the previous
     * version produced a confident wrong one and never refused. */
    /* CALIBRATION, NOT GOALPOST-MOVING. The fit's confidence is the height of
     * the fitted STEP - a difference of means in dB - and that is a different
     * quantity from the threshold version's peak-to-floor percentile gap. It
     * is necessarily smaller: between voice harmonics the spectrum sits at the
     * noise floor, so the signal-side mean is pulled well below the peaks.
     * Reusing min_snr_db = 25 therefore refused every case at every SNR.
     *
     * Gate 1's criterion is unchanged and is about the SIGNAL (refuse below
     * 20 dB achieved SNR, accept above 30). What has to be chosen is the step
     * threshold that DELIVERS that behaviour, so it is measured here first. */
    printf("=== step-height calibration (what the fit actually returns)\n");
    printf("  achieved SNR | cases | step mean | step p10 | step p90\n");
    {
        const double KNOBS[] = { -40, -30, -25, -20, -15, -10, -5, 0, 10 };
        const double EDGES[] = { 0.0, 10.0, 20.0, 30.0, 40.0, 1e9 };
        enum { NB = NELEM(EDGES) - 1 };
        static double steps[NB][4096];
        int cnt[NB];
        memset(cnt, 0, sizeof(cnt));
        sb_tune_cfg_t cfg = harness_cfg();
        cfg.min_step_db = -1000.0f;     /* accept everything, just to observe */
        for (int k = 0; k < NELEM(KNOBS); k++)
        for (int a = 0; a < NELEM(LOW_CUTS); a++)
        for (int b = 0; b < NELEM(DIAL_ERRS); b++)
        for (int c = 0; c < NELEM(F0S); c++) {
            scene_t s = { .carrier_off_hz = DIAL_ERRS[b],
                          .low_cut_hz = LOW_CUTS[a], .high_cut_hz = 2700.0,
                          .f0_hz = F0S[c], .snr_db = KNOBS[k],
                          .upper = true, .win = WIN_HANN };
            float wdb[WIN_N], edge = 0.0f, st = 0.0f;
            s_rng = 1 + (uint64_t)(k * 10000 + a * 1000 + b * 100 + c);
            synth_window(&s, wdb);
            double a_snr = achieved_snr_db(wdb, WIN_N);
            int bk = 0;
            while (bk < NB - 1 && a_snr >= EDGES[bk + 1]) bk++;
            if (sb_tune_find_edge_fit(wdb, WIN_N, SPAN_BINS, (float)BIN_HZ,
                                      s.upper, &cfg, &edge, &st) == SB_TUNE_OK
                && cnt[bk] < 4096) {
                steps[bk][cnt[bk]++] = (double)st;
            }
        }
        for (int bk = 0; bk < NB; bk++) {
            if (!cnt[bk]) continue;
            for (int i = 1; i < cnt[bk]; i++) {
                double key = steps[bk][i]; int j = i - 1;
                while (j >= 0 && steps[bk][j] > key) { steps[bk][j+1] = steps[bk][j]; j--; }
                steps[bk][j+1] = key;
            }
            double sum = 0.0;
            for (int i = 0; i < cnt[bk]; i++) sum += steps[bk][i];
            char lbl[24];
            if (EDGES[bk + 1] > 1e8) snprintf(lbl, sizeof(lbl), "   >= %2.0f dB", EDGES[bk]);
            else snprintf(lbl, sizeof(lbl), "  %2.0f - %2.0f dB", EDGES[bk], EDGES[bk+1]);
            printf("  %-12s | %5d | %8.1f  | %7.1f  | %7.1f\n",
                   lbl, cnt[bk], sum / cnt[bk],
                   steps[bk][cnt[bk] / 10], steps[bk][cnt[bk] * 9 / 10]);
        }
    }
    printf("\n");

    printf("=== GATE 1: refuse when the signal is too weak\n");
    for (int sb = 0; sb < 2; sb++) {
    const bool g_upper = (sb == 0);
    printf("  %s\n", g_upper ? "USB" : "LSB");
    printf("  achieved SNR | cases | accepted | edge sd | verdict\n");
    {
        /* Bucket by ACHIEVED per-bin SNR, not by the amplitude knob. The knob
         * is swept wide enough to fill every bucket. */
        const double KNOBS[] = { -40, -35, -30, -25, -20, -15, -10, -5, 0, 10 };
        const double EDGES[] = { 0.0, 10.0, 20.0, 30.0, 40.0, 1e9 };
        enum { NB = NELEM(EDGES) - 1 };
        stat_t st[NB]; int acc[NB], tot[NB];
        memset(st, 0, sizeof(st));
        memset(acc, 0, sizeof(acc));
        memset(tot, 0, sizeof(tot));

        sb_tune_cfg_t cfg = harness_cfg();
        for (int k = 0; k < NELEM(KNOBS); k++)
        for (int a = 0; a < NELEM(LOW_CUTS); a++)
        for (int b = 0; b < NELEM(DIAL_ERRS); b++)
        for (int c = 0; c < NELEM(F0S); c++) {
            scene_t s = { .carrier_off_hz = DIAL_ERRS[b],
                          .low_cut_hz = LOW_CUTS[a], .high_cut_hz = 2700.0,
                          .f0_hz = F0S[c], .snr_db = KNOBS[k],
                          .upper = g_upper, .win = WIN_HANN };
            float wdb[WIN_N], edge = 0.0f;
            s_rng = 1 + (uint64_t)(k * 10000 + a * 1000 + b * 100 + c);
            synth_window(&s, wdb);
            double a_snr = achieved_snr_db(wdb, WIN_N);
            int bk = 0;
            while (bk < NB - 1 && a_snr >= EDGES[bk + 1]) bk++;
            tot[bk]++;
            if (sb_tune_find_edge_fit(wdb, WIN_N, SPAN_BINS, (float)BIN_HZ,
                                  s.upper, &cfg, &edge, NULL) == SB_TUNE_OK) {
                acc[bk]++;
                stat_add(&st[bk], (double)edge - s.carrier_off_hz);
            }
        }
        for (int bk = 0; bk < NB; bk++) {
            if (!tot[bk]) continue;
            double pa = 100.0 * acc[bk] / tot[bk];
            const char *v;
            /* ⛔ GRADED ON REFUSAL ONLY, and the first version was not.
             * It also demanded sd <= 60 Hz here, which was the wrong test in
             * the wrong place: that sd pools all four transmit filters, so it
             * includes the far station's unknown low cut - a spread the
             * estimator cannot remove and is not supposed to. Gate 1 asks one
             * question, "does it refuse when it cannot tell". Whether the
             * residual error is small ENOUGH is Gate 2's question, and Gate 2
             * answers it the only way that matters: by how often the snapped
             * dial lands on the other operator's frequency. */
            if (EDGES[bk + 1] <= 20.0)   v = (pa <= 5.0)  ? "PASS" : "FAIL";
            else if (EDGES[bk] >= 30.0)  v = (pa >= 95.0) ? "PASS" : "FAIL";
            else                         v = "(gate edge, not graded)";
            if (v[0] == 'F') fails++;
            char lbl[24];
            if (EDGES[bk + 1] > 1e8) snprintf(lbl, sizeof(lbl), "   >= %2.0f dB", EDGES[bk]);
            else snprintf(lbl, sizeof(lbl), "  %2.0f - %2.0f dB", EDGES[bk], EDGES[bk + 1]);
            printf("  %-12s | %5d | %5.1f %%  | %6.1f  | %s\n",
                   lbl, tot[bk], pa, stat_sd(&st[bk]), v);
        }
    }
    printf("\n");
    }   /* sideband */

    /* WHERE THE SPREAD COMES FROM - the question that decides whether stage 1
     * is fixable. If the error is set by the far station's FILTER it is one
     * unknown constant and a calibration helps on average. If it is set by the
     * TALKER'S PITCH it cannot be calibrated away at all, because the lowest
     * harmonic actually present jumps in steps of f0: at f0 200 Hz behind a
     * 300 Hz cut, the first component sits at 400 Hz, not 300. T90's row
     * blames only the filter, so this is worth knowing before building. */
    printf("=== what drives the spread (Hann, drop 6 dB)\n");
    for (int a = 0; a < NELEM(LOW_CUTS); a++) {
        stat_t st = {0};
        sb_tune_cfg_t cfg = harness_cfg();
        cfg.edge_drop_db = 6.0f;
        for (int d = 0; d < NELEM(SNRS); d++) {
            stat_t one = {0};
            for (int b = 0; b < NELEM(DIAL_ERRS); b++)
            for (int c = 0; c < NELEM(F0S); c++) {
                scene_t s = { .carrier_off_hz = DIAL_ERRS[b],
                              .low_cut_hz = LOW_CUTS[a], .high_cut_hz = 2700.0,
                              .f0_hz = F0S[c], .snr_db = SNRS[d],
                              .upper = true, .win = WIN_HANN };
                float wdb[WIN_N], edge = 0.0f;
                s_rng = 1 + (uint64_t)(a * 1000 + b * 100 + c * 10 + d);
                synth_window(&s, wdb);
                if (sb_tune_find_edge_fit(wdb, WIN_N, SPAN_BINS, (float)BIN_HZ,
                                      s.upper, &cfg, &edge, NULL) != SB_TUNE_OK)
                    continue;
                stat_add(&one, (double)edge - s.carrier_off_hz);
                stat_add(&st,  (double)edge - s.carrier_off_hz);
            }
            printf("  low cut %3.0f Hz, SNR %2.0f dB : edge %+7.1f Hz (sd %5.1f)\n",
                   LOW_CUTS[a], SNRS[d], stat_mean(&one), stat_sd(&one));
        }
        printf("  low cut %3.0f Hz, all SNRs  : edge %+7.1f Hz (sd %5.1f)\n\n",
               LOW_CUTS[a], stat_mean(&st), stat_sd(&st));
    }

    /* MUTATION CHECK. A harness that cannot fail proves nothing, and this
     * project has been bitten by exactly that before. Inject a whole grid step
     * of error: landing within half a step of the station is then impossible,
     * so the hit rate must collapse. The FIRST version of this check used
     * bucket equality and could not detect the injection at all - it reported
     * the mutated run as BETTER. That is why the test above is a tolerance. */
    printf("=== mutation check (estimator deliberately +500 Hz wrong)\n");
    {
        stat_t clean = {0}, bad = {0};
        int rc = 0, rb = 0, hc = 0, tc = 0, hb = 0, tb = 0;
        sweep(WIN_HANN, 6.0f, 0.0,   &clean, &rc, &hc, &tc, 500,
              STRONG_KNOBS, NELEM(STRONG_KNOBS), true);
        sweep(WIN_HANN, 6.0f, 500.0, &bad,   &rb, &hb, &tb, 500,
              STRONG_KNOBS, NELEM(STRONG_KNOBS), true);
        double pc = tc ? 100.0 * hc / tc : 0.0;
        double pb = tb ? 100.0 * hb / tb : 0.0;
        printf("  clean  bias mean %.1f Hz, snap@500 %.1f %% (%d/%d)\n",
               stat_mean(&clean), pc, hc, tc);
        printf("  mutated bias mean %.1f Hz, snap@500 %.1f %% (%d/%d)\n",
               stat_mean(&bad), pb, hb, tb);
        if (pb >= pc) {
            printf("  FAIL: a 500 Hz error did not reduce the hit rate - "
                   "the harness cannot tell right from wrong\n");
            fails++;
        } else {
            printf("  ok: the injected error is detected\n");
        }
    }

    printf("=== GATE 4: real signals from the bench QMX (40 m LSB)\n");
    gate4();
    printf("\n");

    printf("\n%s\n", fails ? "HARNESS FAILED" : "harness self-check passed");
    return fails ? 1 : 0;
}
