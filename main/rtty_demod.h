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
 *      character clock is locked, the last character had conf >=
 *      RTTY_AFC_MIN_CONF AND smoothed conf >= RTTY_AFC_MIN_LVL.
 *      Measured 2026-11-12 (harness, -6 dB): +/-30 Hz CER <= 2.5 %, 0 Hz no
 *      cost, 0.2 Hz/s drift 0.9 %. +/-40 Hz pulls in slowly: 12-16 % over
 *      1500 chars at -6 dB, 3 % at -4 dB.
 *   ⛔ Lock alone is NOT a signal gate: noise locks the clock ~200 slots/min
 *      and walked the offset 37 Hz rms (peak 60) in 60 s. The conf gate 0.5
 *      cuts that to 11 Hz rms (peak 27); 0.6 gives 3.5 Hz but loses +/-40 Hz
 *      pull-in. Adding the smoothed-conf gate (squelch, below) at 0.42:
 *      2.5 Hz rms (peak 6) on noise; +/-30 Hz CER <= 4.5 %; +/-40 Hz lost
 *      at -6 dB. At 0.48 (= squelch open) noise wander was 0 but +30 Hz cost
 *      6.9 %; at 0.40, 6 Hz rms. Pull-in beyond +/-30 Hz needs a coarse
 *      acquisition, not a looser gate.
 *   6. Squelch: per-character conf on noise is 0.2-0.5, so a per-character
 *      threshold cannot separate it (0.4 still printed 79 chars/min). Conf
 *      smoothed over ~5 characters can: noise p95 0.40, signal p5 0.62 at
 *      -6 dB and 0.48 at -10 dB. Open 0.48 / close 0.44: 0 noise chars in
 *      30 min, 5-char hold buffer recovers the ramp-up.
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
/* Signal presence: per-character conf smoothed by RTTY_SQ_ALPHA; present at
 * RTTY_SQ_OPEN, gone below RTTY_SQ_OPEN - RTTY_SQ_HYST. cfg.squelch uses the
 * same smoothing and hysteresis with its own open threshold. Up to
 * RTTY_SQ_PENDING characters are held while closed and printed on opening. */
#define RTTY_SQ_ALPHA       0.2f
#define RTTY_SQ_OPEN        0.48f
#define RTTY_SQ_HYST        0.04f
#define RTTY_SQ_PENDING     5
#define RTTY_SQ_LOW         0.5f
#define RTTY_SQ_LOW_RUN     3
/* AFC steers only while smoothed conf >= this. Lower than RTTY_SQ_OPEN on
 * purpose: a signal 30-40 Hz off reads lower conf until AFC has pulled it in. */
#define RTTY_AFC_MIN_LVL    0.42f
/* Coarse acquisition (R013): while no signal is present, find the tone pair
 * within +/- cfg.afc_hz of the centre and put AFC on it. The band around the
 * centre is mixed down, decimated to ~750 Hz, and DFT'd in 0.5 s blocks over
 * RTTY_ACQ_STEP_HZ bins; a pair counts when BOTH tones exceed the median bin
 * power by RTTY_ACQ_SNR. */
#define RTTY_ACQ_RATE_HZ    750.0f
#define RTTY_ACQ_N          400       /* block buffer, >= rate/2 */
#define RTTY_ACQ_BINS       100       /* covers +/-(afc_hz + shift/2) */
#define RTTY_ACQ_STEP_HZ    5.0f
#ifndef RTTY_ACQ_SNR
#define RTTY_ACQ_SNR        6.0f
#endif
#define RTTY_ACQ_MIN_MOVE   10.0f     /* Hz; smaller errors are AFC's job */
/* R026: once a station has been decoded, AFC and acquisition keep within this
 * of where it was last present, so a gap between overs cannot carry the tones
 * to a neighbour. While it is present the anchor follows it (drift). */
#define RTTY_HOLD_HZ        20.0f   /* 40 and 20 both kept 66/66 WAV words; 20 lands nearer */
/* Acquisition window, each side, when AFC is off or narrower: see init. */
#define RTTY_ACQ_FLOOR_HZ   150.0f
/* Squelch: the weaker tone must stand this far over the floor (R014). */
#ifndef RTTY_SQ_TONE_DB
#define RTTY_SQ_TONE_DB     6.0f
#endif
/* 0: the tone gate only OPENS the squelch. 1 (a lost tone also closes it)
 * cost DL2AL a 20-character stretch (15 -> 13 exchange words, the same at
 * 4, 5 and 6 dB - one tone fades there) and bought nothing: with 0 the
 * keyed carrier already prints 0 chars/min. Measured 2026-11-12. */
#ifndef RTTY_SQ_TONE_CLOSE
#define RTTY_SQ_TONE_CLOSE  0
#endif
/* M/S bars: each tone's peak within +/-2 acquisition bins (+/-10 Hz). */
#define RTTY_SNR_BINS       2
/* Band prefilter (R015): mix the pair's centre to 0 Hz and low-pass it
 * (Butterworth, RTTY_PF_ORDER poles, cutoff shift/2 + RTTY_PF_K * baud)
 * before the tone filters, so a strong neighbour outside the pair does not
 * leak in through the boxcar's -13 dB sidelobes. 0 = off. */
#ifndef RTTY_PF_ORDER
#define RTTY_PF_ORDER       8
#endif
#ifndef RTTY_PF_K
#define RTTY_PF_K           0.6f
#endif
#define RTTY_PF_MAXSEC      4
/* Decision history: 8 bits at the longest bit. */
#define RTTY_HIST (8 * RTTY_MAX_BIT_SAMPLES)

typedef struct {
    float fs_hz;      /* complex sample rate */
    float baud;       /* 45.45 */
    float shift_hz;   /* 170 */
    float centre_hz;  /* midpoint of mark and space in the baseband */
    bool  reverse;    /* swap mark and space */
    float squelch;    /* 0 = off; else print only while smoothed conf has
                         reached this (RTTY_SQ_OPEN is the measured choice) */
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
    /* band prefilter: centre mixer + biquads (same coefficients on I and Q) */
    float pc, ps, pstep_c, pstep_s;
    int   pf_n;                       /* sections in use */
    float pf_b0[RTTY_PF_MAXSEC], pf_b1[RTTY_PF_MAXSEC], pf_b2[RTTY_PF_MAXSEC];
    float pf_a1[RTTY_PF_MAXSEC], pf_a2[RTTY_PF_MAXSEC];
    float pf_z[RTTY_PF_MAXSEC][2][2]; /* [section][I/Q][state] */
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
    /* Each tone's power over the noise floor, dB: peak of acq_pow within
     * +/-RTTY_SNR_BINS of the tone over the median bin, every 0.5 s. This is
     * what the M/S bars show; mark_lvl/space_lvl only compare the two tones
     * with each other, so on noise both read "full" (operator, 2026-11-12). */
    float mark_snr_db, space_snr_db;
    /* AFC: previous boxcar sums, discriminator for this bit, offset applied */
    float pmr, pmi, psr, psi;
    float afc_r, afc_i;
    float afc_off;                    /* Hz added to centre_hz; read for display */
    bool  afc_ok;                     /* last character confident enough to steer */
    /* signal presence + squelch */
    float sig_lvl;                    /* smoothed conf; read for display */
    bool  present;                    /* RTTY_SQ_OPEN gate, drives AFC */
    bool  sq_open;                    /* cfg.squelch gate, drives output */
    int   low_run;                    /* characters in a row below RTTY_SQ_LOW */
    int   n_pend;
    rtty_char_t pend[RTTY_SQ_PENDING];
    /* coarse acquisition */
    int   acq_D, acq_k, acq_n, acq_N, acq_nb, acq_blocks;
    float acq_c, acq_s, acq_step_c, acq_step_s;   /* centre mixer */
    float acq_ar, acq_ai;                         /* decimator accumulator */
    float acq_br[RTTY_ACQ_N], acq_bi[RTTY_ACQ_N];
    float acq_f0;                                 /* Hz of bin 0, rel. centre */
    float acq_pow[RTTY_ACQ_BINS];
    uint32_t n_acq;                               /* jumps made */
    bool  acq_held;   /* R026: a station was decoded; tones stay near acq_anchor */
    float acq_anchor; /* R026: afc_off where that station was last present */
    bool  on_station; /* R026: present AND both tones up (R014) */
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
