// webaudio_codec_harness.c - measures the browser audio wire format on the host.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main -I main/util -o test/webaudio_codec_harness.exe test/webaudio_codec_harness.c main/util/webaudio_codec.c -lm
//   ./test/webaudio_codec_harness.exe
//
// WHY THIS EXISTS
//   Both halves of this format fail QUIETLY. Neither shows up as an error
//   anywhere; both arrive as "the web audio sounds wrong" from somebody three
//   time zones away, with no instrument on either end.
//
//     * a mu-law encoder that is wrong in ONE exponent band still produces
//       audible speech. It sounds like a faint rasp on loud signals only, and
//       it is indistinguishable from a noisy band.
//     * an anti-alias filter that is too weak folds everything above 6 kHz
//       back into the passband. On AM at a 20 kHz width that is a real signal
//       arriving at the wrong pitch, which sounds like interference, not like
//       a filter fault - so it gets reported as a receiver problem.
//
// WHAT IT MEASURES
//   1. mu-law against the Sun g711.c reference, for all 65,536 inputs.
//   2. mu-law round-trip error, worst case and at small amplitudes.
//   3. the decimator's passband flatness, 100 Hz .. 4 kHz.
//   4. its attenuation of everything that ALIASES: 7 kHz .. 20 kHz input,
//      which folds back into the 12 kHz stream at the wrong pitch.
//   5. DC gain, which must be unity or the browser and the speaker disagree
//      about how loud the band is.
//
// WHAT IT DOES NOT MEASURE
//   Anything about the link: frame loss, jitter, or whether the AudioWorklet
//   keeps up. Those need a browser and a radio.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webaudio_codec.h"

static int failures = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
           detail ? " - " : "", detail ? detail : "");
    if (!ok) failures++;
}

/* ---- 1. mu-law against the reference ------------------------------------ */

/* Sun Microsystems g711.c linear2ulaw(), transcribed. The 14-bit form: it
   shifts the 16-bit input down by 2 and clips at 8159. If our 16-bit-form
   encoder disagrees with this for even one input, one of the two is wrong. */
static unsigned char ref_linear2ulaw(int pcm_val)
{
    static const int seg_uend[8] = { 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF };
    int mask, seg;
    unsigned char uval;

    pcm_val = pcm_val >> 2;
    if (pcm_val < 0) { pcm_val = -pcm_val; mask = 0x7F; }
    else             { mask = 0xFF; }
    if (pcm_val > 8159) pcm_val = 8159;
    pcm_val += (0x84 >> 2);

    for (seg = 0; seg < 8; seg++) if (pcm_val <= seg_uend[seg]) break;
    if (seg >= 8) return (unsigned char)(0x7F ^ mask);
    uval = (unsigned char)((seg << 4) | ((pcm_val >> (seg + 1)) & 0xF));
    return (unsigned char)(uval ^ mask);
}

static void test_mulaw_vs_reference(void)
{
    printf("1. mu-law vs the Sun g711.c reference, all 65,536 inputs\n");
    long diffs = 0;
    int  first_in = 0, first_ours = 0, first_ref = 0;
    for (int v = -32768; v <= 32767; v++) {
        unsigned char ours = webaud_mulaw_encode((int16_t)v);
        unsigned char ref  = ref_linear2ulaw(v);
        if (ours != ref) {
            if (!diffs) { first_in = v; first_ours = ours; first_ref = ref; }
            diffs++;
        }
    }
    char d[128];
    if (diffs)
        snprintf(d, sizeof(d), "%ld disagreements, first at pcm=%d (ours 0x%02X, ref 0x%02X)",
                 diffs, first_in, first_ours, first_ref);
    else
        snprintf(d, sizeof(d), "0 disagreements");
    check(diffs == 0, "every input encodes to the reference byte", d);
}

static void test_mulaw_roundtrip(void)
{
    printf("2. mu-law round trip - SNR against the signal level\n");
    /* ⛔ NOT AN LSB COUNT. The first version of this test asserted a worst-case
       absolute error and the thresholds were picked by me, which means they
       measured nothing: mu-law's absolute error GROWS with amplitude by
       design, so any number large enough to pass at full scale is far too
       loose to catch a fault down where a weak signal lives.

       What the format actually promises is a roughly CONSTANT signal-to-noise
       ratio over its whole range - that is the point of companding - so that
       is what gets measured, at the levels a receiver really produces. 30 dB
       is the floor: below that the quantisation is audible against a quiet
       band. */
    const double levels_dbfs[] = { 0.0, -6.0, -12.0, -20.0, -30.0, -40.0 };
    double worst_snr = 999.0;
    double worst_at  = 0.0;
    for (size_t i = 0; i < sizeof(levels_dbfs) / sizeof(levels_dbfs[0]); i++) {
        double amp = 32767.0 * pow(10.0, levels_dbfs[i] / 20.0);
        double sig = 0.0, err = 0.0;
        for (int n = 0; n < 48000; n++) {
            double x = amp * sin(2.0 * M_PI * 1000.0 * (double)n / 12000.0);
            int16_t q = (int16_t)lrint(x);
            int16_t back = webaud_mulaw_decode(webaud_mulaw_encode(q));
            double e = (double)back - (double)q;
            sig += (double)q * (double)q;
            err += e * e;
        }
        double snr = 10.0 * log10(sig / (err > 0.0 ? err : 1e-9));
        printf("       %+5.0f dBFS  SNR %5.1f dB\n", levels_dbfs[i], snr);
        if (snr < worst_snr) { worst_snr = snr; worst_at = levels_dbfs[i]; }
    }
    char d[128];
    snprintf(d, sizeof(d), "worst %.1f dB at %+.0f dBFS", worst_snr, worst_at);
    check(worst_snr >= 30.0, "at least 30 dB SNR at every level a receiver produces", d);
}

/* ---- 3-5. the decimator -------------------------------------------------- */

/* Drive a sine through the decimator and return the output's RMS amplitude
   relative to the input's, in dB. The input rate is the 48 kHz s_out stream. */
static double response_db(int f_hz)
{
    webaud_decim_t st;
    webaud_decim_init(&st, 48000);

    const int FS = 48000;
    const int N  = FS;              /* 1 s - long enough that the 31-tap
                                       transient is irrelevant to the RMS */
    double sumsq = 0.0;
    long   n_out = 0;
    for (int i = 0; i < N; i++) {
        double in = 16000.0 * sin(2.0 * M_PI * (double)f_hz * (double)i / (double)FS);
        int16_t out;
        if (!webaud_decim_push(&st, (float)in, &out)) continue;
        if (i < FS / 10) continue;  /* skip the first 100 ms of settling */
        sumsq += (double)out * (double)out;
        n_out++;
    }
    if (!n_out) return -999.0;
    double rms_out = sqrt(sumsq / (double)n_out);
    double rms_in  = 16000.0 / sqrt(2.0);
    return 20.0 * log10(rms_out / rms_in);
}

static void test_passband(void)
{
    printf("3. passband, 100 Hz .. 4 kHz (must be flat - this is the audio)\n");
    const int f[] = { 100, 300, 700, 1000, 2000, 3000, 4000 };
    double lo = 99.0, hi = -99.0;
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        double db = response_db(f[i]);
        printf("       %5d Hz  %+6.2f dB\n", f[i], db);
        if (db < lo) lo = db;
        if (db > hi) hi = db;
    }
    char d[128];
    snprintf(d, sizeof(d), "ripple %.2f dB (%.2f .. %.2f)", hi - lo, lo, hi);
    /* 0.6 dB, not the 1.5 the audio would tolerate: the measured figure is
       0.26 dB, so this is a REGRESSION guard. A threshold set where the
       listener stops caring would have passed the 31-tap filter that this
       test was written to catch. */
    check(hi - lo <= 0.6, "under 0.6 dB of ripple across the voice band", d);
    check(hi <= 0.5, "no passband gain", NULL);
}

static void test_alias_stopband(void)
{
    printf("4. the aliasing band - 7 kHz .. 20 kHz folds INTO the 12 kHz stream\n");
    /* THIS IS THE TEST THAT MATTERS. Anything surviving above 6 kHz does not
       disappear, it REAPPEARS at the wrong pitch. AM at the 20 kHz width is
       the case that can actually put content up here. */
    const int f[] = { 7000, 8000, 9000, 10000, 12000, 16000, 20000 };
    double worst = -999.0;
    int    worst_f = 0;
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        double db = response_db(f[i]);
        int fold = abs(((f[i] + 6000) % 12000) - 6000);
        printf("       %5d Hz  %+6.2f dB  (folds to %d Hz)\n", f[i], db, fold);
        if (db > worst) { worst = db; worst_f = f[i]; }
    }
    char d[128];
    snprintf(d, sizeof(d), "worst %.1f dB at %d Hz", worst, worst_f);
    /* Same reasoning as the ripple threshold: -30 dB is where it starts to
       be audible, -45 is a guard around the -54.9 dB measured, so a filter
       quietly shortened again fails here instead of on somebody's band. */
    check(worst <= -45.0, "everything that aliases is at least 45 dB down", d);
}

static void test_dc_gain(void)
{
    printf("5. DC gain (the browser and the speaker must agree on level)\n");
    webaud_decim_t st;
    webaud_decim_init(&st, 48000);
    int16_t out = 0, last = 0;
    for (int i = 0; i < 4800; i++) if (webaud_decim_push(&st, 10000.0f, &out)) last = out;
    char d[64];
    snprintf(d, sizeof(d), "10000 in, %d out", last);
    check(abs(last - 10000) <= 20, "unity at DC", d);
}

int main(void)
{
    printf("webaudio_codec harness\n\n");
    test_mulaw_vs_reference();
    test_mulaw_roundtrip();
    test_passband();
    test_alias_stopband();
    test_dc_gain();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
