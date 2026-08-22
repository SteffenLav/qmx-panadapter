/* Attempt to build a real soft-decision metric table via Monte Carlo
 * simulation of THIS decoder's own channel statistic - not copied from
 * anyone's published table (K9AN's metric_tables.c is GPL v3, see
 * docs/wspr-phase1-status.md's licensing section; this was meant to be
 * independent original work instead).
 *
 * RESULT: NEGATIVE, NOT SHIPPED. A single fixed-scale table, calibrated
 * at one amplitude (amp=0.06 here), decodes correctly only in a narrow
 * band around that calibration point and FAILS outside it - including at
 * STRONG signal (tested to +6.8 dB SNR, where even the crude hard-decision
 * table in wspr_fano.c works trivially). Confirmed with two different
 * scale choices and up to 250,000 trials each - not a statistics problem,
 * a structural one: D scales roughly with amplitude^2 (it's a power
 * difference), so a signal several times stronger than the calibration
 * point saturates nearly every symbol to byte 0 or 255, and the noisy,
 * low-sample-count entries the calibration run happened to produce at
 * those extreme bins get used as if they were reliable - each printed
 * table run of 100k trials showed long runs of near-empty (single-digit
 * count) tail bins.
 *
 * This is exactly why real decoders like wsprd don't use one fixed table:
 * K9AN's metric_tables.c ships FOUR tables for different Es/No operating
 * points and selects/blends between them. The real fix here would be
 * either the same (multiple tables, chosen by an estimated SNR) or
 * per-capture normalization (derive the quantization scale from the
 * ACTUAL candidate signal's own measured |D| distribution, not a
 * pre-baked constant) - genuine follow-up work, not done here. The
 * existing 2-level hard-decision table in wspr_fano.c stays the shipped
 * default: it has no calibration-fragility at all and is already measured
 * at -22.7 dB SNR sensitivity (test/wspr_synth_harness.c), which this
 * attempt did not beat.
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
 * Prints a C array literal (the built table) to stdout, and reports its
 * measured effect on the sensitivity sweep to stderr - kept as a tool for
 * revisiting this with per-capture normalization, not as something to
 * link into main/.
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

/* Precomputed twiddle tables for both correlator frequencies, built once
 * at startup - simulate_D() is called hundreds of thousands of times and
 * an earlier version of this function called cos()/sin() inside the
 * per-sample loop, which made even a 100k-trial run impractically slow
 * (same mistake, same fix, as main/wspr_decode.c's start-time search -
 * see docs/wspr-phase1-status.md). Reusing sin(a+b) = sin(a)cos(b) +
 * cos(a)sin(b) means the per-trial random phase only needs ONE sin/cos
 * call total, with the rest of the 8192-sample loop being pure
 * table-lookup multiply-adds. */
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
 * P(tone0), where tone1 is one tone-spacing above tone0 - matching what
 * the real decoder compares - `sent_bit` selects which tone actually
 * carries the signal. `amp` is the tone's peak amplitude, `sigma` the
 * AWGN per-sample stddev, both in the same normalized units
 * main/wspr_decode.c's int16 samples/32768.0 use. */
static double simulate_D(int sent_bit, double amp, double sigma)
{
    double re0 = 0, im0 = 0, re1 = 0, im1 = 0;
    double phase0 = rng_uniform() * 2.0 * M_PI; /* random start phase - a
        real capture's symbol boundary doesn't line up with a cosine's
        zero-crossing, and averaging over phase is exactly what a
        realistic simulation must do rather than assuming phase=0. */
    double cp = cos(phase0), sp = sin(phase0);
    const float *sig_cos = (sent_bit == 0) ? g_cos0 : g_cos1;
    const float *sig_sin = (sent_bit == 0) ? g_sin0 : g_sin1;
    for (int n = 0; n < SYM_LEN; n++) {
        /* sin(phase0 + w*n) = sin(phase0)cos(w*n) + cos(phase0)sin(w*n) */
        double sig = amp * (sp * sig_cos[n] + cp * sig_sin[n]);
        double x = sig + sigma * rng_gaussian();
        re0 += x * g_cos0[n]; im0 -= x * g_sin0[n];
        re1 += x * g_cos1[n]; im1 -= x * g_sin1[n];
    }
    double p0 = re0 * re0 + im0 * im0;
    double p1 = re1 * re1 + im1 * im1;
    return p1 - p0;
}

#define NBYTE 256

int main(int argc, char **argv)
{
    init_twiddles();

    /* Reference operating point: chosen near this decoder's own
     * hard-decision sensitivity limit (~-22.7 dB SNR in the 2500 Hz
     * reference bandwidth, measured in test/wspr_synth_harness.c) - the
     * regime a soft metric actually needs to help in, not an arbitrary
     * "strong signal" point where hard-decision already works fine. */
    double amp = 0.06;
    double sigma = 0.10;
    long trials = (argc > 1) ? atol(argv[1]) : 200000;

    fprintf(stderr, "simulating %ld trials at amp=%.3f sigma=%.3f...\n", trials, amp, sigma);

    /* Empirical D-scale: run a small pilot batch first to see the typical
     * magnitude of D, so the byte quantization actually spans the
     * meaningful range instead of guessing a scale constant. */
    double abs_sum = 0;
    int pilot = 2000;
    for (int t = 0; t < pilot; t++) {
        abs_sum += fabs(simulate_D(t & 1, amp, sigma));
    }
    double mean_abs_D = abs_sum / pilot;
    double scale = mean_abs_D / 12.0; /* wider dynamic range than the first
        attempt's /40 - that left most of the byte range as near-empty,
        unreliable-count bins (confirmed by inspecting the printed table:
        long runs of 0s and single-digit counts at the tails), which
        starved the Fano decoder of real information outside a narrow band
        around the calibration amplitude. */
    fprintf(stderr, "pilot mean|D|=%.4f, quantization scale=%.6f\n", mean_abs_D, scale);

    long hist0[NBYTE] = { 0 }, hist1[NBYTE] = { 0 };
    for (long t = 0; t < trials; t++) {
        int bit = (int)(t & 1);
        double D = simulate_D(bit, amp, sigma);
        int byte = (int)lround(128.0 + D / scale);
        if (byte < 0) byte = 0;
        if (byte > 255) byte = 255;
        if (bit == 0) hist0[byte]++; else hist1[byte]++;
    }

    /* Empirical log-likelihood-ratio metric, scaled to fit a reasonable
     * integer range (matching the magnitude the hard-decision table used,
     * roughly +-10, so the Fano threshold/delta tuning already validated
     * doesn't need re-deriving from scratch). Laplace-smoothed (+1) so an
     * unseen byte doesn't produce a -inf/log(0). */
    int mettab[2][NBYTE];
    double METRIC_SCALE = 4.0;
    for (int b = 0; b < NBYTE; b++) {
        double tot0 = hist0[b] + 1.0, tot1 = hist1[b] + 1.0;
        double avg = (tot0 + tot1) / 2.0;
        double m0 = METRIC_SCALE * log(tot0 / avg);
        double m1 = METRIC_SCALE * log(tot1 / avg);
        mettab[0][b] = (int)lround(m0);
        mettab[1][b] = (int)lround(m1);
    }

    printf("/* Generated by test/wspr_metric_sim.c - empirical log-likelihood\n");
    printf(" * metric from %ld Monte Carlo trials at amp=%.3f sigma=%.3f\n", trials, amp, sigma);
    printf(" * (own simulation of this decoder's own channel statistic - not\n");
    printf(" * copied from any published table). Quantization scale=%.6f */\n", scale);
    printf("static const int kSoftMetricScale_x1000 = %d;\n", (int)lround(scale * 1000));
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

    /* Quick self-check: does this table actually decode correctly at the
     * reference point and beyond? (uses the real wspr_fano_decode, real
     * encode/interleave - this IS the correctness check, the printed
     * table is only useful if this passes.) */
    fprintf(stderr, "\nself-check: encode/decode round trip using the new table:\n");
    wspr_msg_bytes_t msg;
    wspr_pack_message("W5BIT", "EL09", 17, &msg);
    /* wspr_fano_decode() consumes ENCODE-ORDER (pre-interleave) soft
     * values - the real receiver deinterleaves before decoding, but since
     * this simulation treats every symbol as an i.i.d. trial (no
     * position-dependent effects modeled), simulating directly against
     * wspr_convolve_encode()'s raw output and skipping interleave/
     * deinterleave entirely gives the identical result while testing
     * exactly what this tool cares about: the metric table's quality, not
     * the (already separately proven) interleave logic. */
    uint8_t raw[WSPR_NSYM];
    wspr_convolve_encode(&msg, raw);

    for (double test_amp = 0.20; test_amp >= 0.01; test_amp *= 0.8) {
        uint8_t soft[WSPR_NSYM];
        for (int i = 0; i < WSPR_NSYM; i++) {
            int data = raw[i];
            /* Simulate this symbol's D exactly as the real receiver would
             * see it, using the just-built table's own quantization. */
            double D = simulate_D(data, test_amp, sigma);
            int byte = (int)lround(128.0 + D / scale);
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
        fprintf(stderr, "  amp=%.4f SNR(2500Hzref)=%6.1fdB ok=%d correct=%d cycles=%u\n",
                test_amp, snr_db, ok, correct, cycles);
    }
    return 0;
}
