/* Synthetic multi-signal WSPR test: encode known messages, synthesize their
 * actual 4-FSK audio (continuous-phase, matching the real transmitted
 * waveform's tone structure), mix several together with AWGN into one
 * 120 s capture, and run the REAL audio-domain decoder
 * (main/wspr_decode.c) against it - same pipeline test/wspr_decode_harness.c
 * runs against the real WAV, but here the ground truth is exact because we
 * built the signal ourselves. Same precedent as this project's FT8 self-test
 * (synth_gfsk_heap() in ft8_test.c): synthesize real audio, decode it with
 * the real pipeline, don't just re-test the bit-level codec.
 *
 * This is NOT a replacement for testing against real captured signals
 * (test/wspr_decode_harness.c already does that, against WSJT's own
 * official WSPR sample) - it answers a different, complementary question:
 * does the coarse-detection + start-time-search machinery correctly
 * separate MULTIPLE simultaneous signals with KNOWN parameters, and where
 * is this hard-decision decoder's sensitivity limit? Both were open
 * questions in docs/wspr-phase1-status.md.
 *
 * Build:
 *   gcc -O2 -Wall -I main -I components/ft8_lib \
 *       -o wspr_synth_harness test/wspr_synth_harness.c \
 *       main/wspr_proto.c main/wspr_fano.c main/wspr_decode.c \
 *       main/wspr_subtract.c \
 *       components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c \
 *       -lm && ./wspr_synth_harness
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wspr_proto.h"
#include "wspr_fano.h"
#include "wspr_decode.h"

static int g_fail = 0;

/* Simple PRNG (xorshift) so runs are deterministic/reproducible - no
 * dependency on libc rand()'s implementation-defined quality. */
static uint32_t g_rng_state = 0xC0FFEE01u;
static double rng_gaussian(void)
{
    /* Box-Muller, using the xorshift PRNG for both uniforms. */
    uint32_t s = g_rng_state;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    double u1 = (s % 1000000 + 1) / 1000001.0;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    double u2 = (s % 1000000 + 1) / 1000001.0;
    g_rng_state = s;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

#define CAPTURE_SECONDS 120.0
#define CAPTURE_SAMPLES ((long)(CAPTURE_SECONDS * WSPR_SAMPLE_RATE_HZ))
#define TONE_SPACING (WSPR_SAMPLE_RATE_HZ / WSPR_SYM_LEN_SAMPLES)

/* Continuous-phase 4-FSK synthesis of one WSPR transmission, added into
 * `buf` (a float accumulation buffer, CAPTURE_SAMPLES long) starting at
 * `start_sample`, at the given center frequency and linear amplitude. */
static int synth_signal(float *buf, long buf_n, const char *call,
                         const char *grid, int dbm, double f0_hz,
                         long start_sample, double amplitude)
{
    wspr_msg_bytes_t msg;
    if (!wspr_pack_message(call, grid, dbm, &msg)) return 0;
    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM], tones[WSPR_NSYM];
    wspr_convolve_encode(&msg, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, tones);

    double phase = 0.0;
    for (int sym = 0; sym < WSPR_NSYM; sym++) {
        double freq = f0_hz + tones[sym] * TONE_SPACING;
        double dphi = 2.0 * M_PI * freq / WSPR_SAMPLE_RATE_HZ;
        for (int n = 0; n < WSPR_SYM_LEN_SAMPLES; n++) {
            long idx = start_sample + (long)sym * WSPR_SYM_LEN_SAMPLES + n;
            if (idx >= 0 && idx < buf_n) buf[idx] += (float)(amplitude * sin(phase));
            phase += dphi;
            if (phase > 2 * M_PI) phase -= 2 * M_PI;
        }
    }
    return 1;
}

static void add_noise(float *buf, long n, double sigma)
{
    for (long i = 0; i < n; i++) buf[i] += (float)(sigma * rng_gaussian());
}

static int16_t *to_int16(const float *buf, long n)
{
    int16_t *out = (int16_t *)malloc((size_t)n * sizeof(int16_t));
    for (long i = 0; i < n; i++) {
        double v = buf[i] * 32767.0;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        out[i] = (int16_t)v;
    }
    return out;
}

/* ---- Test 1: three simultaneous signals, known parameters ---- */

static void test_multi_signal(void)
{
    printf("-- 1. three simultaneous synthetic signals --\n");

    float *acc = (float *)calloc((size_t)CAPTURE_SAMPLES, sizeof(float));
    struct { const char *call, *grid; int dbm; double f0; double amp; } sigs[] = {
        { "K1ABC",  "FN20", 37, 1420.0, 0.30 },
        { "OZ1LAV", "JO45", 23, 1500.0, 0.15 },
        { "VE3XYZ", "EN00", 30, 1580.0, 0.20 },
    };
    long dt0 = 30000; /* arbitrary common start offset, well within slack */
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        int ok = synth_signal(acc, CAPTURE_SAMPLES, sigs[i].call, sigs[i].grid,
                               sigs[i].dbm, sigs[i].f0, dt0, sigs[i].amp);
        if (!ok) { printf("  FAIL  synth('%s') refused\n", sigs[i].call); g_fail++; }
    }
    add_noise(acc, CAPTURE_SAMPLES, 0.02); /* light noise - strong-signal regime */

    int16_t *samples = to_int16(acc, CAPTURE_SAMPLES);
    free(acc);

    wspr_freq_candidate_t cands[8];
    int ncand = wspr_find_candidates(samples, CAPTURE_SAMPLES, 1350.0, 1650.0, cands, 8);

    int found[3] = { 0, 0, 0 };
    for (int c = 0; c < ncand; c++) {
        wspr_decode_result_t r;
        wspr_decode_candidate(samples, CAPTURE_SAMPLES, cands[c].freq_hz, &r);
        if (!r.ok) continue;
        for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
            if (strcmp(r.callsign, sigs[i].call) == 0 && strcmp(r.grid, sigs[i].grid) == 0
                && r.power_dbm == sigs[i].dbm) {
                found[i] = 1;
            }
        }
    }
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        printf("  %s  %s/%s/%d recovered\n", found[i] ? "PASS" : "FAIL",
               sigs[i].call, sigs[i].grid, sigs[i].dbm);
        if (!found[i]) g_fail++;
    }
    free(samples);
}

/* ---- Test 2: sensitivity sweep - where does this decoder stop working? ---- */

/* Noise is i.i.d. Gaussian per sample at WSPR_SAMPLE_RATE_HZ, so its power
 * spreads uniformly over the Nyquist band [0, Fs/2] - N0 (power per Hz) =
 * sigma^2 / (Fs/2). Signal power (sine wave) = amp^2/2. SNR in WSJT-X's
 * standard 2500 Hz reference bandwidth = signal_power / (N0 * 2500) - the
 * unit the commonly-cited -28 dB / -31 dB WSPR sensitivity figures use, so
 * computing it this way makes this sweep directly comparable to published
 * numbers.
 *
 * A first version of this sweep instead reported a naive wideband
 * amp/sigma ratio labeled "dB SNR" and found no breaking point at all down
 * to -20 dB in that (wrong) unit - every point decoded identically. That
 * wasn't real sensitivity, it was a miscalibrated label: each symbol's
 * soft decision comes from an 8192-sample coherent DFT, which has real
 * processing gain (~10*log10(8192) =~ 39 dB) a wideband ratio doesn't
 * account for. The result below only means something once the unit
 * actually matches what it's compared against. */
static void test_sensitivity_sweep(void)
{
    printf("\n-- 2. single-signal sensitivity sweep (informational) --\n");
    printf("   (characterizes this hard-decision decoder's limit - not a pass/fail gate,\n");
    printf("    since there's no external, independently-published number for THIS decoder\n");
    printf("    to check against - see this function's comment for the SNR unit used)\n\n");

    long dt0 = 20000;
    double noise_sigma = 0.10;
    double n0 = (noise_sigma * noise_sigma) / (WSPR_SAMPLE_RATE_HZ / 2.0);
    for (double amp = 0.05; amp >= 0.0005; amp *= 0.75) {
        float *acc = (float *)calloc((size_t)CAPTURE_SAMPLES, sizeof(float));
        synth_signal(acc, CAPTURE_SAMPLES, "W5BIT", "EL09", 17, 1450.0, dt0, amp);
        add_noise(acc, CAPTURE_SAMPLES, noise_sigma);
        int16_t *samples = to_int16(acc, CAPTURE_SAMPLES);
        free(acc);

        double signal_power = amp * amp / 2.0;
        double noise_power_2500 = n0 * 2500.0;
        double snr_2500_db = 10.0 * log10(signal_power / noise_power_2500);

        wspr_decode_result_t r;
        wspr_decode_candidate(samples, CAPTURE_SAMPLES, 1450.0, &r);
        printf("  SNR(2500Hz ref)=%7.1f dB  ok=%d", snr_2500_db, r.ok);
        if (r.ok) printf("  call='%s' grid='%s' dbm=%d cycles=%u", r.callsign, r.grid, r.power_dbm, r.cycles);
        printf("\n");
        free(samples);
    }
}

int main(void)
{
    test_multi_signal();
    test_sensitivity_sweep();

    printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
