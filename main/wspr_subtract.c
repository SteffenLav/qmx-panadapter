#include "wspr_subtract.h"
#include "wspr_proto.h"
#include "wspr_decode.h"       /* WSPR_SYM_LEN_SAMPLES, WSPR_SAMPLE_RATE_HZ */

#include <math.h>
#include <string.h>

#define TONE_SPACING_HZ (WSPR_SAMPLE_RATE_HZ / (double)WSPR_SYM_LEN_SAMPLES)

int wspr_tones_from_message(const char *callsign, const char *grid,
                            int power_dbm, uint8_t tones_out[WSPR_NSYM])
{
    wspr_msg_bytes_t msg;
    if (!wspr_pack_message(callsign, grid, power_dbm, &msg)) return 0;
    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM];
    wspr_convolve_encode(&msg, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, tones_out);
    return 1;
}

int wspr_subtract(int16_t *samples, long n, double f0_hz, long dt_samples,
                  const uint8_t tones[WSPR_NSYM])
{
    if (!samples || !tones || n <= 0) return 0;
    const long LEN = WSPR_SYM_LEN_SAMPLES;
    int done = 0;

    for (int i = 0; i < WSPR_NSYM; i++) {
        long s0 = dt_samples + (long)i * LEN;
        if (s0 < 0 || s0 + LEN > n) continue;           /* partial symbol: leave it */

        const double f = f0_hz + (double)tones[i] * TONE_SPACING_HZ;
        const double w = 2.0 * M_PI * f / (double)WSPR_SAMPLE_RATE_HZ;

        /* Least-squares fit of ONE sinusoid over this symbol.
         *
         * c = sum x[k] e^{-jwk} = (A*LEN/2) e^{jphi} for x = A cos(wk+phi), so
         * (2/LEN)*c is exactly A e^{jphi}. Fitting per symbol rather than once
         * per transmission is deliberate: over 110 s of HF the amplitude and
         * phase genuinely move (that is what QSB IS), and a single global
         * estimate would leave most of a fading signal behind.
         *
         * The absolute sample index is used for the phase so each symbol is
         * fitted in the same reference frame as it will be reconstructed in. */
        double re = 0.0, im = 0.0;
        for (long k = 0; k < LEN; k++) {
            const double ph = w * (double)(s0 + k);
            const double x  = (double)samples[s0 + k];
            re += x * cos(ph);
            im -= x * sin(ph);
        }
        const double a_re = 2.0 * re / (double)LEN;
        const double a_im = 2.0 * im / (double)LEN;

        /* ⚠ NO TAPER. A raised-cosine ramp over the symbol edges was tried, on
         * the theory that subtracting HARD-switched tones from a GFSK signal
         * leaves 162 sharp edges and therefore something broadband. MEASURED
         * AND FALSIFIED: it made things worse, not better (19:10 6 -> 5 unique
         * stations, 19:14 3 -> 1). Leaving more of the signal behind at each
         * transition costs more than the edge artefact does. Do not re-add it
         * without a measurement that says otherwise. */
        for (long k = 0; k < LEN; k++) {
            const double ph = w * (double)(s0 + k);
            double v = (double)samples[s0 + k] - (a_re * cos(ph) - a_im * sin(ph));
            /* Clamp: the residual can exceed int16 where two strong signals
             * overlap, and wrapping would inject a discontinuity that reads as
             * broadband noise to the next pass - the opposite of the point. */
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            samples[s0 + k] = (int16_t)lround(v);
        }
        done++;
    }
    return done;
}
