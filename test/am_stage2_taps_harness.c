/* Host sweep: how many stage-2 taps does AM actually need?
 *
 *   gcc -O2 -Wall -Wextra -o test/am_stage2_taps_harness test/am_stage2_taps_harness.c -lm
 *   ./test/am_stage2_taps_harness
 *
 * ⛔ WHY. rx_audio.c's FIR_LEN is 255 and its own comment says it is "shared
 * by both modes". The history above it shows what sized it: CW leaking content
 * 200-300 Hz off the dial into a nominal 150 Hz filter, confirmed on the air
 * 2026-09-04. That is CW's requirement. AM inherited the number and was never
 * shown to need it.
 *
 * It matters because stage 2 is NON-decimating and is the dominant per-sample
 * cost in the chain - 255 taps against stage 1's 63 - and AM runs it at D=4,
 * i.e. a 12 kHz internal rate, twice SSB's 6 kHz. Measured on bench dev
 * 2026-10-17: AM 6 kHz costs 10.2 ms a frame against USB's 3.7 ms and rails
 * the clock-drift resampler hundreds of times per 10 s. AM 4 kHz does not
 * rail. So the width is limited by this cost.
 *
 * ⚠ THIS MEASURES THE FILTER, NOT THE AUDIO. A transition width is not a
 * listening test. test/am_demod_harness.c deliberately does NOT cover the two
 * FIR stages ("the two FIR stages are NOT in this harness"), which is why this
 * file exists rather than an addition there.
 *
 * The design below is COPIED from build_narrow_fir() in rx_audio.c - Hamming
 * window, normalised sinc, unity DC gain. If that changes, this copy does not
 * notice; it carries the original's names so the next reader can check.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#define DSP_SAMPLE_RATE_HZ 48000

static double sinc_norm(double x)
{
    if (fabs(x) < 1e-9) return 1.0;
    return sin(M_PI * x) / (M_PI * x);
}

/* build_narrow_fir(), in double for the host. */
static void design(double *h, int len, int half_bw_hz, int decim_d)
{
    double fs   = (double)DSP_SAMPLE_RATE_HZ / (double)decim_d;
    double fc   = (double)half_bw_hz / fs;
    int    M    = len - 1;
    double half = M / 2.0;
    double sum  = 0.0;
    for (int n = 0; n < len; n++) {
        double m  = (double)n - half;
        double lp = 2.0 * fc * sinc_norm(2.0 * fc * m);
        double w  = 0.54 - 0.46 * cos(2.0 * M_PI * (double)n / (double)M);
        h[n] = lp * w;
        sum += h[n];
    }
    if (fabs(sum) > 1e-12) for (int n = 0; n < len; n++) h[n] /= sum;
}

/* |H(f)| in dB. */
static double mag_db(const double *h, int len, double f_hz, double fs)
{
    double re = 0.0, im = 0.0;
    double w = 2.0 * M_PI * f_hz / fs;
    for (int n = 0; n < len; n++) { re += h[n] * cos(w * n); im -= h[n] * sin(w * n); }
    double m = sqrt(re * re + im * im);
    return 20.0 * log10(m < 1e-12 ? 1e-12 : m);
}

/* Where the response first falls below `db`, scanning up from the passband. */
static double edge_at(const double *h, int len, double fs, double db, double from_hz)
{
    for (double f = from_hz; f < fs / 2.0; f += 1.0)
        if (mag_db(h, len, f, fs) < db) return f;
    return fs / 2.0;
}

/* Worst (highest) sidelobe above f_hz - the stopband floor that decides how
 * much of the neighbouring channel leaks in. */
static double stopband_db(const double *h, int len, double fs, double from_hz)
{
    double worst = -200.0;
    for (double f = from_hz; f < fs / 2.0; f += 2.0) {
        double m = mag_db(h, len, f, fs);
        if (m > worst) worst = m;
    }
    return worst;
}

static void sweep(const char *what, int half_bw_hz, int decim_d, const int *taps, int n_taps)
{
    double fs = (double)DSP_SAMPLE_RATE_HZ / (double)decim_d;
    printf("\n%s  (half_bw %d Hz, D=%d, internal rate %.0f Hz)\n", what, half_bw_hz, decim_d, fs);
    printf("  taps   -3dB     -60dB   transition   stopband   MAC/s      vs 255\n");
    double base_mac = 0.0;
    for (int i = 0; i < n_taps; i++) {
        int len = taps[i];
        static double h[2048];
        if (len > (int)(sizeof(h)/sizeof(h[0]))) continue;
        design(h, len, half_bw_hz, decim_d);
        double e3   = edge_at(h, len, fs, -3.0,  1.0);
        double e60  = edge_at(h, len, fs, -60.0, e3);
        double stop = stopband_db(h, len, fs, e60);
        double mac  = fs * (double)len;
        if (len == 255) base_mac = mac;
        printf("  %4d  %6.0f  %6.0f  %8.0f Hz  %7.1f dB  %7.2f M  %6.2fx\n",
               len, e3, e60, e60 - e3, stop, mac / 1e6,
               base_mac > 0 ? mac / base_mac : 0.0);
    }
}

int main(void)
{
    static const int taps[] = { 255, 191, 159, 127, 95, 79, 63, 47, 31 };
    const int n = (int)(sizeof(taps) / sizeof(taps[0]));

    printf("Stage-2 lowpass, Hamming-windowed sinc - the design in rx_audio.c\n");
    printf("MAC/s is the per-sample cost: internal rate x taps. Stage 2 is\n");
    printf("NON-decimating, so this is the whole of it.\n");

    /* The reference: CW at its narrowest, which is what chose 255 taps. */
    sweep("CW 150 Hz filter - THE CASE THAT SIZED FIR_LEN", 75, 8, taps, n);

    /* SSB, the other D=8 user. */
    sweep("SSB 2.7 kHz", 1350, 8, taps, n);

    /* AM: the widths in the shipped list, all D=4. */
    sweep("AM 4.0 kHz - MEASURED CLEAN on the bench", 2000, 4, taps, n);
    sweep("AM 6.0 kHz - MEASURED RAILING on the bench", 3000, 4, taps, n);
    sweep("AM 8.0 kHz - shipped, never measured", 4000, 4, taps, n);
    sweep("AM 9.6 kHz - shipped, never measured", 4800, 4, taps, n);

    printf("\nRead it as: AM needs a transition small compared with its own\n");
    printf("bandwidth, not the 78 Hz CW was given. The MAC/s column is what\n");
    printf("the frame-time measurement on the bench is made of.\n");
    return 0;
}
