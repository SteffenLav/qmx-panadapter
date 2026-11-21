#ifndef _INCLUDE_OSD_H_
#define _INCLUDE_OSD_H_

#include <stdint.h>
#include <stdbool.h>
#include "constants.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ORDER-0 ORDERED-STATISTICS DECODING for the (174,91) FT8 LDPC code.
 *
 * What it is for: a candidate whose belief-propagation decode FAILED. OSD
 * re-solves it from the 91 most reliable soft bits instead of iterating, so
 * it recovers messages BP cannot. It cannot help a candidate find_sync never
 * produced - see test/ft8_osd_reach_harness.c, which measured that every
 * missed ground-truth message in the reference corpus does have a candidate,
 * so the whole gap is addressable here.
 *
 * MEASURED WORTH, 22 ground-truth reference WAVs
 * (test/ft8_osd_proto_harness.c, 2026-11-20):
 *
 *   order 0, no subtraction:  260 -> 264 of 362 ground-truth messages
 *   cost: 104.8 calls a slot (one per failing candidate), 2.82 ms a slot on
 *         a host, 0.0269 ms a call
 *
 * ⭐ ADDITIVE WITH SUBTRACT-AND-REPEAT: +24 for subtraction, +4 for this,
 * +28 together - they fix different failures. ⛔ Order 1 is NOT additive with
 * it: order 1 and subtraction recover the SAME messages, so with subtraction
 * on, order 1 is worth nothing over order 0. That is why only order 0 is
 * here.
 *
 * ⚠ COST ON THE P4 IS AN ESTIMATE, NOT A MEASUREMENT. 2.82 ms scaled by the
 * measured host-to-Tab5 decode ratio (1800 / 37.3 = 48.3x) is ~136 ms a slot
 * against FT8_DECODE_BUDGET_MS. Probably optimistic: that ratio came from an
 * FFT-and-LDPC workload, this is GF(2) row reduction, and the reduction runs
 * on uint64 words that a 32-bit RISC-V splits in two with no SIMD to recover
 * it. Even at 4x the estimate it fits, but nobody has run it on a P4.
 *
 * ⛔ WHY THIS NEEDS NO PSRAM, unlike the subtract pass. No second STFT, no
 * second monitor, no private copy of the slot audio. It reads the log174 the
 * caller already has and works in ~3.4 KB of STACK - see the note on
 * ft8_osd_order0() below, which is the one real cost to watch.
 */

/* Reject an OSD codeword disagreeing with the hard decision in more than this
 * many of the 174 bits. 34 is calibrated - this file records the four statistics
 * that were compared and why this one won. ⚠ FITTED on 32 samples from the
 * same 22 files, not confirmed on held-out data.
 * Moved here from decode.c so the decoder and the init log share ONE copy.
 *
 * ⛔ NOW 0 = NO SCREEN, 2026-11-21. The 34 was fitted where the false accepts
 * are - ORDER 1, ~92 trials a candidate. The decoder runs ORDER 0, one trial,
 * and there the screen only cost true decodes (tools/ft8_pair_ab.py, the same
 * audio both ways, subtract pass on):
 *   reference corpus, 22 WAVs   no screen 288, ndis<=34 285 of 362 truth
 *   live 40 m, 33 slots         +3 decodes, 0 lost: SP6CVB PD2NIK RR73,
 *                               KC9BK F1PSX JN08, K4DKW G6PWS IO91 (repeats)
 * - prefixes match their grids, which a 14-bit CRC false accept would not
 * arrange. The expected false-accept rate of one extra trial is ~0.01 a slot;
 * re-check it on a longer recording before this ships anywhere. */
#ifndef FT8_OSD_MAX_NDIS
#define FT8_OSD_MAX_NDIS 0
#endif

/// Build the systematic generator table. Idempotent, and NOT thread-safe:
/// call it once before any decode task starts, not lazily from the decoders.
/// Both FT8 decode halves run concurrently on two cores.
void ft8_osd_init(void);

/// Order-0 OSD on a failed candidate's soft bits.
///
/// @param[in]  log174   the 174 normalized LLRs, positive meaning a ONE bit -
///                      the same convention as bp_decode(), see the note at
///                      the top of ldpc.c
/// @param[in]  max_ndis reject a codeword disagreeing with the hard decision
///                      in more than this many of the 174 bits; 0 = no limit.
///                      ⛔ 34 is the CALIBRATED value and the reason this is
///                      a parameter: of four statistics tried, the count of
///                      disagreeing bits separated real decodes from CRC
///                      false accepts best - threshold 34 keeps every real
///                      decode in the corpus and rejects 17 of 20 false ones,
///                      against 14 of 20 for the soft distance. It is FITTED
///                      on 32 samples from the same 22 files, not confirmed
///                      on held-out data; there is no held-out FT8 corpus in
///                      this tree.
/// @param[out] plain174 the recovered codeword, 0/1 per bit
/// @return true if a codeword was produced. ⛔ THE CALLER MUST STILL CHECK
///         THE CRC - this returns a valid CODEWORD, not a valid MESSAGE, and
///         the 14-bit CRC is the only thing that makes it a message. One
///         trial false-accepts with probability 2^-14.
///
/// ⚠ USES ABOUT 3.4 KB OF STACK (a 91x174-bit working matrix is 2184 B of
/// it). Check the calling task's stack before switching this on.
bool ft8_osd_order0(const float log174[FTX_LDPC_N], int max_ndis,
                    uint8_t plain174[FTX_LDPC_N]);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_OSD_H_
