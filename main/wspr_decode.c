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

    /* Hard-decision per symbol, conditioned on the known sync bit (see
     * wspr_decode.h's "known limitations" - a real soft metric is future
     * work; this is proven-correct, not maximally sensitive). */
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
    int fano_ok = wspr_fano_decode(soft, mettab, 2, 20000, &msg, &metric, &cycles);
    result->cycles = cycles;
    if (!fano_ok) return;

    char call[7], grid[5];
    int dbm;
    if (!wspr_unpack_message(&msg, call, grid, &dbm)) return;

    /* Three independent checks, all sourced/justified in wspr_decode.h and
     * this file's comments - message shape, legal power quantization, and
     * decoder convergence speed. All three must agree. */
    if (strlen(call) < 3) return;
    if (!is_legal_power(dbm)) return;
    if (cycles > WSPR_CYCLES_SUSPECT) return;
    wspr_msg_bytes_t repack;
    if (!wspr_pack_message(call, grid, dbm, &repack)) return;

    strcpy(result->callsign, call);
    strcpy(result->grid, grid);
    result->power_dbm = dbm;
    result->ok = 1;
}
