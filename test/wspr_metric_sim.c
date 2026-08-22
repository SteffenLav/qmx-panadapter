/* Build a real soft-decision metric table via Monte Carlo simulation of
 * THIS decoder's own channel statistic - not copied from anyone's
 * published table (K9AN's metric_tables.c is GPL v3, see
 * docs/wspr-phase1-status.md's licensing section; this is independent
 * original work instead).
 *
 * SECOND ATTEMPT - PER-CAPTURE NORMALIZATION. The first attempt (see git
 * history / docs/wspr-phase1-status.md) built one table at one fixed
 * calibration amplitude and found it only worked in a narrow band around
 * that amplitude - D scales roughly with amplitude^2, so any candidate
 * signal noticeably stronger or weaker than the calibration point
 * saturated into unreliable table entries. The fix: normalize each
 * symbol's D by an estimate of the SIGNAL'S OWN typical magnitude before
 * quantizing - both here (during training, pooled across many amplitudes)
 * and in main/wspr_decode.c (during real decoding, from the actual
 * candidate's own 162 symbols) - so the table is calibrated in
 * SCALE-INVARIANT units instead of absolute ones. This is the same idea
 * real receivers implement as AGC; it isn't a perfect equalizer (a
 * genuinely low-SNR signal's normalized statistics are still noisier than
 * a high-SNR one's, even after removing the raw amplitude), but it should
 * remove the GROSS mismatch that broke the first attempt.
 *
 * The statistic being calibrated is what main/wspr_decode.c computes per
 * symbol: given the known sync bit, the difference in coherent-DFT power
 * between the two candidate data tones, D = P(tone for data=1) - P(tone
 * for data=0).
 *
 * Build:
 *   gcc -O2 -Wall -I main -o wspr_metric_sim test/wspr_metric_sim.c \
 *       main/wspr_proto.c main/wspr_fano.c -lm
 *
 * Prints a C array literal (the built table) to stdout; reports the
 * self-check (does this table + per-capture normalization actually beat
 * the -22.7 dB hard-decision baseline across a wide SNR sweep?) to stderr.
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
#define NBYTE 256

static uint32_t g_rng = 0xA5A5A5A5u;
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

/* Precomputed twiddle tables - see docs/wspr-phase1-status.md, this exact
 * optimization was needed twice already (main/wspr_decode.c's start-time
 * search, and this file's first version) before a per-sample cos()/sin()
 * call was recognized as the thing making runs impractically slow. */
static float g_cos0[SYM_LEN], g_sin0[SYM_LEN], g_cos1[SYM_LEN], g_sin1[SYM_LEN];

static void init_twiddles(void)
{
    double w0 = 2.0 * M_PI * 1450.0 / FS;
    double w1 = 2.0 * M_PI * (1450.0 + TONE_SPACING) / FS;
    for (int n = 0; n < SYM_LEN; n++) {
        g_cos0[n] = (float)cos(w0 * n);
        g_sin0[n] = (float)sin(w0 * n);
        g_cos1[n] = (float)cos(w1 * n);
        g_sin1[n] = (float)sin(w1 * n);
    }
}

/* Simulate one symbol's coherent DFT power difference D = P(tone1) -
 * P(tone0) - `sent_bit` selects which tone carries the signal. `amp` is
 * the tone's peak amplitude, `sigma` the AWGN per-sample stddev. */
static double simulate_D(int sent_bit, double amp, double sigma)
{
    double re0 = 0, im0 = 0, re1 = 0, im1 = 0;
    double phase0 = rng_uniform() * 2.0 * M_PI;
    double cp = cos(phase0), sp = sin(phase0);
    const float *sig_cos = (sent_bit == 0) ? g_cos0 : g_cos1;
    const float *sig_sin = (sent_bit == 0) ? g_sin0 : g_sin1;
    for (int n = 0; n < SYM_LEN; n++) {
        double sig = amp * (sp * sig_cos[n] + cp * sig_sin[n]);
        double x = sig + sigma * rng_gaussian();
        re0 += x * g_cos0[n]; im0 -= x * g_sin0[n];
        re1 += x * g_cos1[n]; im1 -= x * g_sin1[n];
    }
    double p0 = re0 * re0 + im0 * im0;
    double p1 = re1 * re1 + im1 * im1;
    return p1 - p0;
}

/* Estimate the normalization scale for a batch of D values the same way a
 * real decode would: from the batch's OWN mean absolute value. Shared by
 * training (pooling across amplitudes) and the self-check (per-capture),
 * so the two can't quietly use different conventions. */
static double batch_scale(const double *d, int n)
{
    double sum = 0;
    for (int i = 0; i < n; i++) sum += fabs(d[i]);
    double s = sum / n;
    return (s > 1e-9) ? s : 1e-9;
}

static long g_hist0[NBYTE], g_hist1[NBYTE];

static void train_at_amplitude(double amp, double sigma, long trials)
{
    /* Pilot batch to estimate this amplitude's own typical |D| - trained
     * the same way decode-time per-capture normalization will estimate
     * it from a real candidate's 162 symbols, just with a bigger sample
     * for a cleaner training signal. */
    enum { PILOT = 4000 };
    double pilot_d[PILOT];
    for (int t = 0; t < PILOT; t++) pilot_d[t] = simulate_D(t & 1, amp, sigma);
    double scale = batch_scale(pilot_d, PILOT);

    for (long t = 0; t < trials; t++) {
        int bit = (int)(t & 1);
        double D = simulate_D(bit, amp, sigma) / scale;
        int byte = (int)lround(128.0 + D * 20.0); /* fixed post-normalization
            spread - D/scale is O(1) by construction, so a single spread
            constant (unlike the first attempt's per-amplitude scale) is
            appropriate here: normalization already removed the amplitude
            dependence, this just maps the normalized unit onto the byte
            range. */
        if (byte < 0) byte = 0;
        if (byte > 255) byte = 255;
        if (bit == 0) g_hist0[byte]++; else g_hist1[byte]++;
    }
}

int main(int argc, char **argv)
{
    init_twiddles();
    double sigma = 0.10;
    long trials_per_amp = (argc > 1) ? atol(argv[1]) : 30000;

    /* Train across a wide span of amplitudes - roughly -25 to +10 dB in
     * the 2500 Hz reference sense - so the pooled table reflects
     * "typical normalized statistics" rather than one operating point. */
    double amps[] = { 0.015, 0.025, 0.04, 0.06, 0.09, 0.13, 0.19, 0.28 };
    int n_amps = (int)(sizeof(amps) / sizeof(amps[0]));
    memset(g_hist0, 0, sizeof(g_hist0));
    memset(g_hist1, 0, sizeof(g_hist1));
    for (int a = 0; a < n_amps; a++) {
        fprintf(stderr, "training at amp=%.3f (%ld trials)...\n", amps[a], trials_per_amp);
        train_at_amplitude(amps[a], sigma, trials_per_amp);
    }

    int mettab[2][NBYTE];
    double METRIC_SCALE = 4.0;
    for (int b = 0; b < NBYTE; b++) {
        double tot0 = g_hist0[b] + 1.0, tot1 = g_hist1[b] + 1.0;
        double avg = (tot0 + tot1) / 2.0;
        mettab[0][b] = (int)lround(METRIC_SCALE * log(tot0 / avg));
        mettab[1][b] = (int)lround(METRIC_SCALE * log(tot1 / avg));
    }

    printf("/* Generated by test/wspr_metric_sim.c (per-capture-normalized,\n");
    printf(" * pooled across %d amplitudes x %ld trials each) - own\n", n_amps, trials_per_amp);
    printf(" * simulation, not copied from any published table. */\n");
    printf("static const int kSoftMetric[2][256] = {\n");
    for (int bit = 0; bit < 2; bit++) {
        printf("  {");
        for (int b = 0; b < NBYTE; b++) {
            printf("%d%s", mettab[bit][b], (b < NBYTE - 1) ? "," : "");
            if (b % 16 == 15) printf("\n   ");
        }
        printf("},\n");
    }
    printf("};\n");

    /* Self-check: for each of a WIDE range of test amplitudes, simulate
     * all 162 symbols, estimate the normalization scale from THAT
     * CAPTURE'S OWN 162 values (not a separate pilot - this is exactly
     * what main/wspr_decode.c would have to do on a real candidate), then
     * decode. */
    fprintf(stderr, "\nself-check across a wide SNR sweep (per-capture normalization):\n");
    wspr_msg_bytes_t msg;
    wspr_pack_message("W5BIT", "EL09", 17, &msg);
    uint8_t raw[WSPR_NSYM];
    wspr_convolve_encode(&msg, raw);

    int n_ok = 0, n_tested = 0;
    for (double test_amp = 0.30; test_amp >= 0.0004; test_amp *= 0.78) {
        double d[WSPR_NSYM];
        for (int i = 0; i < WSPR_NSYM; i++) d[i] = simulate_D(raw[i], test_amp, sigma);
        double scale = batch_scale(d, WSPR_NSYM);

        uint8_t soft[WSPR_NSYM];
        for (int i = 0; i < WSPR_NSYM; i++) {
            int byte = (int)lround(128.0 + (d[i] / scale) * 20.0);
            if (byte < 0) byte = 0;
            if (byte > 255) byte = 255;
            soft[i] = (uint8_t)byte;
        }
        wspr_msg_bytes_t decoded;
        unsigned int metric = 0, cycles = 0;
        int ok = wspr_fano_decode(soft, mettab, 2, 20000, &decoded, &metric, &cycles);
        int correct = ok && memcmp(msg.dat, decoded.dat, 7) == 0;
        double snr_db = 10.0 * log10((test_amp * test_amp / 2.0)
                                       / ((sigma * sigma / (FS / 2.0)) * 2500.0));
        fprintf(stderr, "  amp=%.4f SNR(2500Hzref)=%6.1fdB correct=%d cycles=%u\n",
                test_amp, snr_db, correct, cycles);
        n_tested++;
        if (correct) n_ok++;
    }
    fprintf(stderr, "\n%d/%d correct across the sweep\n", n_ok, n_tested);
    return 0;
}
