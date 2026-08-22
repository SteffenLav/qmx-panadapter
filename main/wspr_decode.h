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
 * checks and why each exists - one caught the file's own STRONGEST signal,
 * cause not yet confirmed, see docs/wspr-phase1-status.md).
 *
 * KNOWN LIMITATIONS (next work, not silently glossed over):
 *  - No frequency-drift compensation. A real WSPR transmitter's oscillator
 *    can drift a fraction of a Hz over the 110.6 s transmission; this
 *    module assumes a single fixed center frequency for the whole message.
 *    A linear-drift search WAS tried against the one strong-signal
 *    candidate that fails to decode plausibly, and it did NOT confirm
 *    drift as the cause - a better-scoring drift produced a DIFFERENT,
 *    still-implausible decode with a worse (not better) Fano cycle count.
 *    See docs/wspr-phase1-status.md for the negative result. Don't assume
 *    drift compensation fixes that candidate without re-testing.
 *  - Hard-decision only. Tone power is compared pairwise (which of two
 *    candidate tones is stronger) rather than feeding a graded soft metric
 *    to the Fano decoder - correctness-proven (see wspr_codec_harness.c)
 *    but not the maximum-sensitivity approach a real weak-signal receiver
 *    needs.
 *  - The three plausibility checks (message shape, legal power, Fano cycle
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
