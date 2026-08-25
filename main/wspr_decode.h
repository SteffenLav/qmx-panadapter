#pragma once
/* WSPR audio-domain decode: turn 12000 Hz mono PCM samples into decoded
 * WSPR type-1 messages, on top of the protocol/FEC layer in wspr_proto.h /
 * wspr_fano.h.
 *
 * Portable on purpose (no ESP deps) so test/wspr_decode_harness.c can link
 * the REAL functions against a real captured WSPR WAV file - same
 * convention as every other module here. Proven against WSJT's own
 * official WSPR sample recording (test/wav_reference/wspr/150426_0918.wav,
 * from sourceforge.net/projects/wsjt/files/samples/WSPR/): 5 of 8 detected
 * candidates decode cleanly to standard-format US callsigns with legal
 * WSPR power values and fast Fano convergence (82-102 cycles); the other 3
 * are correctly rejected as implausible (see wspr_decode.c for the three
 * checks and why each exists).
 *
 * RESOLVED: why the file's own STRONGEST candidate still fails. Real
 * ionospheric fading (QSB) within the 110 s transmission - not a bug, not
 * frequency drift (tested and ruled out), not a second overlapping signal.
 * See test/wspr_diag_candidate0.c and docs/wspr-phase1-status.md for the
 * full diagnosis (sync-match rate climbing from ~55% to ~85% across the
 * transmission, total power rising ~20x, a clean single frequency peak).
 *
 * Per-symbol reliability-weighted decoding (wspr_fano_decode_weighted(),
 * main/wspr_fano.c/.h) was built and shipped as a FALLBACK specifically to
 * target this: it measurably beats hard-decision at moderate fading (10/10
 * vs 9/10 across seeds, test/wspr_fading_harness.c), but even oracle
 * (ground-truth) weighting cannot recover THIS candidate's severity -
 * confirmed a genuine information-theoretic limit, not a tuning gap. Hard-
 * decision stays the primary path (no calibration fragility); weighted is
 * tried only when hard-decision doesn't produce a plausible result.
 *
 * KNOWN LIMITATIONS (next work, not silently glossed over):
 *  - No frequency-drift compensation - tested against the one candidate
 *    that seemed like a plausible drift case and ruled out (see above);
 *    may still matter for a different real signal never captured yet.
 *  - Whole-capture soft-decision metrics (as opposed to the per-symbol
 *    weighted fallback above) were tried twice and NOT shipped - see
 *    docs/wspr-phase1-status.md's licensing section and the two "soft
 *    metric" update entries for why (a fixed-scale table was too narrow;
 *    a per-capture-normalized one worked synthetically but regressed the
 *    real WAV 5/8 -> 1/8).
 *  - The plausibility checks (message shape, legal power, Fano cycle
 *    count) are a proxy for signal quality, not a real quality metric
 *    (sync correlation vs. noise floor, the way wsprd itself gates
 *    candidates). Good enough to reject a wrong decode in testing so far,
 *    not yet validated against a genuinely weak/marginal real signal.
 */
#include <stdint.h>
#include "wspr_proto.h"
#include "wspr_fano.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WSPR_SAMPLE_RATE_HZ 12000.0
#define WSPR_SYM_LEN_SAMPLES 8192 /* symbol period = 8192/12000 s = 0.68267 s */
/* == WSPR_NSYM * TONE_SPACING's reciprocal; matches wspr_fano.h's WSPR_NSYM */

typedef struct {
    double freq_hz;
    double comb_score; /* relative, not calibrated to any absolute unit */
} wspr_freq_candidate_t;

/* Scan mono 12000 Hz samples[0..n) for candidate WSPR signal center
 * frequencies in [f_lo_hz, f_hi_hz) - a coarse, cheap (single FFT) pass
 * meant to hand a short list of real candidates to wspr_decode_candidate(),
 * not a final answer. Writes up to max_out candidates into `out`, ordered
 * strongest-first. Returns the count written. */
int wspr_find_candidates(const int16_t *samples, long n, double f_lo_hz,
                          double f_hi_hz, wspr_freq_candidate_t *out,
                          int max_out);

typedef struct {
    int ok; /* 1 if this cleared all three plausibility checks */
    char callsign[7];
    char grid[5];
    int power_dbm;
    double freq_hz;      /* the candidate frequency that was tried */
    long best_dt_samples; /* transmission start offset found within the capture */
    double sync_score;   /* higher = better match to the known sync vector */
    unsigned int cycles; /* Fano decoder cycle count - a quality signal, see wspr_decode.c */
} wspr_decode_result_t;

/* Try to decode a WSPR transmission near candidate frequency f0_hz within
 * mono 12000 Hz samples[0..n). Internally searches the transmission
 * start-time slack (the capture is expected to be >= 110.6 s, i.e. at
 * least one full WSPR transmission plus some margin) and picks the best
 * alignment by sync-vector correlation. Fills *result unconditionally;
 * result->ok is the gated verdict. */
void wspr_decode_candidate(const int16_t *samples, long n, double f0_hz,
                            wspr_decode_result_t *result);

/* ---- FALSE-DECODE GUARDS -------------------------------------------------
 *
 * wspr_decode_candidate()'s own checks (message shape, legal power, repack
 * round-trip, cycles <= WSPR_CYCLES_SUSPECT) are not sufficient. Simulation
 * mode produced a counterexample on hardware: with a purely synthetic window
 * containing six phantom stations and nothing else, the decoder emitted a
 * SEVENTH station -
 *
 *     DECODED 'LG9TPW' 'FQ54' 17 dBm  f=1442.23 Hz dt=4.76s cycles=1534
 *
 * - which passed every check. 17 dBm is a legal power, the callsign and grid
 * are well formed, and 1534 is inside the 2000 threshold. It sat 6.6 Hz from
 * a genuine decode at 1448.82 Hz, i.e. about one WSPR signal bandwidth: a
 * candidate on a strong signal's skirt.
 *
 * This matters more than a display artefact. A WSPR spot is a RECEPTION
 * REPORT, and publishing a station that was never on the air is a fabricated
 * measurement - the thing this project refuses to do anywhere else (see the
 * RST placeholder and PSK Reporter callsign rules in CLAUDE.md).
 *
 * Two guards, deliberately INDEPENDENT, because they trade differently and
 * which one is right cannot be settled on synthetic audio:
 *
 *   NEAR - reject a decode within near_hz of one already accepted this cycle.
 *          Surgical. Two WSPR signals closer than ~6 Hz overlap anyway, so it
 *          should cost no real decode.
 *   SLOW - reject a decode needing more than slow_cycles Fano cycles. Blunt.
 *          Every genuine decode observed so far converged in 81-102 cycles
 *          (5 real signals in the reference WAV, 6 synthetic phantoms), but
 *          synthetic phantoms are clean and say nothing about weak real
 *          signals, so this could cost sensitivity.
 *
 * ⭐ BOTH ARE ALWAYS MEASURED, whether or not they are enforced. A guard that
 * only acts when enabled can never be compared against the alternative on the
 * SAME signals - it would need two flashes and two different band conditions.
 * Measuring both on every decode means one ordinary receiving session
 * accumulates the evidence to decide, which is the whole point. */

#define WSPR_GUARD_NEAR_HZ      10.0   /* ~1.5x a WSPR signal's ~6 Hz width */
/* 1000, and ENFORCED as of 2026-08-25 - see wspr_guards_defaults() for the two
 * runs that justify enforcing it at all.
 *
 * ⚠ THE NUMBER WAS 600 FOR ABOUT TEN MINUTES AND THAT WAS WRONG, from stale
 * measurements: the cycle counts used to pick it came from BEFORE the
 * windowed-sinc decimation filter landed, and the new filter moves them. The
 * same wsprd-confirmed station (PA3BCA, 19:06 reference window) went from 336
 * cycles pre-filter to 823 post-filter, so 600 would have rejected a decode
 * another implementation calls real.
 *
 * ⭐ GENERALISE THAT: a Fano cycle count is a property of the DECODE PATH, not
 * of the signal. Any change to the front end - filter, decimation, metric -
 * invalidates every threshold expressed in cycles. Re-measure against
 * test/wav_reference/wspr/ before trusting one.
 *
 * 1000 clears the highest confirmed-real decode observed on the current path
 * (823) with margin, still rejects 68 of 112 slow one-offs across 16 h of
 * running, and loses nothing anywhere in the data. */
#define WSPR_GUARD_SLOW_CYCLES  1000u
#define WSPR_ACCEPTED_MAX       16

typedef struct {
    int    enforce_near;
    double near_hz;
    int    enforce_slow;
    unsigned int slow_cycles;
} wspr_guards_t;

/* Frequencies accepted so far IN THIS CYCLE. Reset per cycle by the caller. */
typedef struct {
    int    n;
    double freq_hz[WSPR_ACCEPTED_MAX];
} wspr_accepted_t;

typedef enum {
    WSPR_GUARD_PASS = 0,
    WSPR_GUARD_REJECT_NEAR,
    WSPR_GUARD_REJECT_SLOW,
} wspr_guard_verdict_t;

/* Sensible defaults: NEAR enforced (agreed low-risk), SLOW measured only. */
void wspr_guards_defaults(wspr_guards_t *g);

/* Distance in Hz to the nearest already-accepted decode, or -1.0 if none.
 * Always meaningful, independent of whether any guard is enforced. */
double wspr_nearest_accepted_hz(const wspr_accepted_t *acc, double freq_hz);

/* The verdict for `r` given what has already been accepted this cycle.
 * *nearest_hz_out (may be NULL) always receives the measured distance, and
 * *would_near / *would_slow (may be NULL) always receive what EACH guard
 * would have decided, regardless of which is enforced - that is the data the
 * real-world comparison is made from. */
wspr_guard_verdict_t wspr_guard_check(const wspr_guards_t *g,
                                      const wspr_accepted_t *acc,
                                      const wspr_decode_result_t *r,
                                      double *nearest_hz_out,
                                      int *would_near, int *would_slow);

void wspr_accepted_add(wspr_accepted_t *acc, double freq_hz);

#ifdef __cplusplus
}
#endif
