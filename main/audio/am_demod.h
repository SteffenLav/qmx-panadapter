// am_demod.h - AM envelope detector for the RX audio chain.
//
// ⭐ IT LIVES IN A HEADER SO THE HOST HARNESS TESTS THE SHIPPED CODE.
// test/am_demod_harness.c includes THIS file; there is no second copy of the
// demodulator to drift out of step. Same arrangement as line_in_hilbert.h.
//
// WHERE IT SITS IN THE CHAIN
//   rx_audio.c already mixes the wanted signal down to complex baseband and
//   filters it (NCO1 -> stage-1 decimator -> stage-2 narrow lowpass), leaving
//   s_narrow_re/im at the 6 kHz internal rate. For CW and SSB the audio is the
//   REAL PART of that, after a final NCO rotation. For AM it is the MAGNITUDE,
//   which is where this file comes in.
//
// ⛔ THE CARRIER IS A DC TERM AND IT MUST BE REMOVED HERE, NOT LATER.
//   |z| of an AM signal is (carrier + modulation), so the envelope of a
//   steady carrier is a large constant. Two things break if it is passed on:
//
//     * the AGC in rx_audio.c is driven from fabsf(audio). A constant equal to
//       the carrier pegs its envelope, the gain law target/(env+1) collapses,
//       and the modulation is squashed to nothing. The louder the station, the
//       quieter it gets.
//     * the codec would be handed a large DC offset, which is a steady pull on
//       the headphone element and audible as a click when it switches in or
//       out.
//
//   So a one-pole high-pass follows the magnitude immediately: track the mean
//   with a slow leak and subtract it. What is left is the modulation, centred
//   on zero, which is what every stage downstream already expects.
//
// ⚠ THE FIRST SAMPLE MUST PRIME THE TRACKER, NOT BE FILTERED BY IT.
//   Starting the mean at zero means the first output IS the whole carrier -
//   full scale, decaying over the filter's time constant. That is a thump in
//   the headphones on every mode change into AM. Priming on the first sample
//   costs one branch and removes it entirely.
//
// ⚠ AND THE CORNER IS NOT A FREE CHOICE. Too high and it eats the low audio
//   an AM station actually carries; too low and it takes a long time to settle
//   and lets a fading carrier ride through as a rumble. 20 Hz is comfortably
//   below the lowest useful modulation and settles in about 8 ms at 6 kHz.
//   The harness measures both ends of that trade rather than asserting it.

#pragma once

#include <math.h>

// High-pass corner that strips the carrier, in Hz. See the note above.
#define AM_DC_CORNER_HZ   20.0f

typedef struct {
    float dc;        // tracked carrier level (the mean of the envelope)
    int   primed;    // 0 until the first sample has seeded dc
} am_demod_t;

static inline void am_demod_reset(am_demod_t *a)
{
    a->dc = 0.0f;
    a->primed = 0;
}

// One-pole coefficient for the corner above at the given sample rate. Clamped
// because a corner at or past Nyquist is not a filter, it is an oscillator.
static inline float am_demod_alpha(float fs_hz)
{
    if (fs_hz <= 0.0f) return 0.5f;
    float a = 2.0f * (float)M_PI * AM_DC_CORNER_HZ / fs_hz;
    if (a > 0.5f)   a = 0.5f;
    if (a < 1e-6f)  a = 1e-6f;
    return a;
}

// Envelope-detect one complex baseband sample and return the audio.
// alpha comes from am_demod_alpha() once per retune, not per sample.
static inline float am_demod_step(am_demod_t *a, float re, float im, float alpha)
{
    float env = sqrtf(re * re + im * im);
    if (!a->primed) { a->dc = env; a->primed = 1; }
    else            { a->dc += (env - a->dc) * alpha; }
    return env - a->dc;
}
