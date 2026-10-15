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
/* ⛔ NOT COPIES. These used to be literals here with a comment saying they
 * matched rx_audio.h. On 2026-10-15 AM moved to D=2 and the ceiling to 10000,
 * and this harness went on measuring D=4 and printing "ceiling 4800 Hz" -
 * passing every check, describing a chain that no longer existed. rx_audio.h
 * needs only stdbool and stdint, so it can simply be read. */
#include "rx_audio.h"
#define RX_DECIM_D_NARROW  RX_AUDIO_DECIM_D_NARROW   // CW / SSB
#define RX_DECIM_D_AM      RX_AUDIO_DECIM_D_AM       // AM
#define FIR_DECIM_LEN      63
#define FIR_LEN            255           // stage-2 taps
#define AM_HALF_BW_HZ      RX_AUDIO_AM_HALF_BW_HZ      // the DEFAULT
#define AM_MAX_HALF_BW_HZ  RX_AUDIO_AM_HALF_BW_MAX_HZ  // what the chain can carry
/* The detector runs at the AM internal rate. */
#define FS_DEC             ((float)DSP_SAMPLE_RATE_HZ / (float)RX_DECIM_D_AM)

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

static void build_decimator_copy(float *c, int decim_d)
{
    float cutoff_norm = 0.45f / (float)decim_d;
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
    /* ⛔ THE COLUMN HEADINGS ARE BUILT FROM THE REAL FACTORS. They were
     * literal "D=8" / "D=4" strings, so when AM moved to D=2 this harness
     * printed a column headed D=4 that held D=2 numbers - a report wrong in
     * the one field a reader uses to interpret every other number in it. */
    char h_narrow[24], h_am[24];
    snprintf(h_narrow, sizeof(h_narrow), "D=%d (CW/SSB)", RX_DECIM_D_NARROW);
    snprintf(h_am,     sizeof(h_am),     "D=%d (AM)",     RX_DECIM_D_AM);
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
        /* ⚠ THIS SWEEP USED TO STOP AT 2900 Hz, which was fine while
           AM_MAX_HALF_BW_HZ was 4800 only because nothing ever asked for the
           top of the band. It now reaches 5400, past the 5000 Hz ceiling, so
           "flat to AM_MAX_HALF_BW_HZ" is a measurement across the WHOLE
           range the picker can select rather than across the part somebody
           happened to list. */
        const float f[] = {50, 100, 200, 300, 1000, 2000, 2400, 2700, 2900,
                           3500, 4000, 4500, 4800, 5000, 5400};
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
    printf("\n4. Stage-1 decimator skirt, BOTH factors (the real bandwidth limit)\n");
    {
        float c8[FIR_DECIM_LEN], c4[FIR_DECIM_LEN], c2[FIR_DECIM_LEN];
        build_decimator_copy(c8, RX_DECIM_D_NARROW);
        build_decimator_copy(c4, RX_DECIM_D_AM);
        /* ✅ D=2 IS WHAT AM RUNS AT, as of 2026-10-15. This column was the
           feasibility study and is now the regression: if it stops being flat
           to 10000 Hz, the 20.0 kHz width is silently rolling off.
           The AM list reaches 10.0 kHz (the HF legal maximum for amateur AM)
           and 20.0 kHz (a broadcaster's full audio, for SWL): half-bandwidths
           of 5000 and 10000 Hz. D=4 carried neither - it is already -2.40 dB
           at 5000, which is why the move to D=2 happened at all.
           Measure before writing any of it. That order is the whole lesson of
           2026-10-15, when the constant was raised first. */
        build_decimator_copy(c2, 4);   /* the factor AM used until 2026-10-15 */
        const float f[] = {1000, 2000, 2400, 2700, 3000, 4000, 4500, 4800,
                           5000, 5200, 5400, 6000, 7000,
                           8000, 9000, 10000, 10500, 11000, 12000};
        float at_max = 0.0f;
        printf("     %8s  %13s  %12s  %12s\n",
               "audio", h_narrow, h_am, "D=4 (the old AM)");
        for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
            float db8 = decim_mag_db(c8, f[i]);
            float db4 = decim_mag_db(c4, f[i]);
            float db2 = decim_mag_db(c2, f[i]);
            printf("     %6.0f Hz  %+10.2f dB  %+9.2f dB  %+9.2f dB%s\n",
                   (double)f[i], (double)db8, (double)db4, (double)db2,
                   (int)f[i] == AM_MAX_HALF_BW_HZ ? "   <- AM_MAX_HALF_BW_HZ" : "");
            if ((int)f[i] == AM_MAX_HALF_BW_HZ) at_max = db4;
        }
        /* A broadcast station transmits ~4.5 kHz of audio - carrying it is the
         * whole reason AM runs at its own D, so say whether it does. */
        float at_bc = decim_mag_db(c4, 4500.0f);
        snprintf(d, sizeof(d), "D=%d is %.2f dB at 4500 Hz, a broadcaster's top end",
                 RX_DECIM_D_AM,
                 (double)at_bc);
        check(at_bc > -3.0f, "the full broadcast audio fits at the AM D", d);
        snprintf(d, sizeof(d), "the AM decimator is %.2f dB at %d Hz",
                 (double)at_max, AM_MAX_HALF_BW_HZ);
        // The point of the cap: at AM_MAX_HALF_BW_HZ the chain must still be
        // carrying the audio, not throwing it away. -6 dB is the limit of what
        // is worth calling passband.
        check(at_max > -6.0f, "AM_MAX_HALF_BW_HZ is inside the AM chain's passband", d);
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

    // ---- 6. THE CW REGRESSION GUARD --------------------------------------
    // Stage 2 is FIR_LEN taps at the DECIMATED rate, so its transition band is
    // 3.3*fs_dec/FIR_LEN: 78 Hz at D=8, 155 Hz at D=4. That 78 Hz is the entire
    // reason the chain has two stages - a wider transition let nearby FT8 tones
    // bleed through a nominal 150 Hz CW filter, on the air 2026-09-04 (see
    // rx_audio.c's FIR_LEN comment). CW filters go down to 50 Hz. THIS is what
    // makes D per-mode instead of simply 4 everywhere, so it gets a guard.
    printf("\n6. Stage-2 transition band (why D stays 8 for CW)\n");
    {
        float t8 = 3.3f * ((float)DSP_SAMPLE_RATE_HZ / RX_DECIM_D_NARROW) / (float)FIR_LEN;
        float t4 = 3.3f * ((float)DSP_SAMPLE_RATE_HZ / RX_DECIM_D_AM)     / (float)FIR_LEN;
        printf("     %-16s: %5.1f Hz\n", h_narrow, (double)t8);
        printf("     %-16s: %5.1f Hz   (fine under a %d Hz filter, not under 50 Hz)\n",
               h_am, (double)t4, AM_MAX_HALF_BW_HZ);
        snprintf(d, sizeof(d), "CW transition is %.1f Hz at D=8", (double)t8);
        check(t8 < 80.0f, "CW keeps the 78 Hz transition it was tuned for", d);

        /* ---- 7. THE DEFAULT AM WIDTH MUST CLEAR THE ADJACENT CHANNEL ----
         * Shortwave broadcast channels are spaced 5 kHz, so the NEIGHBOUR's
         * carrier is 5000 Hz from the one being listened to. The chain can
         * carry 4800, but using it would put the filter edge on that
         * neighbour - which is why the default is 3000 and the ceiling is a
         * separate constant. This guards the DECISION, not the DSP: it is
         * arithmetic on the two constants that encode it. */
        const float adjacent_hz = 5000.0f;
        float edge = (float)AM_HALF_BW_HZ + t4;    /* filter edge + its skirt */
        printf("\n7. Adjacent channel (5 kHz spacing on shortwave)\n");
        printf("     default filter edge + transition : %.0f Hz\n", (double)edge);
        printf("     adjacent carrier                 : %.0f Hz\n", (double)adjacent_hz);
        printf("     margin                           : %.0f Hz\n", (double)(adjacent_hz - edge));
        snprintf(d, sizeof(d), "default edge %.0f Hz vs neighbour at %.0f Hz",
                 (double)edge, (double)adjacent_hz);
        check(edge < adjacent_hz - 1000.0f,
              "the default AM width leaves >1 kHz to the next channel", d);
        snprintf(d, sizeof(d), "ceiling %d Hz would sit %.0f Hz from the neighbour",
                 AM_MAX_HALF_BW_HZ, (double)(adjacent_hz - ((float)AM_MAX_HALF_BW_HZ + t4)));
        check(AM_MAX_HALF_BW_HZ > AM_HALF_BW_HZ,
              "the ceiling is a SEPARATE constant from the default", d);
    }

    printf("\n%s\n", g_fail ? "FAILURES PRESENT" : "all checks passed");
    return g_fail ? 1 : 0;
}
