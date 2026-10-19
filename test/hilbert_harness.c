// hilbert_harness.c - measures the line-in quadrature synthesis on the host.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main -o test/hilbert_harness.exe test/hilbert_harness.c -lm
//   ./test/hilbert_harness.exe
//
// WHY THIS EXISTS
//   The 3.5 mm jack gives ONE channel. line_in.c synthesises the missing
//   quadrature channel with a Hilbert transform so the spectrum comes out
//   one-sided instead of mirrored. Every way that can go wrong is silent:
//
//     * a flipped sign puts the whole band on the NEGATIVE half - the display
//       looks empty on the side you are watching, and nothing logs anything;
//     * a mismatched delay on the I path degrades the cancellation smoothly,
//       so a mirror image fades up as a second, weaker trace;
//     * too few taps moves the filter's useless low-frequency edge UP into
//       the 300-3000 Hz band an HF receiver's audio actually occupies, which
//       shows as mirror images of exactly the signals you care about and
//       nothing else.
//
//   None of those is a crash, an error code, or a log line. They are all
//   "the jack input looks a bit odd on the waterfall", which is the kind of
//   report that arrives weeks later from a user. So the image rejection gets
//   MEASURED here, in dB, before any of it reaches hardware.
//
// WHAT IT MEASURES
//   For a tone at frequency f, build the analytic signal exactly as line_in.c
//   does - Q = FIR(x), I = x delayed by the group delay - and then compute
//   how much of it lands on the negative half of the spectrum. That ratio IS
//   the image rejection. A perfect transform gives -infinity dB; the useful
//   number is where it crosses a threshold a waterfall would show.
//
// WHAT IT CANNOT COVER
//   That esp-dsp's dsps_fir_f32 computes the same convolution this file does
//   (it does; the harness convolves directly rather than linking esp-dsp),
//   and that the codec delivers slot 3 at the scale assumed. Those need the
//   hardware.
//
// It includes the firmware's own line_in_hilbert.h, so the kernel measured
// here is the kernel that runs.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "audio/line_in_hilbert.h"

#define FS_HZ   48000.0
#define NSAMP   8192          /* ~170 ms: long enough that the FIR transient
                               * is a small fraction of what is measured */

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

static float h[LI_HILBERT_N];

/* Image rejection for a tone at f_hz, in dB (negative = good).
 *
 * Builds I and Q the way line_in.c does, then correlates the analytic signal
 * against e^(+jwt) and e^(-jwt). The wanted component sits on one, the mirror
 * on the other, and the ratio is the rejection. Done by direct correlation
 * rather than an FFT so there is no window or bin-leakage to argue about. */
static double image_rejection_db(double f_hz)
{
    static float x[NSAMP], q[NSAMP];
    const double w = 2.0 * M_PI * f_hz / FS_HZ;

    for (int n = 0; n < NSAMP; n++) x[n] = (float)cos(w * n);

    /* Q = h * x, the same convolution dsps_fir_f32 performs. */
    for (int n = 0; n < NSAMP; n++) {
        double acc = 0.0;
        for (int k = 0; k < LI_HILBERT_N; k++) {
            int idx = n - k;
            if (idx >= 0) acc += (double)h[k] * (double)x[idx];
        }
        q[n] = (float)acc;
    }

    /* Skip the FIR transient: nothing before the delay line is full is a
     * measurement of the filter, it is a measurement of the start-up. */
    const int skip = LI_HILBERT_N * 2;

    double pr = 0, pi = 0, nr = 0, ni = 0;
    for (int n = skip; n < NSAMP; n++) {
        /* I is x delayed by the group delay, so I and Q describe the same
         * instant. This line is the one the firmware's ring buffer does. */
        int d = n - LI_HILBERT_MID;
        if (d < 0) continue;
        double I = x[d], Q = q[n];
        double c = cos(w * n), s = sin(w * n);
        /* Project onto e^(-jwt) (the wanted, positive-frequency part) and
         * e^(+jwt) (the mirror). */
        pr += I * c + Q * s;   pi += Q * c - I * s;
        nr += I * c - Q * s;   ni += Q * c + I * s;
    }
    double wanted = sqrt(pr * pr + pi * pi);
    double image  = sqrt(nr * nr + ni * ni);
    if (wanted <= 0.0) return 0.0;
    if (image  <= 0.0) return -200.0;
    return 20.0 * log10(image / wanted);
}

// The kernel's own structure. These are not style points: an even tap that is
// not zero means the transform is not a Hilbert transform at all.
static void test_kernel_shape(void)
{
    CHECK(LI_HILBERT_N % 2 == 1, "the tap count %d is not odd", LI_HILBERT_N);
    CHECK(LI_HILBERT_MID == LI_HILBERT_N / 2, "the group delay is not the centre tap");

    for (int n = 0; n < LI_HILBERT_N; n++) {
        int k = n - LI_HILBERT_MID;
        if ((k % 2) == 0)
            CHECK(h[n] == 0.0f, "tap %d (k=%d) is %g, should be exactly 0", n, k, (double)h[n]);
    }

    // Odd symmetry about the centre: h[mid+k] == -h[mid-k]. A kernel that is
    // EVEN-symmetric is a lowpass, not a phase shifter, and it would produce
    // a Q identical to I - a 45-degree signal with no image rejection at all.
    for (int k = 1; k <= LI_HILBERT_MID; k++) {
        float a = h[LI_HILBERT_MID + k], b = h[LI_HILBERT_MID - k];
        CHECK(fabsf(a + b) < 1e-7f, "taps are not odd-symmetric at k=%d (%g vs %g)",
              k, (double)a, (double)b);
    }
}

// THE MEASUREMENT THAT MATTERS. Rejection across the band, and the assertion
// that it is good enough to be invisible on a waterfall where the whole
// displayed range is about 45 dB.
static void test_image_rejection(void)
{
    static const struct { double f; double worst_db; const char *what; } cases[] = {
        /* The FT8/JS8/WSPR audio band is the one that has to be right. */
        /* Thresholds are the MEASURED figures with ~4 dB of margin, not
         * aspirations. See the table in line_in_hilbert.h. 40 dB puts the
         * residual image at or below the floor of a ~45 dB waterfall. */
        {  200.0, -19.0, "200 Hz - below an SSB passband; degraded on purpose" },
        {  300.0, -39.0, "300 Hz - the low edge of an SSB passband" },
        {  500.0, -50.0, "500 Hz" },
        { 1000.0, -60.0, "1 kHz" },
        { 1500.0, -40.0, "1.5 kHz - the middle of the JS8/FT8 band" },
        { 2400.0, -40.0, "2.4 kHz - the high edge of an SSB passband" },
        { 3000.0, -40.0, "3 kHz" },
        { 6000.0, -40.0, "6 kHz" },
        {12000.0, -40.0, "12 kHz - a quarter of the sample rate" },
        {20000.0, -35.0, "20 kHz" },
    };
    printf("  image rejection (negative is good):\n");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double db = image_rejection_db(cases[i].f);
        printf("    %8.1f Hz : %7.1f dB   %s\n", cases[i].f, db, cases[i].what);
        CHECK(db <= cases[i].worst_db,
              "%s: rejection %.1f dB is worse than the %.1f dB this band needs",
              cases[i].what, db, cases[i].worst_db);
    }

    // The useless edges are expected to BE useless - assert that too, so a
    // future change that appears to improve DC is recognised as the sign
    // that something else broke rather than as a win.
    double dc = image_rejection_db(20.0);
    printf("    %8.1f Hz : %7.1f dB   20 Hz - expected to be poor, this is the filter's edge\n",
           20.0, dc);
    CHECK(dc > -40.0, "20 Hz rejection is %.1f dB - suspiciously good for a "
                      "Hilbert filter's low edge; check the measurement", dc);
}

// Sign convention: audio must land on the POSITIVE half of the spectrum,
// because ui.c reports dial_hz = 0 for a real source and the axis then reads
// audio frequency directly. Flip this and the display is empty where the
// operator is looking.
static void test_sign_convention(void)
{
    // image_rejection_db() returns the ratio of the NEGATIVE-frequency
    // component to the positive one. A strongly negative dB figure therefore
    // already means the energy went positive. Restate it as its own check so
    // the intent is recorded, not inferred from another test's threshold.
    double db = image_rejection_db(1500.0);
    CHECK(db < -20.0, "a 1.5 kHz tone put %.1f dB of its energy on the NEGATIVE "
                      "half - the Hilbert sign is inverted", db);
}

int main(void)
{
    printf("hilbert_harness (%d taps, delay %d samples, %.0f Hz)\n",
           LI_HILBERT_N, LI_HILBERT_MID, FS_HZ);
    li_build_hilbert(h);
    test_kernel_shape();
    test_image_rejection();
    test_sign_convention();
    if (g_fail == 0) printf("  all checks passed\n");
    else             printf("  %d CHECK(s) failed\n", g_fail);
    return g_fail ? 1 : 0;
}
