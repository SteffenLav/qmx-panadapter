#pragma once
/* WSPR's K=32 rate-1/2 convolutional code: encode, interleave, sync-vector
 * combine, and the Fano sequential decoder that undoes all of it.
 *
 * Portable on purpose (no ESP deps) so test/wspr_codec_harness.c can link
 * the REAL functions - same convention as wspr_proto.h/db_gridlines.c.
 *
 * The Fano decoder (wspr_fano_decode) is a CLEAN-ROOM implementation of
 * Fano's 1963 sequential decoding algorithm - public academic material,
 * independent of any particular codebase. An earlier version of this file
 * ported WSJT-X's own lib/wsprd/fano.c (GPL v3) "near-verbatim", which was
 * a licensing mistake in this MIT-licensed project - caught and corrected;
 * see docs/wspr-phase1-status.md for the full account. The "Layland-
 * Lushbaugh" K=32 generator polynomials are WSPR's actual FEC (a protocol
 * fact, not anyone's expression) - Plain Viterbi is infeasible at K=32
 * (2^31 states); the Fano sequential-decode algorithm is what makes this
 * tractable, per docs/wspr-phase0-research.md.
 *
 * The sync vector and generator polynomials are cross-checked against three
 * independent source trees (WSJT-X, WsprryPi, wsprcan) - see
 * docs/wspr-phase0-research.md for the corroboration. (Those are protocol
 * constants - facts about what WSPR transmits, not copyrightable code.)
 */
#include <stdint.h>
#include "wspr_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WSPR_ENC_BITS 81 /* 50 message bits + 31 zero flush bits */
#define WSPR_NSYM     162 /* = 2 * WSPR_ENC_BITS, one FSK tone per symbol */

/* WSPR's 162-bit sync vector (0/1, one bit per channel symbol), in CHANNEL
 * (post-interleave) order - this is the low bit of each transmitted tone
 * index. Cross-verified byte-identical against WSJT-X's, WsprryPi's, and
 * wsprcan's own pr3[]/npr3[] arrays. */
extern const uint8_t wspr_sync_vector[WSPR_NSYM];

/* Convolutionally encode the 50-bit message (+ 31 implicit zero flush bits)
 * into 162 raw ENCODE-ORDER bits (0/1, NOT yet interleaved, NOT yet
 * combined with the sync vector). Matches WSJT-X's own encode()/ENCODE
 * macro bit-for-bit. */
void wspr_convolve_encode(const wspr_msg_bytes_t *msg,
                           uint8_t raw_bits_out[WSPR_NSYM]);

/* Permute 162 encode-order bits into channel-slot order, and the inverse.
 * Both use the same bit-reversal-of-an-8-bit-index permutation WSPR's own
 * interleaver uses (self-inverse in the sense that the same lookup table
 * drives both directions - see the .c file). */
void wspr_interleave(const uint8_t raw_bits[WSPR_NSYM],
                      uint8_t channel_bits_out[WSPR_NSYM]);
void wspr_deinterleave(const uint8_t channel_bits[WSPR_NSYM],
                        uint8_t raw_bits_out[WSPR_NSYM]);

/* Combine interleaved data bits with the sync vector to produce the 4-FSK
 * tone index (0-3, tone = sync_bit | (data_bit<<1)) actually transmitted
 * for each of the 162 channel slots - and the inverse, which strips a KNOWN
 * sync bit back out to recover the interleaved data bit alone. */
void wspr_symbols_to_tones(const uint8_t channel_bits[WSPR_NSYM],
                            uint8_t tones_out[WSPR_NSYM]);
void wspr_tones_to_symbols(const uint8_t tones[WSPR_NSYM],
                            uint8_t channel_bits_out[WSPR_NSYM]);

/* A simple two-level (0 or 255) hard-decision metric table - enough to
 * prove the Fano decoder against noiseless/quantized test vectors. A real
 * AWGN-tuned soft table, built from an actual per-symbol SNR estimate, is
 * Phase 1 follow-up work once real captured audio is in the loop (see
 * docs/wspr-phase0-research.md's "what Phase 1 should pull next"). */
void wspr_build_hard_metric_table(int mettab[2][256]);

/* Fano-decode 162 already-DEINTERLEAVED soft metric values (encode order,
 * mettab-indexable 0-255) back to the 50-bit message. Clean-room
 * implementation of the Fano algorithm - see wspr_fano.c's header comment.
 *
 * `delta` is the threshold-adjust step; `maxcycles_per_bit` bounds the search so a
 * bad candidate returns failure instead of hanging - same concern as FT8's
 * FT8_DECODE_BUDGET_MS.
 *
 * Returns 1 on success, 0 on timeout (the search never satisfied the
 * threshold within budget - `*metric_out`/`*cycles_out` are still filled in
 * on a timeout, for diagnostics). */
int wspr_fano_decode(const uint8_t deinterleaved_soft[WSPR_NSYM],
                      int mettab[2][256], int delta,
                      unsigned int maxcycles_per_bit,
                      wspr_msg_bytes_t *msg_out,
                      unsigned int *metric_out, unsigned int *cycles_out);

#ifdef __cplusplus
}
#endif
