#pragma once
/* RTTY FSK demodulator and character framer - T196, phase R1.
 *
 * Input is COMPLEX baseband at fs_hz. Mark and space sit at
 * centre_hz +/- shift_hz/2. Convention: MARK IS THE HIGHER RF FREQUENCY
 * (standard FSK). In complex baseband that is not inverted, mark is at
 * centre + shift/2. `reverse` swaps them (a station sending inverted, or an
 * inverted sideband upstream).
 *
 * A real audio signal can be fed as (x, 0): its negative-frequency image is
 * 2*centre_hz away and the per-tone filter rejects it.
 *
 * Method, chosen for a first version and measured in test/rtty_harness.c:
 *   1. Mix each tone to DC with a rotating phasor (no sinf per sample).
 *   2. Boxcar over exactly one bit: the matched filter for a rectangular bit.
 *   3. Envelopes |M|, |S|; decision d = (|M|-|S|)/(|M|+|S|), mark when d > 0.
 *      The ratio makes it independent of signal level (no AGC needed).
 *   4. Asynchronous framing. A mark->space crossing of d is only a CANDIDATE
 *      start. d is kept in a history ring; 7 bits after the crossing the
 *      framer searches the start time over +/-0.4 bit for the one that
 *      maximises sum |d| at the 7 sample points (start, 5 data, stop), with
 *      start = space and stop = mark. The boxcar output crosses zero half a
 *      bit after the real edge, so bit k is sampled at start + (k+0.5) bits.
 *      Timing resyncs on every character.
 *
 *   ⛔ Measured dead end (2026-11-12, test/rtty_harness.c, fs 6 kHz): taking
 *      the start time straight from the first zero crossing cost 2-3 dB at
 *      threshold (CER 5.4 % at -6 dB vs 0.3 % theory) and made +/-10 Hz
 *      tuning error or 1 % baud error double the CER. One noisy crossing -
 *      or a noise dip in the idle mark just before the edge - sets the
 *      timing of all 7 samples.
 *
 *   5. AFC (cfg.afc_hz > 0): the boxcar output of the tone that is on
 *      rotates at the tuning error. Sum z[n]*conj(z[n-1]) of the dominant
 *      tone (|d| > RTTY_AFC_GATE) over one bit; its angle is the error.
 *      Move both tones by RTTY_AFC_MU of it per bit, ONLY while the
 *      character clock is locked AND the last character had conf >=
 *      RTTY_AFC_MIN_CONF.
 *      Measured 2026-11-12 (harness, -6 dB): +/-30 Hz CER <= 2.5 %, 0 Hz no
 *      cost, 0.2 Hz/s drift 0.9 %. +/-40 Hz pulls in slowly: 12-16 % over
 *      1500 chars at -6 dB, 3 % at -4 dB.
 *   ⛔ Lock alone is NOT a signal gate: noise locks the clock ~200 slots/min
 *      and walked the offset 37 Hz rms (peak 60) in 60 s. The conf gate 0.5
 *      cuts that to 11 Hz rms (peak 27); 0.6 gives 3.5 Hz but loses +/-40 Hz
 *      pull-in. A real signal-presence squelch should replace this gate.
 *
 * Not in this version: ATC for selective fading, bit-clock tracking inside a
 * character. Add them only if the harness or a recording shows the need.
 *
 * Pure C, no ESP-IDF. Memory: one rtty_demod_t, ~14.5 KB, owned by the caller. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bit length in samples must fit. 45.45 Bd at 12 kHz = 264; at 6 kHz = 132. */
#define RTTY_MAX_BIT_SAMPLES 300
/* Character clock: phase gain, period gain, empty slots before the lock
 * drops, period limit around nominal. */
#define RTTY_GRID_ALPHA     0.25f
#define RTTY_GRID_BETA      0.05f
#define RTTY_GRID_MAX_MISS  2
#define RTTY_GRID_MAX_RATE  0.03
/* AFC: |d| a sample needs to count for its tone; loop gain per bit. */
#define RTTY_AFC_GATE       0.3f
#define RTTY_AFC_MU         0.1f
/* AFC moves only while the last character's conf reached this. */
#ifndef RTTY_AFC_MIN_CONF
#define RTTY_AFC_MIN_CONF   0.5f
#endif
/* Decision history: 8 bits at the longest bit. */
#define RTTY_HIST (8 * RTTY_MAX_BIT_SAMPLES)

typedef struct {
    float fs_hz;      /* complex sample rate */
    float baud;       /* 45.45 */
    float shift_hz;   /* 170 */
    float centre_hz;  /* midpoint of mark and space in the baseband */
    bool  reverse;    /* swap mark and space */
    float squelch;    /* 0 = off; else drop characters with conf below this */
    float afc_hz;     /* 0 = off; else track the signal up to +/- this far */
} rtty_demod_cfg_t;

typedef struct {
    uint8_t code;     /* 5-bit ITA2 code, bit 1 = LSB */
    float   conf;     /* 0..1: mean |d| over the 7 sample points */
    double  t_start;  /* absolute input sample of the start edge (diagnostics) */
} rtty_char_t;

typedef void (*rtty_char_cb)(const rtty_char_t *c, void *user);

typedef struct {
    rtty_demod_cfg_t cfg;
    int   L;                          /* bit length, samples (rounded) */
    float T;                          /* bit length, samples (exact) */
    /* phasors and their per-sample steps */
    float mc, ms, mstep_c, mstep_s;   /* mark */
    float sc, ss, sstep_c, sstep_s;   /* space */
    /* boxcar rings and running sums */
    float mr[RTTY_MAX_BIT_SAMPLES], mi[RTTY_MAX_BIT_SAMPLES];
    float sr[RTTY_MAX_BIT_SAMPLES], si[RTTY_MAX_BIT_SAMPLES];
    float smr, smi, ssr, ssi;
    int   pos;
    int   filled;
    /* framer */
    float    hist[RTTY_HIST];         /* d, indexed by absolute sample % RTTY_HIST */
    uint32_t ns;                      /* absolute count of d values produced */
    float    prev_d;
    bool     in_char;
    double   t_cross;                 /* absolute time of the candidate crossing */
    /* character clock */
    bool     g_valid;
    bool     g_lock;                  /* flywheel: decode at predicted slots */
    int      g_miss;                  /* empty slots in a row */
    double   g_ref;                   /* last start sample point used */
    double   g_P;                     /* tracked character period, samples */
    float    g_bits;                  /* period in bits: 7, 7.5 or 8 */
    float    g_vote;                  /* stop-length vote in progress */
    int      g_votes;
    float    cfg_alpha, cfg_beta;     /* loop gains; init sets the defaults */
    /* tuning aid: smoothed tone envelopes */
    float mark_lvl, space_lvl;
    /* AFC: previous boxcar sums, discriminator for this bit, offset applied */
    float pmr, pmi, psr, psi;
    float afc_r, afc_i;
    float afc_off;                    /* Hz added to centre_hz; read for display */
    bool  afc_ok;                     /* last character confident enough to steer */
    /* counters */
    uint32_t n_chars, n_framing, n_false_start, n_squelched, n_grid, n_unlock;
} rtty_demod_t;

/* Returns false when the configuration cannot be run (bit too long, etc.). */
bool rtty_demod_init(rtty_demod_t *d, const rtty_demod_cfg_t *cfg);

/* Feed n complex samples. cb is called for every accepted character. */
void rtty_demod_feed(rtty_demod_t *d, const float *re, const float *im, int n,
                     rtty_char_cb cb, void *user);

#ifdef __cplusplus
}
#endif
