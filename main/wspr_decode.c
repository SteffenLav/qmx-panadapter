#include "wspr_decode.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kiss_fftr.h"

#define TONE_SPACING (WSPR_SAMPLE_RATE_HZ / WSPR_SYM_LEN_SAMPLES) /* 1.46484375 Hz */

int wspr_find_candidates(const int16_t *samples, long n, double f_lo_hz,
                          double f_hi_hz, wspr_freq_candidate_t *out,
                          int max_out)
{
    if (n <= 0 || max_out <= 0) return 0;
    int nfft = (int)n;
    kiss_fftr_cfg cfg = kiss_fftr_alloc(nfft, 0, NULL, NULL);
    if (!cfg) return 0;
    kiss_fft_scalar *in = (kiss_fft_scalar *)malloc((size_t)nfft * sizeof(kiss_fft_scalar));
    kiss_fft_cpx *spec = (kiss_fft_cpx *)malloc((size_t)(nfft / 2 + 1) * sizeof(kiss_fft_cpx));
    if (!in || !spec) {
        free(in); free(spec); free(cfg);
        return 0;
    }
    for (int i = 0; i < nfft; i++) in[i] = (kiss_fft_scalar)(samples[i] / 32768.0);
    kiss_fftr(cfg, in, spec);

    double bin_hz = WSPR_SAMPLE_RATE_HZ / nfft;
    int lo_bin = (int)(f_lo_hz / bin_hz), hi_bin = (int)(f_hi_hz / bin_hz);
    int nbins = nfft / 2 + 1;
    if (hi_bin > nbins) hi_bin = nbins;
    if (lo_bin < 0) lo_bin = 0;
    int tone_step_bins = (int)(TONE_SPACING / bin_hz + 0.5);

    float *mag = (float *)malloc((size_t)nbins * sizeof(float));
    for (int b = 0; b < nbins; b++) {
        float re = spec[b].r, im = spec[b].i;
        mag[b] = re * re + im * im;
    }

    int nscore = hi_bin - lo_bin;
    int count = 0;
    if (nscore > 0) {
        float *score = (float *)calloc((size_t)nscore, sizeof(float));
        for (int b = lo_bin; b < hi_bin; b++) {
            double s = 0;
            for (int k = 0; k < 4; k++) {
                int bb = b + k * tone_step_bins;
                if (bb < nbins) s += mag[bb];
            }
            score[b - lo_bin] = (float)s;
        }
        for (int c = 0; c < max_out; c++) {
            int best = -1;
            float bestv = -1;
            for (int i = 0; i < nscore; i++) {
                if (score[i] > bestv) { bestv = score[i]; best = i; }
            }
            if (best < 0 || bestv <= 0) break;
            out[count].freq_hz = (best + lo_bin) * bin_hz;
            out[count].comb_score = bestv;
            count++;
            int supp = tone_step_bins * 4;
            for (int i = best - supp; i <= best + supp; i++) {
                if (i >= 0 && i < nscore) score[i] = -1;
            }
        }
        free(score);
    }

    free(mag);
    free(spec);
    free(in);
    free(cfg);
    return count;
}

/* Twiddle tables for the 4 tone frequencies at a given f0, indexed by
 * ABSOLUTE sample position - precomputed once so the start-time search
 * never calls cos()/sin() in its inner loop, only array lookups +
 * multiply-add. Without this an exhaustive search over the ~9 s of start
 * slack doesn't finish in reasonable time (verified: minutes, not the
 * ~2 s/candidate this version takes). */
typedef struct {
    float *cos_tab[4];
    float *sin_tab[4];
} twiddles_t;

static int build_twiddles(double f0, long n, twiddles_t *tw)
{
    for (int k = 0; k < 4; k++) {
        tw->cos_tab[k] = (float *)malloc((size_t)n * sizeof(float));
        tw->sin_tab[k] = (float *)malloc((size_t)n * sizeof(float));
        if (!tw->cos_tab[k] || !tw->sin_tab[k]) return 0;
        double w = 2.0 * M_PI * (f0 + k * TONE_SPACING) / WSPR_SAMPLE_RATE_HZ;
        for (long i = 0; i < n; i++) {
            tw->cos_tab[k][i] = (float)cos(w * i);
            tw->sin_tab[k][i] = (float)sin(w * i);
        }
    }
    return 1;
}

static void free_twiddles(twiddles_t *tw)
{
    for (int k = 0; k < 4; k++) { free(tw->cos_tab[k]); free(tw->sin_tab[k]); }
}

static void extract_tone_powers(const int16_t *samples, long n,
                                 const twiddles_t *tw, long start_sample,
                                 double tone_power[WSPR_NSYM][4])
{
    for (int sym = 0; sym < WSPR_NSYM; sym++) {
        long base = start_sample + (long)sym * WSPR_SYM_LEN_SAMPLES;
        long n0 = base < 0 ? 0 : base;
        long n1 = base + WSPR_SYM_LEN_SAMPLES > n ? n : base + WSPR_SYM_LEN_SAMPLES;
        for (int k = 0; k < 4; k++) {
            const float *ct = tw->cos_tab[k], *st = tw->sin_tab[k];
            float re = 0, im = 0;
            for (long idx = n0; idx < n1; idx++) {
                float x = samples[idx] / 32768.0f;
                re += x * ct[idx];
                im -= x * st[idx];
            }
            tone_power[sym][k] = (double)re * re + (double)im * im;
        }
    }
}

/* sync=1 tones are {1,3} (odd tone index), sync=0 tones are {0,2} - the
 * higher this is, the better the (f0, dt) alignment matches WSPR's known
 * 162-bit sync vector. */
static double sync_score(double tone_power[WSPR_NSYM][4])
{
    double s = 0;
    for (int i = 0; i < WSPR_NSYM; i++) {
        double p1 = tone_power[i][1] + tone_power[i][3];
        double p0 = tone_power[i][0] + tone_power[i][2];
        double favor1 = p1 - p0;
        s += wspr_sync_vector[i] ? favor1 : -favor1;
    }
    return s;
}

static int is_legal_power(int dbm)
{
    static const int legal[] = { 0, 3, 7, 10, 13, 17, 20, 23, 27, 30, 33, 37,
                                  40, 43, 47, 50, 53, 57, 60 };
    for (size_t i = 0; i < sizeof(legal) / sizeof(legal[0]); i++) {
        if (legal[i] == dbm) return 1;
    }
    return 0;
}

/* A clean decode converges fast — every genuine signal found in the
 * reference WAV (see wspr_decode.h) took 82-102 Fano cycles; the file's
 * own strongest candidate, which fails to decode plausibly (cause not
 * confirmed - frequency drift was tried and did NOT explain it, see
 * docs/wspr-phase1-status.md), took 49400. Picked with real margin: ~20x
 * the clean cluster's ceiling, far below any observed false-decode cycle
 * count. */
#define WSPR_CYCLES_SUSPECT 2000

/* Runs the three shared plausibility checks (message shape, legal power
 * quantization, Fano convergence speed - see wspr_decode.h) and fills
 * *result if a decoded message passes all three. Shared by both decode
 * attempts below so the two can't silently apply different standards. */
static int accept_if_plausible(const wspr_msg_bytes_t *msg, unsigned int cycles,
                                wspr_decode_result_t *result)
{
    result->cycles = cycles;
    char call[7], grid[5];
    int dbm;
    if (!wspr_unpack_message(msg, call, grid, &dbm)) return 0;
    if (strlen(call) < 3) return 0;
    if (!is_legal_power(dbm)) return 0;
    if (cycles > WSPR_CYCLES_SUSPECT) return 0;
    wspr_msg_bytes_t repack;
    if (!wspr_pack_message(call, grid, dbm, &repack)) return 0;

    strcpy(result->callsign, call);
    strcpy(result->grid, grid);
    result->power_dbm = dbm;
    result->ok = 1;
    return 1;
}

/* Hard-decision per symbol, conditioned on the known sync bit. Simple and
 * robust - no calibration dependency at all - and what decodes 5 of the
 * reference WAV's 8 candidates.
 *
 * A per-capture-normalized SOFT metric (wspr_build_soft_metric_table())
 * was tried in place of this and reverted - worth recording why, since
 * it's a real trap. In a synthetic single/multi-tone AWGN sweep
 * (test/wspr_metric_sim.c, test/wspr_synth_harness.c) it measured ~2-3 dB
 * more sensitive than this hard-decision table. Wired in here, it
 * REGRESSED real-world performance badly: 1/8 plausible decodes from the
 * reference WAV instead of 5/8. Lesson worth keeping: a synthetic
 * sensitivity sweep passing is evidence, not proof - the real WAV test is
 * what actually decides whether a decoder change is a genuine
 * improvement. Full account in docs/wspr-phase1-status.md. */
static int try_hard_decision(double tp[WSPR_NSYM][4], wspr_decode_result_t *result)
{
    uint8_t channel_bits[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double p_data1 = sync ? tp[i][3] : tp[i][2];
        double p_data0 = sync ? tp[i][1] : tp[i][0];
        channel_bits[i] = (p_data1 > p_data0) ? 1 : 0;
    }
    uint8_t raw[WSPR_NSYM];
    wspr_deinterleave(channel_bits, raw);
    uint8_t soft[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) soft[i] = raw[i] ? 255 : 0;

    int mettab[2][256];
    wspr_build_hard_metric_table(mettab);
    wspr_msg_bytes_t msg;
    unsigned int metric = 0, cycles = 0;
    if (!wspr_fano_decode(soft, mettab, 2, 20000, &msg, &metric, &cycles)) {
        result->cycles = cycles;
        return 0;
    }
    return accept_if_plausible(&msg, cycles, result);
}

/* PER-SYMBOL reliability-weighted decision - targets a specific failure
 * mode the hard-decision path can't handle at all: real HF fading (QSB)
 * WITHIN a single 110 s transmission (test/wspr_diag_candidate0.c,
 * docs/wspr-phase1-status.md). A signal can be strong for half the
 * transmission and near-noise for the other half; hard-decision (and the
 * reverted per-CAPTURE soft table) both give every symbol the same
 * weight regardless of which half it came from, so ~40+ genuinely
 * unreliable symbols from the weak half poison the decode even though
 * the strong half alone would likely be enough.
 *
 * The fix: weight each symbol's contribution to the Fano metric by that
 * symbol's own LOCAL signal strength (smoothed over nearby symbols, since
 * fading is a slow, continuous envelope, not an independent per-symbol
 * event) relative to the capture's overall level. A symbol from a deep
 * fade contributes almost nothing either way; a symbol from a strong
 * stretch contributes with full confidence. This is a per-symbol analytic
 * formula, not a trained table (deliberately - the per-capture SOFT TABLE
 * attempt above regressed real data despite a careful synthetic
 * validation, and a formula that doesn't depend on matching a pre-trained
 * distribution shape is less exposed to that same synthetic/real
 * mismatch). Called as a FALLBACK, only when hard-decision doesn't
 * already produce a plausible result - see wspr_decode_candidate() -  so
 * candidates hard-decision already handles correctly can't regress. */
static int try_weighted_decision(double tp[WSPR_NSYM][4], wspr_decode_result_t *result)
{
    double d_channel[WSPR_NSYM], power_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double p_data1 = sync ? tp[i][3] : tp[i][2];
        double p_data0 = sync ? tp[i][1] : tp[i][0];
        d_channel[i] = p_data1 - p_data0;
        power_channel[i] = tp[i][0] + tp[i][1] + tp[i][2] + tp[i][3];
    }

    /* Smooth the power envelope over a window of neighboring symbols, IN
     * TRANSMISSION-TIME (channel) ORDER - fading is a slow envelope over
     * real time, and only channel order reflects real time; raw/encode
     * order is scrambled by the interleaver. Window ~15 symbols (~10.2 s)
     * - short enough to track fading on the timescale
     * test/wspr_diag_candidate0.c actually observed (structure visible at
     * ~18 s granularity), long enough to average out symbol-to-symbol
     * jitter rather than chasing it. */
    const int half_window = 7;
    double smoothed[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int lo = i - half_window, hi = i + half_window;
        if (lo < 0) lo = 0;
        if (hi > WSPR_NSYM - 1) hi = WSPR_NSYM - 1;
        double sum = 0;
        for (int j = lo; j <= hi; j++) sum += power_channel[j];
        smoothed[i] = sum / (hi - lo + 1);
    }

    /* Reliability weight relative to the capture's own median smoothed
     * power - median rather than mean so a handful of very strong or very
     * weak symbols can't drag the reference point around. Clamped to keep
     * the search numerically well-behaved (an unclamped near-zero weight
     * would make a symbol contribute nothing at all rather than "very
     * little", which loses information a genuinely marginal symbol might
     * still carry). */
    double sorted[WSPR_NSYM];
    memcpy(sorted, smoothed, sizeof(sorted));
    for (int i = 1; i < WSPR_NSYM; i++) { /* insertion sort - 162 elements, not worth qsort overhead */
        double key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }
    double median = sorted[WSPR_NSYM / 2];
    if (median < 1e-9) median = 1e-9;

    double weight_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        double w = smoothed[i] / median;
        if (w < 0.2) w = 0.2;
        if (w > 3.0) w = 3.0;
        weight_channel[i] = w;
    }

    double d_raw[WSPR_NSYM], weight_raw[WSPR_NSYM];
    wspr_deinterleave_scores(d_channel, d_raw);
    wspr_deinterleave_scores(weight_channel, weight_raw);

    double abs_sum = 0;
    for (int i = 0; i < WSPR_NSYM; i++) abs_sum += fabs(d_raw[i]);
    double scale = abs_sum / WSPR_NSYM;
    if (scale < 1e-9) scale = 1e-9;

    /* ALPHA/BETA/CAP shape the metric the same way the hard-decision
     * table's match=+1/mismatch=-3 does: BETA is a fixed per-symbol
     * penalty ensuring an uninformative (near-zero D) symbol still nets
     * negative for BOTH candidate bits (the Fano algorithm's structural
     * requirement - see wspr_build_hard_metric_table()'s comment), and
     * ALPHA sets how strongly a confident D actually moves the metric.
     * weight[] then scales the WHOLE contribution - informative part and
     * fixed penalty alike - so a low-reliability symbol's vote barely
     * counts either way instead of confidently voting the wrong way. */
    const double ALPHA = 6.0, BETA = 3.0, CAP = 4.0;
    int branch_metric[WSPR_NSYM][2];
    for (int i = 0; i < WSPR_NSYM; i++) {
        double dn = d_raw[i] / scale;
        if (dn > CAP) dn = CAP;
        if (dn < -CAP) dn = -CAP;
        double base = ALPHA * dn;
        branch_metric[i][1] = (int)lround(weight_raw[i] * (base - BETA));
        branch_metric[i][0] = (int)lround(weight_raw[i] * (-base - BETA));
    }

    wspr_msg_bytes_t msg;
    unsigned int metric = 0, cycles = 0;
    if (!wspr_fano_decode_weighted(branch_metric, 2, 20000, &msg, &metric, &cycles)) {
        result->cycles = cycles;
        return 0;
    }
    return accept_if_plausible(&msg, cycles, result);
}

void wspr_decode_candidate(const int16_t *samples, long n, double f0_hz,
                            wspr_decode_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->freq_hz = f0_hz;

    long slack = n - (long)WSPR_NSYM * WSPR_SYM_LEN_SAMPLES;
    if (slack < 0) slack = 0;

    twiddles_t tw;
    if (!build_twiddles(f0_hz, n, &tw)) { free_twiddles(&tw); return; }

    /* Coarse start-time search, then refine around the best coarse hit -
     * exhaustive at symbol-period-fine resolution would be needlessly
     * slow; this two-pass search finds the same optimum in testing. */
    long best_dt = 0;
    double best_score = -1e300;
    long coarse_step = WSPR_SYM_LEN_SAMPLES / 8;
    if (coarse_step < 1) coarse_step = 1;
    for (long dt = 0; dt <= slack; dt += coarse_step) {
        double tp[WSPR_NSYM][4];
        extract_tone_powers(samples, n, &tw, dt, tp);
        double s = sync_score(tp);
        if (s > best_score) { best_score = s; best_dt = dt; }
    }
    long fine_lo = best_dt - coarse_step, fine_hi = best_dt + coarse_step;
    if (fine_lo < 0) fine_lo = 0;
    if (fine_hi > slack) fine_hi = slack;
    long fine_step = WSPR_SYM_LEN_SAMPLES / 32;
    if (fine_step < 1) fine_step = 1;
    for (long dt = fine_lo; dt <= fine_hi; dt += fine_step) {
        double tp[WSPR_NSYM][4];
        extract_tone_powers(samples, n, &tw, dt, tp);
        double s = sync_score(tp);
        if (s > best_score) { best_score = s; best_dt = dt; }
    }

    double tp[WSPR_NSYM][4];
    extract_tone_powers(samples, n, &tw, best_dt, tp);
    free_twiddles(&tw);

    result->best_dt_samples = best_dt;
    result->sync_score = best_score;

    if (try_hard_decision(tp, result)) return;
    /* Hard decision failed or was implausible - give the per-symbol
     * weighted attempt a shot before giving up on this candidate. */
    try_weighted_decision(tp, result);
}
