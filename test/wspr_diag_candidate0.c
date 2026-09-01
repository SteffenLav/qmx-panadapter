/* Diagnostic: why does the reference WAV's strongest candidate
 * (f0=1500.933 Hz) fail to decode plausibly while weaker candidates
 * decode cleanly? ANSWERED (see docs/wspr-phase1-status.md): real
 * ionospheric fading (QSB) within the 110 s transmission, not a bug, not
 * frequency drift (already ruled out separately), not a second
 * overlapping signal.
 *
 * Three pieces of evidence, all produced by this tool:
 *  1. Sync-bit match rate climbs from ~52-63% (near coin-flip) in the
 *     first ~55 s to 81-89% in the last ~55 s - uniform noise would sit
 *     flat near 50% throughout; a steady signal would sit flat near
 *     wherever its real confidence is. A clear monotonic climb is fading.
 *  2. Total 4-tone power rises ~20x from the first window to the last,
 *     tracking the same shape - the signal was physically weak for
 *     roughly the first half of the transmission and strong for the
 *     second half.
 *  3. Scanning +/-3 Hz around the candidate frequency shows ONE clean,
 *     single-humped peak - no second bump, ruling out an overlapping
 *     second station sharing this frequency.
 *
 * Why the decoder can't recover it despite the real signal being there:
 * roughly 46 of 162 symbols are wrong, mostly clustered in the noisy
 * first half, and the metric table (hard OR the reverted per-capture
 * soft table) gives every symbol the SAME confidence regardless of
 * whether it came from the coin-flip-weak or crystal-clear part of the
 * signal. The code reliably corrects ~2 symbol errors (see
 * wspr_codec_harness.c); 46 uniformly-weighted "errors" is far beyond
 * that. A decoder with genuine PER-SYMBOL (not just per-capture) local
 * confidence weighting - down-weighting the noisy early symbols, trusting
 * the strong late ones - would have a real shot at this. That's a
 * different, more targeted idea than either soft-metric attempt so far
 * (both used one global scale per capture, not a per-symbol local one) -
 * worth trying before assuming soft metrics in general don't help on real
 * signals.
 *
 * Kept as a reusable diagnostic, not deleted - useful for any future
 * "why doesn't this candidate decode" question.
 *
 * Build:
 *   gcc -O2 -Wall -I main -o wspr_diag_candidate0 \
 *       test/wspr_diag_candidate0.c main/wspr_proto.c main/wspr_fano.c \
 *       -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wspr_proto.h"
#include "wspr_fano.h"

#define FS 12000.0
#define SYM_LEN 8192
#define TONE_SPACING (FS / SYM_LEN)

static int16_t *g_samples = NULL;
static long g_nsamples = 0;

static int load_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 0; }
    uint8_t hdr[12];
    fread(hdr, 1, 12, f);
    int channels = 1, bits = 16;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t cksize = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);
        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            fread(fmt, 1, cksize < 16 ? cksize : 16, f);
            channels = fmt[2] | (fmt[3] << 8);
            bits = fmt[14] | (fmt[15] << 8);
            if (cksize > 16) fseek(f, (long)(cksize - 16), SEEK_CUR);
        } else if (memcmp(ck, "data", 4) == 0) {
            g_nsamples = cksize / (bits / 8) / channels;
            g_samples = (int16_t *)malloc((size_t)g_nsamples * sizeof(int16_t));
            for (long i = 0; i < g_nsamples; i++) {
                uint8_t s[4];
                int bpf = (bits / 8) * channels;
                if (fread(s, 1, (size_t)bpf, f) != (size_t)bpf) { g_nsamples = i; break; }
                g_samples[i] = (int16_t)(s[0] | (s[1] << 8));
            }
            break;
        } else {
            fseek(f, (long)(cksize + (cksize & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    return g_nsamples > 0;
}

typedef struct { float *cos_tab[4]; float *sin_tab[4]; } twiddles_t;

static void build_twiddles(double f0, twiddles_t *tw)
{
    for (int k = 0; k < 4; k++) {
        tw->cos_tab[k] = (float *)malloc((size_t)g_nsamples * sizeof(float));
        tw->sin_tab[k] = (float *)malloc((size_t)g_nsamples * sizeof(float));
        double w = 2.0 * M_PI * (f0 + k * TONE_SPACING) / FS;
        for (long n = 0; n < g_nsamples; n++) {
            tw->cos_tab[k][n] = (float)cos(w * n);
            tw->sin_tab[k][n] = (float)sin(w * n);
        }
    }
}
static void free_twiddles(twiddles_t *tw)
{
    for (int k = 0; k < 4; k++) { free(tw->cos_tab[k]); free(tw->sin_tab[k]); }
}

static void extract_tone_powers(const twiddles_t *tw, long start_sample,
                                 double tone_power[WSPR_NSYM][4])
{
    for (int sym = 0; sym < WSPR_NSYM; sym++) {
        long base = start_sample + (long)sym * SYM_LEN;
        long n0 = base < 0 ? 0 : base;
        long n1 = base + SYM_LEN > g_nsamples ? g_nsamples : base + SYM_LEN;
        for (int k = 0; k < 4; k++) {
            const float *ct = tw->cos_tab[k], *st = tw->sin_tab[k];
            float re = 0, im = 0;
            for (long idx = n0; idx < n1; idx++) {
                float x = g_samples[idx] / 32768.0f;
                re += x * ct[idx]; im -= x * st[idx];
            }
            tone_power[sym][k] = (double)re * re + (double)im * im;
        }
    }
}

/* Self-contained replica of wspr_decode.c's try_weighted_decision(), but
 * with tunable parameters and UNCONDITIONAL diagnostic output (prints the
 * raw decode result regardless of whether it passes the plausibility
 * gate) - for iterating on ALPHA/BETA/CAP/window/clamp quickly against a
 * known-hard real candidate instead of guessing blind. */
static void try_weighted_diag(double tp[WSPR_NSYM][4], int half_window,
                               double alpha, double beta, double cap,
                               double wmin, double wmax, const char *label)
{
    double d_channel[WSPR_NSYM], power_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double p_data1 = sync ? tp[i][3] : tp[i][2];
        double p_data0 = sync ? tp[i][1] : tp[i][0];
        d_channel[i] = p_data1 - p_data0;
        power_channel[i] = tp[i][0] + tp[i][1] + tp[i][2] + tp[i][3];
    }

    double smoothed[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int lo = i - half_window, hi = i + half_window;
        if (lo < 0) lo = 0;
        if (hi > WSPR_NSYM - 1) hi = WSPR_NSYM - 1;
        double sum = 0;
        for (int j = lo; j <= hi; j++) sum += power_channel[j];
        smoothed[i] = sum / (hi - lo + 1);
    }

    double sorted[WSPR_NSYM];
    memcpy(sorted, smoothed, sizeof(sorted));
    for (int i = 1; i < WSPR_NSYM; i++) {
        double key = sorted[i]; int j = i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }
    double median = sorted[WSPR_NSYM / 2];
    if (median < 1e-9) median = 1e-9;

    double weight_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        double w = smoothed[i] / median;
        if (w < wmin) w = wmin;
        if (w > wmax) w = wmax;
        weight_channel[i] = w;
    }

    double d_raw[WSPR_NSYM], weight_raw[WSPR_NSYM];
    wspr_deinterleave_scores(d_channel, d_raw);
    wspr_deinterleave_scores(weight_channel, weight_raw);

    double abs_sum = 0;
    for (int i = 0; i < WSPR_NSYM; i++) abs_sum += fabs(d_raw[i]);
    double scale = abs_sum / WSPR_NSYM;
    if (scale < 1e-9) scale = 1e-9;

    int branch_metric[WSPR_NSYM][2];
    for (int i = 0; i < WSPR_NSYM; i++) {
        double dn = d_raw[i] / scale;
        if (dn > cap) dn = cap;
        if (dn < -cap) dn = -cap;
        double base = alpha * dn;
        branch_metric[i][1] = (int)lround(weight_raw[i] * (base - beta));
        branch_metric[i][0] = (int)lround(weight_raw[i] * (-base - beta));
    }

    wspr_msg_bytes_t msg;
    unsigned int metric = 0, cycles = 0;
    int ok = wspr_fano_decode_weighted(branch_metric, 2, 20000, &msg, &metric, &cycles);
    if (!ok) {
        fprintf(stderr, "  [%s] TIMEOUT (cycles=%u)\n", label, cycles);
        return;
    }
    char call[7], grid[5]; int dbm;
    int unpacked = wspr_unpack_message(&msg, call, grid, &dbm);
    fprintf(stderr, "  [%s] converged cycles=%u  unpacked=%d call='%s' grid='%s' dbm=%d\n",
            label, cycles, unpacked, unpacked ? call : "?", unpacked ? grid : "?",
            unpacked ? dbm : -1);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "test/wav_reference/wspr/150426_0918.wav";
    if (!load_wav(path)) { fprintf(stderr, "load failed\n"); return 1; }
    fprintf(stderr, "loaded %ld samples\n", g_nsamples);

    double f0 = 1500.933;
    long best_dt = 2048; /* from the earlier decode_harness run */

    twiddles_t tw;
    build_twiddles(f0, &tw);
    double tp[WSPR_NSYM][4];
    extract_tone_powers(&tw, best_dt, tp);
    free_twiddles(&tw);

    /* Per-symbol sync-bit match: does the STRONGER of tones{0,2} vs
     * {1,3} match the known sync bit at this position? Mismatches
     * clustered in time = fading/drift; uniform ~50% = genuine noise;
     * high uniform match with wrong DATA bits = something else. */
    int match = 0;
    int window_match[6] = {0,0,0,0,0,0}; /* 6 chunks of 27 symbols each */
    double window_score[6] = {0,0,0,0,0,0};
    for (int i = 0; i < WSPR_NSYM; i++) {
        double p1 = tp[i][1] + tp[i][3];
        double p0 = tp[i][0] + tp[i][2];
        int predicted_sync = (p1 > p0) ? 1 : 0;
        int actual_sync = wspr_sync_vector[i];
        int chunk = i / 27;
        if (chunk > 5) chunk = 5;
        window_score[chunk] += (actual_sync ? (p1 - p0) : (p0 - p1));
        if (predicted_sync == actual_sync) { match++; window_match[chunk]++; }
    }
    fprintf(stderr, "\nsync-bit match: %d/162 (%.1f%%) - 50%% would be pure noise\n",
            match, 100.0 * match / WSPR_NSYM);
    fprintf(stderr, "match rate by time-window (27 symbols ~18.4s each):\n");
    for (int c = 0; c < 6; c++) {
        fprintf(stderr, "  window %d (sym %3d-%3d): %2d/27 match (%.0f%%)  sync-score-sum=%.0f\n",
                c, c*27, c*27+26, window_match[c], 100.0*window_match[c]/27.0, window_score[c]);
    }

    /* Raw tone power magnitude over time - is the signal steady, or does
     * it fade in/out (would show as total power dropping mid-transmission)? */
    fprintf(stderr, "\ntotal 4-tone power by time-window (steady vs fading):\n");
    for (int c = 0; c < 6; c++) {
        double tot = 0;
        for (int i = c*27; i < c*27+27 && i < WSPR_NSYM; i++) {
            for (int k = 0; k < 4; k++) tot += tp[i][k];
        }
        fprintf(stderr, "  window %d: total power=%.1f\n", c, tot);
    }

    /* Widen the search: is there possibly a SECOND, separate comb very
     * close to f0 (a second overlapping station)? Scan +/- 3 Hz around
     * f0 in fine steps and print comb energy - a single clean signal
     * should show ONE peak; two overlapping stations might show a
     * double-humped or broadened peak. */
    fprintf(stderr, "\ncomb energy vs frequency offset near f0 (looking for a second signal):\n");
    for (double df = -3.0; df <= 3.0; df += 0.3) {
        twiddles_t tw2;
        build_twiddles(f0 + df, &tw2);
        double tp2[WSPR_NSYM][4];
        extract_tone_powers(&tw2, best_dt, tp2);
        free_twiddles(&tw2);
        double tot = 0;
        for (int i = 0; i < WSPR_NSYM; i++) for (int k = 0; k < 4; k++) tot += tp2[i][k];
        fprintf(stderr, "  df=%+5.2f Hz  total_power=%10.1f\n", df, tot);
    }

    fprintf(stderr, "\nper-symbol weighted decode - parameter sweep:\n");
    try_weighted_diag(tp, 7,  6.0, 3.0, 4.0, 0.2, 3.0, "default (win=7,a=6,b=3,cap=4)");
    try_weighted_diag(tp, 3,  6.0, 3.0, 4.0, 0.2, 3.0, "narrower window (win=3)");
    try_weighted_diag(tp, 15, 6.0, 3.0, 4.0, 0.2, 3.0, "wider window (win=15)");
    try_weighted_diag(tp, 25, 6.0, 3.0, 4.0, 0.2, 3.0, "very wide window (win=25)");
    try_weighted_diag(tp, 7,  10.0, 3.0, 4.0, 0.2, 3.0, "stronger alpha (a=10)");
    try_weighted_diag(tp, 7,  6.0, 1.0, 4.0, 0.2, 3.0, "weaker beta (b=1)");
    try_weighted_diag(tp, 7,  6.0, 6.0, 4.0, 0.2, 3.0, "stronger beta (b=6)");
    try_weighted_diag(tp, 7,  6.0, 3.0, 4.0, 0.05, 5.0, "wider weight clamp (0.05-5)");
    try_weighted_diag(tp, 7,  6.0, 3.0, 4.0, 0.02, 8.0, "very wide weight clamp (0.02-8)");
    try_weighted_diag(tp, 7,  6.0, 3.0, 8.0, 0.2, 3.0, "wider D cap (cap=8)");
    try_weighted_diag(tp, 11, 8.0, 2.0, 6.0, 0.05, 6.0, "combined tweak");

    return 0;
}
