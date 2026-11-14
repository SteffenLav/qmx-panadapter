#pragma once
/* RTTY auto speed + shift (R028). Pure C, no ESP-IDF - the harness links it.
 *
 * Fed the same complex baseband as rtty_demod, with the pair's expected
 * centre. Two stages, both from the signal alone:
 *
 *   SHIFT: a power spectrum over +/-RTTY_AUTO_SPAN_HZ of the centre, 5.9 Hz
 *   bins (256-point FFT at 1500 Hz), averaged. Each standard shift S is scored at every centre c within
 *   +/-RTTY_AUTO_CSEARCH_HZ: the weaker of the two tone peaks (c -/+ S/2)
 *   over the median bin. The weaker, not the sum: one strong tone of a
 *   neighbour must not make a pair.
 *
 *   SPEED: with the pair found, one two-tone detector PER CANDIDATE baud
 *   b, boxcar = half b's bit (one 5 ms window for all failed every case at
 *   -6 dB). SPACE runs are timed: a space run never holds a stop bit, so it
 *   is a whole number of bits whatever the stop length (1, 1.5, 2). Which
 *   tone is space is not assumed - the runs of BOTH tones are scored and
 *   the better one is space (GRITTY's LSB files scored -0.3..-0.4 with the
 *   high tone taken as mark: those were mark runs with 1.5-bit stops).
 *   Sums decay (RTTY_AUTO_KEEP runs) so a finished over does not dilute
 *   the next. A new state counts only after it holds RTTY_AUTO_DEBOUNCE of
 *   a bit, timed from where it began: on real audio ~1000 of ~1300 runs in
 *   SX150ITU were 1/4-3/4-bit glitches, which split real runs and scored
 *   every speed negative (synthetic noise never made them).
 *   score = mean cos(2 pi run / T_b). A b Bd signal also fits 2b (every
 *   run is an even number of 2b bits), so the LOWEST candidate within
 *   RTTY_AUTO_NEAR of the best wins; 2b does not fit b (a one-bit run is
 *   half a b bit, cos = -1).
 *   ⛔ Measured dead end 2026-11-14: scoring ALL runs against HALF bits (to
 *   cover the 1.5-bit stop) fits wrong speeds - 50 Bd with 1 stop bit read
 *   as 75 (20 ms = 3 half-bits of 75 Bd), 100 Bd with 1 or 2 read as 50.
 *
 * Nothing here decodes text; the caller applies the result to rtty_demod. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTTY_AUTO_SPAN_HZ     600.0f
#define RTTY_AUTO_CSEARCH_HZ  150.0f
#define RTTY_AUTO_BIN_HZ      (RTTY_AUTO_RATE_HZ / RTTY_AUTO_NFFT)   /* 5.86 Hz */
#define RTTY_AUTO_RATE_HZ     1500.0f   /* spectrum decimated to this */
#define RTTY_AUTO_NFFT        256       /* radix-2 FFT; a 300-point DFT cost 4.5x the demod's own acquisition */
#define RTTY_AUTO_NBINS       205       /* -102..+102 bins = +/-598 Hz */
#define RTTY_AUTO_MAXWIN      80        /* boxcar ring: half a 45.45 Bd bit at 6 kHz = 66 */
#define RTTY_AUTO_NSHIFT      5
#define RTTY_AUTO_NBAUD       4
#define RTTY_AUTO_MIN_TRANS   80        /* space runs before a speed verdict */
#define RTTY_AUTO_NEAR        0.75f
#define RTTY_AUTO_HIST_N      64        /* 1 ms bins */
#define RTTY_AUTO_HIST_DET    2         /* the 75 Bd detector */
#define RTTY_AUTO_KEEP        150.0f    /* runs: time constant of the decaying sums */
#define RTTY_AUTO_DEBOUNCE    0.4f

extern const float rtty_auto_shifts[RTTY_AUTO_NSHIFT];   /* 170 200 425 450 850 */
extern const float rtty_auto_bauds[RTTY_AUTO_NBAUD];     /* 45.45 50 75 100 */

typedef struct {
    float fs, centre;                 /* input rate, expected pair centre (Hz) */
    /* spectrum */
    int   D, k;                       /* decimation factor, phase */
    float mc, ms, mstep_c, mstep_s;   /* mixer to 0 Hz */
    float ar, ai;
    float br[RTTY_AUTO_NFFT], bi[RTTY_AUTO_NFFT];
    int   nb;
    float pow[RTTY_AUTO_NBINS];
    int   blocks;
    /* ⛔ Work buffers live HERE, never on the stack: rtty_auto_feed runs in
     * rx_audio (4 KB internal stack). 2 KB of FFT arrays there overflowed it
     * on the first block - bench2 2026-11-14, Stack protection fault. */
    float fx[RTTY_AUTO_NFFT], fy[RTTY_AUTO_NFFT];
    float sorted[RTTY_AUTO_NBINS];
    /* shift verdict */
    bool  shift_ok;
    float shift, pair_centre, pair_db; /* pair_db: weaker tone over median */
    /* speed: one detector per candidate, boxcar = half its bit */
    float m_c, m_s, m_sc, m_ss, s_c, s_s, s_sc, s_ss;   /* tone mixers (shared) */
    struct {
        int   W, pos, filled, state;
        int   cand;                    /* debounce: state seen, not yet held */
        uint32_t t_cand;
        float mr[RTTY_AUTO_MAXWIN], mi[RTTY_AUTO_MAXWIN], sr[RTTY_AUTO_MAXWIN], si[RTTY_AUTO_MAXWIN];
        float smr, smi, ssr, ssi;
        uint32_t t_last;
        bool  have_last;
        float T;                       /* bit, samples */
        uint32_t deb;                  /* debounce, samples */
        float acc[2], cnt[2];          /* [0] runs of the LOW tone, [1] of the HIGH; decaying */
        int   ntrans, nshort;
    } det[RTTY_AUTO_NBAUD];
    /* ⛔ NO double ANYWHERE ON THE SAMPLE PATH: the P4's FPU is single
     * precision, so double is software. A double counter + per-sample
     * double compare took the decoder from 4.5 % to 10.5 % of core 1 and
     * the worst call to 32 ms (bench2 2026-11-14). The host harness, on a
     * PC with fast doubles, could not show it. */
    uint32_t n;                        /* sample count; differences are modular */
    uint32_t max_dt;
    int   ntrans;                      /* space runs timed by the slowest detector */
    bool  baud_ok;
    float baud;
    bool  mark_low;                    /* the high tone's runs were the whole-bit ones:
                                        * mark is the LOW tone (LSB audio, or a reversed
                                        * station) */
    float score[RTTY_AUTO_NBAUD], short_frac[RTTY_AUTO_NBAUD];
    /* Development: run lengths in 1 ms bins from the RTTY_AUTO_HIST_DET
     * detector, [0] low tone, [1] high tone - to see what a real radio path
     * does to the timing (a 75 Bd signal via a QMX scored 100 > 75). */
    uint16_t hist[2][RTTY_AUTO_HIST_N];
} rtty_auto_t;

void rtty_auto_init(rtty_auto_t *a, float fs, float centre_hz);

/* Decode quality over a window: the MEAN CONFIDENCE of the characters
 * decoded (rtty_char_t.conf), 0 if fewer than RTTY_AUTO_Q_MIN_CHARS.
 * Measured on a clean 75 Bd WAV (2026-11-15): 75 -> 0.84, 100 -> 0.65,
 * 50 -> 0.61, 45.45 -> 0.57, wrong shift -> no characters.
 * ⛔ Not character counts: (rate x clean fraction) scored 45.45 at 0.88 on
 * a real 75 Bd over (bench2) - a slow decoder on a fast signal frames
 * garbage cleanly - and a right switch (0.88 -> 0.92) was reverted. */
#define RTTY_AUTO_Q_MIN_CHARS 5
float rtty_auto_quality(uint32_t dchars, float dconf_sum);

/* After an Auto switch: keep it (true) or revert (false). Kept only when
 * the decode clearly improved - so a damaged signal cannot push a wrong
 * setting and leave it there (on-air 2026-11-14: 100 Bd, 850 Hz, Reverse
 * flips on a starved, damaged 75 Bd signal). */
bool rtty_auto_trial_keep(float q_before, float q_after);   /* +0.08, or <0.2 -> >0.5 */
void rtty_auto_feed(rtty_auto_t *a, const float *re, const float *im, int n);

#ifdef __cplusplus
}
#endif
