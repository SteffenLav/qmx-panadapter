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

/* ⛔ NO cos()/sin() PER SAMPLE. This function ran two loops of 8192 samples per
 * symbol, each calling cos() and sin() on a double - 5.3 MILLION software
 * double trig calls per subtracted signal. On the ESP32-P4, whose FPU is
 * single-precision only, that measured at 67 SECONDS per signal on hardware:
 * a 120 s WSPR cycle went from 58 s to 154 s and started overrunning its own
 * decode budget. On a host it is invisible - hardware double, fast libm - so
 * it survived every host test of the two-pass path.
 *
 * Instead the oscillator advances by one complex multiply per sample in float
 * and is re-seeded from the exact phase every OSC_RESEED samples. Error can
 * then accumulate over at most 512 float steps and is WIPED rather than
 * rescaled, so this is not a precision trade - the fit and the residual are as
 * good as before, at about a thousandth of the trig cost. */
#define OSC_RESEED 512

typedef struct { double w; long base; float c, s, cs, sn; } osc_t;

static void osc_init(osc_t *o, double w, long base)
{
    o->w = w; o->base = base;
    o->cs = (float)cos(w); o->sn = (float)sin(w);
    o->c = 1.0f; o->s = 0.0f;
}

/* cos/sin of w*(base+k), for k stepping 0,1,2,... in order. */
static inline void osc_at(osc_t *o, long k)
{
    if ((k & (OSC_RESEED - 1)) == 0) {
        const double ph = o->w * (double)(o->base + k);
        o->c = (float)cos(ph);
        o->s = (float)sin(ph);
    } else {
        const float nc = o->c * o->cs - o->s * o->sn;
        const float ns = o->c * o->sn + o->s * o->cs;
        o->c = nc; o->s = ns;
    }
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
        osc_t osc;
        osc_init(&osc, w, s0);
        float re = 0.0f, im = 0.0f;
        for (long k = 0; k < LEN; k++) {
            osc_at(&osc, k);
            const float x = (float)samples[s0 + k];
            re += x * osc.c;
            im -= x * osc.s;
        }
        const float a_re = 2.0f * re / (float)LEN;
        const float a_im = 2.0f * im / (float)LEN;

        /* ⚠ NO TAPER. A raised-cosine ramp over the symbol edges was tried, on
         * the theory that subtracting HARD-switched tones from a GFSK signal
         * leaves 162 sharp edges and therefore something broadband. MEASURED
         * AND FALSIFIED: it made things worse, not better (19:10 6 -> 5 unique
         * stations, 19:14 3 -> 1). Leaving more of the signal behind at each
         * transition costs more than the edge artefact does. Do not re-add it
         * without a measurement that says otherwise. */
        osc_init(&osc, w, s0);
        for (long k = 0; k < LEN; k++) {
            osc_at(&osc, k);
            double v = (double)samples[s0 + k] - (double)(a_re * osc.c - a_im * osc.s);
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
