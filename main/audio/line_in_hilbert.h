#pragma once
/* line_in_hilbert.h - the Hilbert kernel, as pure maths with no ESP headers.
 *
 * Split out of line_in.c so test/hilbert_harness.c can link THE SAME code
 * rather than a copy of it. The thing this kernel decides - how well the
 * mirror image cancels - is not visible as a crash or an error code. A wrong
 * sign shows up as a waterfall where every signal appears twice, and a wrong
 * window or length shows up as a weak second trace only at low audio
 * frequencies. Both read as "the jack input is a bit odd" rather than as a
 * bug, which is exactly the kind of thing that has to be measured on the host
 * before it reaches hardware.
 */
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Tap count, window and group delay. ALL THREE WERE MEASURED, not chosen.
 *
 * A windowed Hilbert FIR is useless at DC and at Nyquist and only
 * approximates -90 deg in between. How far down it stays useful is set by the
 * length and the window, and getting it wrong is silent: the symptom is a
 * mirror image of LOW-frequency signals only, which looks like a second weak
 * station rather than a bug.
 *
 * test/hilbert_harness.c measures the image rejection directly. Measured
 * 2026-10-10, in dB (more negative is better):
 *
 *     N    window      200 Hz   300 Hz   500 Hz    1 kHz
 *    127   Blackman      -7.8    -12.0    -21.6    -64.8
 *    127   Hamming      -10.2    -16.1    -31.6    -60.4
 *    191   Hamming      -16.2    -27.2    -55.8    -58.4
 *    255   Hamming      -23.2    -43.7    -57.0    -66.3   <- chosen
 *    383   Hamming      -45.2    -55.4    -54.2    -61.4
 *    511   Blackman     -43.7    -82.8    -74.2    -77.5
 *
 * ⛔ 127 TAPS WAS THE FIRST ATTEMPT AND IT WAS WRONG. Its -12 dB at 300 Hz
 * puts a mirror image well inside the ~45 dB a waterfall displays, so every
 * low audio tone would have drawn twice. The comment at the time claimed the
 * response was flat "from roughly 500 Hz"; the measurement says the real
 * edge for useful rejection was nearer 1 kHz. This is why the harness exists.
 *
 * 255 taps holds 40 dB or better across the whole 300-3000 Hz band an SSB
 * receiver delivers, which puts the residual image at or below the waterfall
 * floor. Below 300 Hz it degrades as the table shows, and that is accepted -
 * an SSB passband does not start there.
 *
 * Hamming rather than Blackman: at a fixed length Blackman's wider main lobe
 * costs low-frequency performance, and it buys stopband depth the midband
 * already has 20 dB more of than it needs.
 *
 * Cost is 255 x 48000 = 12.2 M multiply-accumulates per second through
 * esp-dsp's kernel. Doubling again to 511 buys another 20-40 dB nobody can
 * see on a waterfall.
 */
#define LI_HILBERT_N    255
#define LI_HILBERT_MID  (LI_HILBERT_N / 2)   /* 127: the group delay, in samples */

/* h[k] = 2/(pi*k) for ODD k, 0 for even k, windowed; k is the offset from the
 * centre tap. The even taps really are zero - that is the structure of the
 * transform, not an optimisation.
 *
 * ⛔ SIGN CONVENTION: this is +H{x}, so the analytic signal is x + j*H{x} and
 * a tone cos(wt) becomes e^(+jwt). Audio therefore lands on the POSITIVE half
 * of the spectrum, which is what ui.c assumes when it reports dial_hz = 0 for
 * a real source and labels the axis in audio Hz. Flip the sign and every
 * signal lands on the negative half instead: the right-hand side of the
 * display goes empty, and nothing logs anything. test_sign_convention() in
 * the harness is the guard. */
static inline void li_build_hilbert(float *h)
{
    for (int n = 0; n < LI_HILBERT_N; n++) {
        int k = n - LI_HILBERT_MID;
        float v = ((k == 0) || ((k % 2) == 0))
                    ? 0.0f
                    : 2.0f / ((float)M_PI * (float)k);
        float t = (float)n / (float)(LI_HILBERT_N - 1);
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * t);   /* Hamming */
        h[n] = v * w;
    }
}
