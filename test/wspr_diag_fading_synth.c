/* Controlled test: does per-symbol reliability weighting recover a
 * message with a KNOWN fading pattern matching what
 * test/wspr_diag_candidate0.c diagnosed in the real signal (weak/near-
 * noise first half, strong second half)? If hard-decision fails this and
 * weighted succeeds, the mechanism works and the real candidate's formula
 * just needs more tuning. If weighted ALSO fails here, the formula itself
 * has a problem worth fixing before spending more time on the harder
 * real-world case.
 *
 * ANSWERED (see docs/wspr-phase1-status.md and test/wspr_fading_harness.c,
 * the permanent regression test this exploration led to): the mechanism
 * is not broken - a badly-calibrated first attempt at this file gave a
 * false alarm (its "weak" amplitude turned out to still give 100% match,
 * not actually testing fading at all). At a properly-calibrated moderate
 * severity, weighted decoding measurably beats hard-decision (10/10 vs
 * 9/10 across seeds). At the real candidate's actual (severe) fading
 * level, BOTH decoders fail even with oracle weighting - a genuine
 * information-theoretic limit, not a defect. Kept as an exploration
 * script; wspr_fading_harness.c is the authoritative, assertion-based
 * version of this same investigation.
 *
 * Build:
 *   gcc -O2 -Wall -I main -o wspr_diag_fading_synth \
 *       test/wspr_diag_fading_synth.c main/wspr_proto.c main/wspr_fano.c \
 *       -lm
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

static uint32_t g_rng = 0x1234ABCDu;
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

/* Synthesize one symbol's worth of samples for tone index `tone` (0-3) at
 * the given amplitude, add noise, and return the 4-tone coherent power
 * vector - directly, without going through an intermediate sample buffer,
 * since we only need the post-DFT statistic here. */
static void synth_symbol_powers(int tone, double amp, double sigma, double tp[4])
{
    double phase0 = rng_uniform() * 2.0 * M_PI;
    double cp = cos(phase0), sp = sin(phase0);
    double re[4] = {0,0,0,0}, im[4] = {0,0,0,0};
    for (int n = 0; n < SYM_LEN; n++) {
        double sig = amp * (sp * g_cos[tone][n] + cp * g_sin[tone][n]);
        double x = sig + sigma * rng_gaussian();
        for (int k = 0; k < 4; k++) {
            re[k] += x * g_cos[k][n];
            im[k] -= x * g_sin[k][n];
        }
    }
    for (int k = 0; k < 4; k++) tp[k] = re[k]*re[k] + im[k]*im[k];
}

static int try_hard(double tp[WSPR_NSYM][4], char call[7], char grid[5], int *dbm, unsigned int *cycles)
{
    uint8_t channel_bits[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double p1 = sync ? tp[i][3] : tp[i][1];
        double p0 = sync ? tp[i][1] : tp[i][0]; /* fixed below */
        (void)p0;
        double pd1 = sync ? tp[i][3] : tp[i][2];
        double pd0 = sync ? tp[i][1] : tp[i][0];
        (void)p1;
        channel_bits[i] = (pd1 > pd0) ? 1 : 0;
    }
    uint8_t raw[WSPR_NSYM], soft[WSPR_NSYM];
    wspr_deinterleave(channel_bits, raw);
    for (int i = 0; i < WSPR_NSYM; i++) soft[i] = raw[i] ? 255 : 0;
    int mettab[2][256];
    wspr_build_hard_metric_table(mettab);
    wspr_msg_bytes_t msg;
    unsigned int metric = 0;
    int ok = wspr_fano_decode(soft, mettab, 2, 20000, &msg, &metric, cycles);
    if (!ok) return 0;
    return wspr_unpack_message(&msg, call, grid, dbm);
}

static int try_weighted(double tp[WSPR_NSYM][4], int half_window, double alpha,
                         double beta, double cap, double wmin, double wmax,
                         char call[7], char grid[5], int *dbm, unsigned int *cycles)
{
    double d_channel[WSPR_NSYM], power_channel[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int sync = wspr_sync_vector[i];
        double pd1 = sync ? tp[i][3] : tp[i][2];
        double pd0 = sync ? tp[i][1] : tp[i][0];
        d_channel[i] = pd1 - pd0;
        power_channel[i] = tp[i][0]+tp[i][1]+tp[i][2]+tp[i][3];
    }
    double smoothed[WSPR_NSYM];
    for (int i = 0; i < WSPR_NSYM; i++) {
        int lo = i-half_window, hi = i+half_window;
        if (lo<0) lo=0; if (hi>WSPR_NSYM-1) hi=WSPR_NSYM-1;
        double s=0; for (int j=lo;j<=hi;j++) s+=power_channel[j];
        smoothed[i]=s/(hi-lo+1);
    }
    double sorted[WSPR_NSYM]; memcpy(sorted, smoothed, sizeof(sorted));
    for (int i=1;i<WSPR_NSYM;i++){double key=sorted[i];int j=i-1;while(j>=0&&sorted[j]>key){sorted[j+1]=sorted[j];j--;}sorted[j+1]=key;}
    double median = sorted[WSPR_NSYM/2]; if (median<1e-9) median=1e-9;
    double weight_channel[WSPR_NSYM];
    for (int i=0;i<WSPR_NSYM;i++){double w=smoothed[i]/median; if(w<wmin)w=wmin; if(w>wmax)w=wmax; weight_channel[i]=w;}
    double d_raw[WSPR_NSYM], weight_raw[WSPR_NSYM];
    wspr_deinterleave_scores(d_channel, d_raw);
    wspr_deinterleave_scores(weight_channel, weight_raw);
    double abs_sum=0; for(int i=0;i<WSPR_NSYM;i++) abs_sum+=fabs(d_raw[i]);
    double scale = abs_sum/WSPR_NSYM; if (scale<1e-9) scale=1e-9;
    int branch_metric[WSPR_NSYM][2];
    for (int i=0;i<WSPR_NSYM;i++){
        double dn=d_raw[i]/scale; if(dn>cap)dn=cap; if(dn<-cap)dn=-cap;
        double base=alpha*dn;
        branch_metric[i][1]=(int)lround(weight_raw[i]*(base-beta));
        branch_metric[i][0]=(int)lround(weight_raw[i]*(-base-beta));
    }
    wspr_msg_bytes_t msg; unsigned int metric=0;
    int ok = wspr_fano_decode_weighted(branch_metric, 2, 20000, &msg, &metric, cycles);
    if (!ok) return 0;
    return wspr_unpack_message(&msg, call, grid, dbm);
}

int main(void)
{
    double f0 = 1450.0;
    init_twiddles(f0);

    wspr_msg_bytes_t msg;
    wspr_pack_message("K1ABC", "FN20", 37, &msg);
    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], tones[WSPR_NSYM];
    wspr_convolve_encode(&msg, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, tones);

    /* Match the diagnosed real pattern: symbols 0-89 (~55%) heavily
     * faded (amp low enough that hard-decision match rate lands near
     * 50-60%, same as measured), symbols 90-161 strong. */
    double tp[WSPR_NSYM][4];
    double sigma = 0.10;
    for (int i = 0; i < WSPR_NSYM; i++) {
        /* Calibrated (via a separate amplitude sweep) so the first-half
         * sync match rate lands at ~57%, matching the real signal's
         * diagnosed ~52-63% - the first attempt at this used amp=0.012,
         * which turned out to still give 100% match (not actually weak),
         * making that test not representative of the real failure mode
         * at all. */
        double amp = (i < 90) ? 0.0014 : 0.15;
        synth_symbol_powers(tones[i], amp, sigma, tp[i]);
    }

    /* Sanity-check the fading pattern matches what was diagnosed on the
     * real signal - print match rate by half. */
    int match_first = 0, match_second = 0;
    for (int i = 0; i < WSPR_NSYM; i++) {
        double pd1 = tp[i][1] + tp[i][3], pd0 = tp[i][0] + tp[i][2];
        int predicted = (pd1 > pd0) ? 1 : 0;
        int actual = wspr_sync_vector[i];
        if (i < 90) { if (predicted == actual) match_first++; }
        else { if (predicted == actual) match_second++; }
    }
    fprintf(stderr, "sanity: first-half sync match %d/90 (%.0f%%), second-half %d/72 (%.0f%%)\n",
            match_first, 100.0*match_first/90.0, match_second, 100.0*match_second/72.0);

    char call[7], grid[5]; int dbm; unsigned int cycles;
    int hard_ok = try_hard(tp, call, grid, &dbm, &cycles);
    fprintf(stderr, "\nhard-decision: ok=%d", hard_ok);
    if (hard_ok) fprintf(stderr, " call='%s' grid='%s' dbm=%d cycles=%u", call, grid, dbm, cycles);
    fprintf(stderr, "\n");

    fprintf(stderr, "\nweighted decode sweep:\n");
    struct { int win; double a,b,cap,wmin,wmax; const char *label; } trials[] = {
        {7, 6,3,4, 0.2,3.0, "default"},
        {15,6,3,4, 0.2,3.0, "wider window"},
        {7, 10,3,4,0.2,3.0, "stronger alpha"},
        {7, 6,3,4, 0.02,5.0,"wider clamp"},
        {11,8,2,6, 0.05,6.0,"combined"},
    };
    for (size_t t = 0; t < sizeof(trials)/sizeof(trials[0]); t++) {
        int ok = try_weighted(tp, trials[t].win, trials[t].a, trials[t].b, trials[t].cap,
                               trials[t].wmin, trials[t].wmax, call, grid, &dbm, &cycles);
        int correct = ok && strcmp(call,"K1ABC")==0 && strcmp(grid,"FN20")==0 && dbm==37;
        fprintf(stderr, "  [%-16s] ok=%d correct=%d", trials[t].label, ok, correct);
        if (ok) fprintf(stderr, " call='%s' grid='%s' dbm=%d cycles=%u", call, grid, dbm, cycles);
        fprintf(stderr, "\n");
    }
    return 0;
}
