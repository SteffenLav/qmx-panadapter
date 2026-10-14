// am_demod_harness.c - measures the Tab5's AM demodulation on the host.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main -I main/audio -o test/am_demod_harness.exe test/am_demod_harness.c -lm
//   ./test/am_demod_harness.exe
//
// WHY THIS EXISTS
//   AM was silent on the Tab5 until 2026-10-14 and the fix is small - an
//   envelope detector on a complex baseband the chain already produced. Small
//   is not the same as safe. Every way it can go wrong is silent on the glass:
//
//     * leave the carrier's DC in and the AGC pegs on it, so the stronger the
//       station the quieter it gets. Nothing logs that; it just sounds broken.
//     * ask for more audio bandwidth than the chain can carry and the top of
//       the passband is the DECIMATOR'S SKIRT, not the station - it does not
//       fail, it just quietly rolls off and nobody knows where.
//     * start the DC tracker at zero and the first sample of every mode change
//       is the whole carrier at full scale: a thump in the headphones.
//
//   So the numbers get measured here before any of it reaches a radio.
//
// WHAT IT MEASURES
//   1. DC rejection on an UNMODULATED carrier - what is left must be silence.
//   2. The audio frequency response of the demodulator, 50 Hz .. 2900 Hz, so
//      AM_MAX_HALF_BW_HZ is a measurement rather than a guess.
//   3. Modulation-depth linearity: a 50% modulated carrier must come out at
//      half the amplitude of a 100% one, within a tolerance.
//   4. The settle thump: peak output in the first millisecond after a reset.
//
// ⛔ WHAT IT DOES NOT MEASURE
//   The two FIR stages are NOT in this harness - rx_audio.c builds them with
//   esp-dsp, which is not on the host. Stage 1's cutoff is reproduced from the
//   SAME formula below and clearly marked as a copy; if build_decimator()
//   changes, this harness does not notice, which is why the copy carries the
//   original's file and constant names. The demodulator itself is the shipped
//   code - am_demod.h is included, not reimplemented.
//
// ⛔ AND IT PROVES NOTHING ABOUT AUDIBLE QUALITY. A flat response and clean DC
//   rejection on a synthetic carrier is the floor, not the ceiling. No AM
//   station has been through this chain on hardware.

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "am_demod.h"

// ---- Constants mirrored from the firmware ---------------------------------
// ⚠ COPIES. The originals are in main/audio/rx_audio.c; this harness cannot
// include that file (esp-dsp, FreeRTOS). Names kept identical so a grep for
// either constant finds both places.
#define DSP_SAMPLE_RATE_HZ 48000
#define RX_DECIM_D         8
#define FIR_DECIM_LEN      63
#define AM_MAX_HALF_BW_HZ  2400          // == rx_audio.h RX_AUDIO_AM_HALF_BW_HZ (host copy: that header pulls in FreeRTOS)
#define FS_DEC             ((float)DSP_SAMPLE_RATE_HZ / (float)RX_DECIM_D)

static int g_fail = 0;
static void check(int ok, const char *what, const char *detail)
{
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) g_fail++;
}

// ---- Stage-1 decimator response (a COPY of build_decimator's design) ------
// Reproduces main/audio/rx_audio.c build_decimator() exactly: 63-tap Hamming
// windowed sinc, cutoff_norm = 0.45/RX_DECIM_D of the FULL rate, normalised
// to unity DC gain. Used only to report WHERE the chain's own ceiling is.
static float sinc_norm(float x)
{
    if (fabsf(x) < 1e-7f) return 1.0f;
    return sinf((float)M_PI * x) / ((float)M_PI * x);
}

static void build_decimator_copy(float *c)
{
    float cutoff_norm = 0.45f / (float)RX_DECIM_D;
    int   M    = FIR_DECIM_LEN - 1;
    float half = M / 2.0f;
    for (int n = 0; n < FIR_DECIM_LEN; n++) {
        float m  = (float)n - half;
        float lp = 2.0f * cutoff_norm * sinc_norm(2.0f * cutoff_norm * m);
        float w  = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)n / (float)M);
        c[n] = lp * w;
    }
    float sum = 0.0f;
    for (int n = 0; n < FIR_DECIM_LEN; n++) sum += c[n];
    if (fabsf(sum) > 1e-6f) for (int n = 0; n < FIR_DECIM_LEN; n++) c[n] /= sum;
}

// |H(f)| of that FIR at a frequency in Hz against the FULL 48 kHz rate.
static float decim_mag_db(const float *c, float f_hz)
{
    float w = 2.0f * (float)M_PI * f_hz / (float)DSP_SAMPLE_RATE_HZ;
    float re = 0.0f, im = 0.0f;
    for (int n = 0; n < FIR_DECIM_LEN; n++) {
        re += c[n] * cosf(w * (float)n);
        im -= c[n] * sinf(w * (float)n);
    }
    float m = sqrtf(re * re + im * im);
    return 20.0f * log10f(m < 1e-12f ? 1e-12f : m);
}

// ---- The demodulator under test -------------------------------------------
// Drive am_demod_step() with a synthetic complex baseband: a carrier at
// baseband DC (which is what rx_audio.c's NCO1 produces for AM, centre 0)
// amplitude-modulated at f_mod with depth m. Returns the RMS of the audio
// and, via out_peak, the largest excursion.
static float demod_rms(float f_mod_hz, float depth, int n,
                       float *out_peak, int skip)
{
    am_demod_t a;
    am_demod_reset(&a);
    float alpha = am_demod_alpha(FS_DEC);
    const float carrier = 10000.0f;     // same order as the int16 IQ the radio sends
    double acc = 0.0;
    int    used = 0;
    float  peak = 0.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / FS_DEC;
        float env = carrier * (1.0f + depth * sinf(2.0f * (float)M_PI * f_mod_hz * t));
        // A real AM signal at baseband DC is purely real, but the chain hands
        // the detector an arbitrary phase - rotate it so the test exercises
        // the magnitude rather than accidentally testing the real part.
        float ph = 0.7f;
        float re = env * cosf(ph), im = env * sinf(ph);
        float v = am_demod_step(&a, re, im, alpha);
        if (i >= skip) { acc += (double)v * (double)v; used++; }
        if (fabsf(v) > peak) peak = fabsf(v);
    }
    if (out_peak) *out_peak = peak;
    return used ? (float)sqrt(acc / (double)used) : 0.0f;
}

int main(void)
{
    char d[160];
    printf("AM demodulator harness - internal rate %.0f Hz, corner %.0f Hz\n\n",
           (double)FS_DEC, (double)AM_DC_CORNER_HZ);

    // ---- 1. DC rejection: an unmodulated carrier must come out silent -----
    printf("1. Carrier rejection (no modulation)\n");
    {
        float peak = 0.0f;
        // Skip the first 100 ms so the one-pole has settled; what remains is
        // the steady-state leakage, which is what an operator would hear as a
        // DC offset or a rumble.
        int n = (int)(FS_DEC * 0.5f);
        float rms = demod_rms(0.0f, 0.0f, n, &peak, (int)(FS_DEC * 0.1f));
        // The carrier is 10000; -80 dB of it is 1.0 LSB of int16.
        float db = 20.0f * log10f((rms < 1e-9f ? 1e-9f : rms) / 10000.0f);
        snprintf(d, sizeof(d), "steady-state residual %.1f dB below carrier", (double)db);
        check(db < -80.0f, "unmodulated carrier is silent", d);
    }

    // ---- 2. The settle thump ---------------------------------------------
    printf("\n2. Priming (the thump on entering AM)\n");
    {
        float peak = 0.0f;
        int n = (int)(FS_DEC * 0.001f);      // first millisecond only
        (void)demod_rms(0.0f, 0.0f, n, &peak, 0);
        snprintf(d, sizeof(d), "peak in first 1 ms = %.1f (carrier 10000)", (double)peak);
        // Without priming this is the full carrier, 10000. With it, ~0.
        check(peak < 100.0f, "first sample does not pass the carrier", d);
    }

    // ---- 3. Audio frequency response -------------------------------------
    // The reference is 1 kHz, comfortably inside the passband. Everything is
    // reported relative to it, which is what "flat" has to mean here.
    printf("\n3. Audio response of the detector (relative to 1 kHz)\n");
    {
        int n = (int)(FS_DEC * 1.0f);
        int skip = (int)(FS_DEC * 0.1f);
        float ref = demod_rms(1000.0f, 0.5f, n, NULL, skip);
        const float f[] = {50, 100, 200, 300, 1000, 2000, 2400, 2700, 2900};
        float worst_in_band = 0.0f;
        for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
            float r = demod_rms(f[i], 0.5f, n, NULL, skip);
            float db = 20.0f * log10f((r < 1e-9f ? 1e-9f : r) / ref);
            printf("     %6.0f Hz : %+6.2f dB%s\n", (double)f[i], (double)db,
                   f[i] <= AM_MAX_HALF_BW_HZ ? "" : "   (above AM_MAX_HALF_BW_HZ)");
            if (f[i] <= AM_MAX_HALF_BW_HZ && fabsf(db) > worst_in_band)
                worst_in_band = fabsf(db);
        }
        snprintf(d, sizeof(d), "worst deviation in band = %.2f dB", (double)worst_in_band);
        check(worst_in_band < 1.0f, "flat within 1 dB up to AM_MAX_HALF_BW_HZ", d);
    }

    // ---- 4. Where the CHAIN's ceiling actually is -------------------------
    // This is the number AM_MAX_HALF_BW_HZ is chosen from, and it comes from
    // stage 1, not from the detector.
    printf("\n4. Stage-1 decimator skirt (the real bandwidth limit)\n");
    {
        float c[FIR_DECIM_LEN];
        build_decimator_copy(c);
        const float f[] = {1000, 2000, 2400, 2700, 3000, 3500, 4000};
        float at_max = 0.0f;
        for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
            float db = decim_mag_db(c, f[i]);
            printf("     %6.0f Hz : %+6.2f dB\n", (double)f[i], (double)db);
            if ((int)f[i] == AM_MAX_HALF_BW_HZ) at_max = db;
        }
        snprintf(d, sizeof(d), "decimator is %.2f dB at %d Hz",
                 (double)at_max, AM_MAX_HALF_BW_HZ);
        // The point of the cap: at AM_MAX_HALF_BW_HZ the chain must still be
        // carrying the audio, not throwing it away. -6 dB is the limit of what
        // is worth calling passband.
        check(at_max > -6.0f, "AM_MAX_HALF_BW_HZ is inside the chain's passband", d);
    }

    // ---- 5. Modulation-depth linearity -----------------------------------
    printf("\n5. Modulation depth linearity\n");
    {
        int n = (int)(FS_DEC * 1.0f);
        int skip = (int)(FS_DEC * 0.1f);
        float r100 = demod_rms(1000.0f, 1.0f, n, NULL, skip);
        float r50  = demod_rms(1000.0f, 0.5f, n, NULL, skip);
        float r25  = demod_rms(1000.0f, 0.25f, n, NULL, skip);
        float e50 = fabsf(r50 / r100 - 0.5f);
        float e25 = fabsf(r25 / r100 - 0.25f);
        snprintf(d, sizeof(d), "50%%=%.4f (want 0.5), 25%%=%.4f (want 0.25)",
                 (double)(r50 / r100), (double)(r25 / r100));
        check(e50 < 0.01f && e25 < 0.01f, "output is linear in modulation depth", d);
    }

    printf("\n%s\n", g_fail ? "FAILURES PRESENT" : "all checks passed");
    return g_fail ? 1 : 0;
}
