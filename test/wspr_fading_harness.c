/* Regression test for per-symbol reliability-weighted decoding
 * (wspr_fano_decode_weighted() + main/wspr_decode.c's try_weighted_decision()
 * logic, replicated here against the real decoder functions) - the
 * mechanism built to recover signals with real intra-transmission HF
 * fading (QSB), motivated by test/wspr_diag_candidate0.c's finding that
 * the reference WAV's strongest candidate fails not from a bug but from
 * genuine fading severity a uniform-confidence decoder can't handle.
 *
 * This harness captures the validated result from that investigation
 * (docs/wspr-phase1-status.md) as a permanent, deterministic regression
 * test rather than leaving it in throwaway exploration scripts:
 *
 *  1. MODERATE fading (50 of 162 symbols weak, ~75-80% sync match) - the
 *     weighted decoder measurably beats hard-decision here, including a
 *     specific seed where hard-decision fails outright and weighted
 *     succeeds. This is the claim "per-symbol weighting is a genuine
 *     improvement" rests on.
 *  2. SEVERE fading (90 of 162 symbols near coin-flip, ~57% match -
 *     matching the real candidate's own diagnosed pattern) - BOTH
 *     hard-decision and weighted decoding fail, even with ORACLE
 *     (ground-truth) knowledge of which symbols are weak. This is not a
 *     bug: it's a real information-theoretic limit (too much of the
 *     message is effectively erased for a K=32 rate-1/2 code to
 *     reconstruct), and asserting it stays honest about what this
 *     mechanism can and can't do - a future change that makes case 2
 *     start passing should be treated with suspicion, not celebrated
 *     immediately, since it would mean either a real breakthrough or a
 *     decoder bug (e.g. accidentally decoding noise and calling it
 *     success).
 *
 * Build:
 *   gcc -O2 -Wall -I main -o wspr_fading_harness \
 *       test/wspr_fading_harness.c main/wspr_proto.c main/wspr_fano.c \
 *       -lm && ./wspr_fading_harness
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wspr_proto.h"
#include "wspr_fano.h"

#define SYM_LEN 8192
#define FS 12000.0
#define TONE_SPACING (FS / SYM_LEN)

static int g_fail = 0;

static uint32_t g_rng;
static double rng_uniform(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (g_rng % 1000000 + 1) / 1000001.0;
}
static double rng_gaussian(void)
{
    double u1 = rng_uniform(), u2 = rng_uniform();
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static float g_cos[4][SYM_LEN], g_sin[4][SYM_LEN];
static void init_twiddles(double f0)
{
    for (int k = 0; k < 4; k++) {
        double w = 2.0 * M_PI * (f0 + k * TONE_SPACING) / FS;
        for (int n = 0; n < SYM_LEN; n++) {
            g_cos[k][n] = (float)cos(w * n);
            g_sin[k][n] = (float)sin(w * n);
        }
    }
}

static void synth_symbol_powers(int tone, double amp, double sigma, double tp[4])
{
    double phase0 = rng_uniform() * 2.0 * M_PI;
    double cp = cos(phase0), sp = sin(phase0);
    double re[4] = {0,0,0,0}, im[4] = {0,0,0,0};
    for (int n = 0; n < SYM_LEN; n++) {
        double sig = amp * (sp * g_cos[tone][n] + cp * g_sin[tone][n]);
        double x = sig + sigma * rng_gaussian();
        for (int k = 0; k < 4; k++) { re[k] += x * g_cos[k][n]; im[k] -= x * g_sin[k][n]; }
    }
    for (int k = 0; k < 4; k++) tp[k] = re[k]*re[k] + im[k]*im[k];
}

static int hard_decode(double tp[WSPR_NSYM][4], wspr_msg_bytes_t *msg)
{
    uint8_t channel_bits[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double pd1 = sync ? tp[i][3] : tp[i][2], pd0 = sync ? tp[i][1] : tp[i][0];
        channel_bits[i] = (pd1 > pd0) ? 1 : 0;
    }
    uint8_t raw[WSPR_NSYM], soft[WSPR_NSYM];
    wspr_deinterleave(channel_bits, raw);
    for (int i = 0; i < WSPR_NSYM; i++) soft[i] = raw[i] ? 255 : 0;
    int mettab[2][256];
    wspr_build_hard_metric_table(mettab);
    unsigned int metric = 0, cycles = 0;
    return wspr_fano_decode(soft, mettab, 2, 20000, msg, &metric, &cycles);
}

/* Same weighting formula as main/wspr_decode.c's try_weighted_decision(),
 * but taking an EXPLICIT weight array (oracle or estimated) instead of
 * deriving it from a smoothed envelope - lets this harness test the
 * metric-combination logic directly against known weight patterns. */
static int weighted_decode(double tp[WSPR_NSYM][4], const double weight_channel[WSPR_NSYM],
                            wspr_msg_bytes_t *msg, unsigned int *cycles_out)
{
    double d_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double pd1 = sync ? tp[i][3] : tp[i][2], pd0 = sync ? tp[i][1] : tp[i][0];
        d_channel[i] = pd1 - pd0;
    }
    double d_raw[WSPR_NSYM], weight_raw[WSPR_NSYM];
    wspr_deinterleave_scores(d_channel, d_raw);
    wspr_deinterleave_scores(weight_channel, weight_raw);

    double abs_sum = 0; int n_strong = 0;
    for (int i = 0; i < WSPR_NSYM; i++) if (weight_raw[i] > 0.5) { abs_sum += fabs(d_raw[i]); n_strong++; }
    double scale = n_strong > 0 ? abs_sum / n_strong : 1.0;
    if (scale < 1e-9) scale = 1e-9;

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
    unsigned int metric = 0;
    return wspr_fano_decode_weighted(branch_metric, 2, 20000, msg, &metric, cycles_out);
}

/* ---- Test 1: uniform strong signal - mechanism sanity check ---- */

static void test_uniform_sanity(void)
{
    printf("-- 1. uniform strong signal (no fading) - mechanism sanity --\n");
    wspr_msg_bytes_t truth;
    wspr_pack_message("K1ABC", "FN20", 37, &truth);
    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], tones[WSPR_NSYM];
    wspr_convolve_encode(&truth, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, tones);
    init_twiddles(1450.0);

    g_rng = 0xCAFEBABEu;
    double tp[WSPR_NSYM][4];
    for (int i = 0; i < WSPR_NSYM; i++) synth_symbol_powers(tones[i], 0.15, 0.10, tp[i]);

    double weight[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) weight[i] = 1.0;
    wspr_msg_bytes_t decoded;
    unsigned int cycles = 0;
    int ok = weighted_decode(tp, weight, &decoded, &cycles);
    int correct = ok && memcmp(truth.dat, decoded.dat, 7) == 0;
    printf("  %s  weighted decode of a clean signal succeeds (cycles=%u)\n",
           correct ? "PASS" : "FAIL", cycles);
    if (!correct) g_fail++;
}

/* ---- Test 2: moderate fading - weighted beats hard-decision ---- */

static void test_moderate_fading(void)
{
    printf("\n-- 2. moderate fading (50/162 weak, ~75-80%% match) --\n");
    wspr_msg_bytes_t truth;
    wspr_pack_message("K1ABC", "FN20", 37, &truth);
    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], tones[WSPR_NSYM];
    wspr_convolve_encode(&truth, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, tones);
    init_twiddles(1450.0);

    int n_weak = 50;
    double weak_amp = 0.003, strong_amp = 0.15;
    double weight[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) weight[i] = (i < n_weak) ? 0.25 : 1.0;

    int hard_correct = 0, weighted_correct = 0, weighted_saved_a_miss = 0, n_trials = 10;
    for (int seed = 0; seed < n_trials; seed++) {
        g_rng = 0x2000u + (uint32_t)seed * 0x9E3779B9u;
        double tp[WSPR_NSYM][4];
        for (int i = 0; i < WSPR_NSYM; i++) {
            double amp = (i < n_weak) ? weak_amp : strong_amp;
            synth_symbol_powers(tones[i], amp, 0.10, tp[i]);
        }
        wspr_msg_bytes_t hd, wd;
        int hok = hard_decode(tp, &hd);
        int hc = hok && memcmp(truth.dat, hd.dat, 7) == 0;
        unsigned int cycles;
        int wok = weighted_decode(tp, weight, &wd, &cycles);
        int wc = wok && memcmp(truth.dat, wd.dat, 7) == 0;
        hard_correct += hc; weighted_correct += wc;
        if (!hc && wc) weighted_saved_a_miss++;
    }
    printf("  hard-decision:    %d/%d correct\n", hard_correct, n_trials);
    printf("  weighted:         %d/%d correct\n", weighted_correct, n_trials);
    printf("  cases weighted recovered that hard-decision missed: %d\n", weighted_saved_a_miss);
    /* The claim this mechanism exists to support: weighted is at least as
     * good as hard-decision at moderate fading, and recovers at least one
     * case hard-decision cannot. If a future change regresses this below
     * hard-decision's own rate, something is wrong with the weighting,
     * not just "unlucky seeds" - these seeds are fixed. */
    int pass = (weighted_correct >= hard_correct) && (weighted_saved_a_miss >= 1);
    printf("  %s  weighted is >= hard-decision AND recovers >= 1 case hard-decision misses\n",
           pass ? "PASS" : "FAIL");
    if (!pass) g_fail++;
}

/* ---- Test 3: severe fading - both fail, and that's correct ---- */

static void test_severe_fading_both_fail(void)
{
    printf("\n-- 3. severe fading (90/162 near coin-flip, ~57%% match) --\n");
    printf("   matches test/wspr_diag_candidate0.c's diagnosed real-world pattern.\n");
    printf("   BOTH decoders are expected to fail here - not a bug, a genuine\n");
    printf("   information-theoretic limit (confirmed with ORACLE weighting too).\n");
    printf("   This assertion exists to catch a future decoder bug (e.g. a false\n");
    printf("   positive on noise) disguised as \"finally recovered it\", not to\n");
    printf("   discourage trying - see docs/wspr-phase1-status.md before treating\n");
    printf("   a change here as a real breakthrough.\n");

    wspr_msg_bytes_t truth;
    wspr_pack_message("K1ABC", "FN20", 37, &truth);
    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], tones[WSPR_NSYM];
    wspr_convolve_encode(&truth, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, tones);
    init_twiddles(1450.0);

    int n_weak = 90;
    double weak_amp = 0.0014, strong_amp = 0.15;
    /* ORACLE weighting (ground truth, near-zero for the known-weak
     * region) - the BEST CASE for this mechanism. If even this fails,
     * no estimated (non-oracle) weighting could do better. */
    double weight[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) weight[i] = (i < n_weak) ? 0.001 : 1.0;

    int hard_correct = 0, weighted_correct = 0, n_trials = 10;
    for (int seed = 0; seed < n_trials; seed++) {
        g_rng = 0x1000u + (uint32_t)seed * 0x9E3779B9u;
        double tp[WSPR_NSYM][4];
        for (int i = 0; i < WSPR_NSYM; i++) {
            double amp = (i < n_weak) ? weak_amp : strong_amp;
            synth_symbol_powers(tones[i], amp, 0.10, tp[i]);
        }
        wspr_msg_bytes_t hd, wd;
        int hok = hard_decode(tp, &hd);
        hard_correct += (hok && memcmp(truth.dat, hd.dat, 7) == 0);
        unsigned int cycles;
        int wok = weighted_decode(tp, weight, &wd, &cycles);
        weighted_correct += (wok && memcmp(truth.dat, wd.dat, 7) == 0);
    }
    printf("  hard-decision: %d/%d correct (expected 0)\n", hard_correct, n_trials);
    printf("  weighted (oracle): %d/%d correct (expected 0)\n", weighted_correct, n_trials);
    int pass = (hard_correct == 0) && (weighted_correct == 0);
    printf("  %s  both fail as expected at this severity\n", pass ? "PASS" : "FAIL");
    if (!pass) {
        g_fail++;
        printf("  NOTE: if this FAILED because weighted_correct > 0, that could be\n");
        printf("  a genuine breakthrough OR a false-positive decode of noise -\n");
        printf("  verify against the real WAV before trusting it either way.\n");
    }
}

int main(void)
{
    test_uniform_sanity();
    test_moderate_fading();
    test_severe_fading_both_fail();

    printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
