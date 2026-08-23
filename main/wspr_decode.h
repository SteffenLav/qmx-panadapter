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

#ifdef __cplusplus
}
#endif
