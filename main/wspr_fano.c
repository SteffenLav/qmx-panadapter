#include "wspr_fano.h"
#include <stdlib.h>
#include <string.h>

/* K=32 rate-1/2 "Layland-Lushbaugh" generator polynomials - the ones WSPR
 * actually uses (WSJT-X's fano.c also lists NASA-standard and
 * Massey-Johannesson alternatives behind #ifdef, unused by WSPR). */
#define WSPR_POLY1 0xf2d05351UL
#define WSPR_POLY2 0xe4613c47UL

const uint8_t wspr_sync_vector[WSPR_NSYM] = {
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,
    0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,
    0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,
    1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,
    0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,
    0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,
    0,1,0,0,0,1,1,1,0,0,0,0,0,1,0,1,0,0,1,1,
    0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,
    0,0
};

static int parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (int)(v & 1);
}

void wspr_convolve_encode(const wspr_msg_bytes_t *msg,
                           uint8_t raw_bits_out[WSPR_NSYM])
{
    uint32_t state = 0;
    for (int i = 0; i < WSPR_ENC_BITS; i++) {
        int bit;
        if (i < 50) {
            bit = (msg->dat[i / 8] >> (7 - (i % 8))) & 1;
        } else {
            bit = 0; /* flush */
        }
        state = (state << 1) | (uint32_t)bit;
        raw_bits_out[2 * i]     = (uint8_t)parity32(state & WSPR_POLY1);
        raw_bits_out[2 * i + 1] = (uint8_t)parity32(state & WSPR_POLY2);
    }
}

/* Bit-reversal of an 8-bit value - the permutation WSPR's interleaver is
 * built from (see wspr_interleave_map() below). */
static uint8_t reverse8(uint8_t v)
{
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

/* interleave_map[i] = the channel-slot position that encode-order bit i
 * moves to. Built by walking k=0..255, keeping the reversed values <162 in
 * order - that scan order is what makes this the SAME permutation on both
 * encode and decode: wspr_interleave() writes channel[map[i]] = raw[i],
 * wspr_deinterleave() reads raw[i] = channel[map[i]] - same table, just
 * read in the direction that matches which array is "known". */
static void build_interleave_map(uint8_t map_out[WSPR_NSYM])
{
    int idx = 0;
    for (int k = 0; k < 256 && idx < WSPR_NSYM; k++) {
        uint8_t j = reverse8((uint8_t)k);
        if (j < WSPR_NSYM) {
            map_out[idx++] = j;
        }
    }
}

void wspr_interleave(const uint8_t raw_bits[WSPR_NSYM],
                      uint8_t channel_bits_out[WSPR_NSYM])
{
    uint8_t map[WSPR_NSYM];
    build_interleave_map(map);
    for (int i = 0; i < WSPR_NSYM; i++) {
        channel_bits_out[map[i]] = raw_bits[i];
    }
}

void wspr_deinterleave(const uint8_t channel_bits[WSPR_NSYM],
                        uint8_t raw_bits_out[WSPR_NSYM])
{
    uint8_t map[WSPR_NSYM];
    build_interleave_map(map);
    for (int i = 0; i < WSPR_NSYM; i++) {
        raw_bits_out[i] = channel_bits[map[i]];
    }
}

void wspr_deinterleave_scores(const double channel_scores[WSPR_NSYM],
                               double raw_scores_out[WSPR_NSYM])
{
    uint8_t map[WSPR_NSYM];
    build_interleave_map(map);
    for (int i = 0; i < WSPR_NSYM; i++) {
        raw_scores_out[i] = channel_scores[map[i]];
    }
}

void wspr_symbols_to_tones(const uint8_t channel_bits[WSPR_NSYM],
                            uint8_t tones_out[WSPR_NSYM])
{
    for (int i = 0; i < WSPR_NSYM; i++) {
        tones_out[i] = (uint8_t)(wspr_sync_vector[i] | (channel_bits[i] << 1));
    }
}

void wspr_tones_to_symbols(const uint8_t tones[WSPR_NSYM],
                            uint8_t channel_bits_out[WSPR_NSYM])
{
    for (int i = 0; i < WSPR_NSYM; i++) {
        channel_bits_out[i] = (uint8_t)((tones[i] >> 1) & 1);
    }
}

void wspr_build_hard_metric_table(int mettab[2][256])
{
    /* A Fano metric must give a WRONG path a negative expected increment
     * (that's what lets the threshold/backtrack logic tell wrong from
     * right at all) — a plain match=1/mismatch=0 table doesn't have that
     * property and measurably fails to correct even single-symbol errors
     * (17/24 in early testing here). match=1/mismatch=-3 gives a random
     * (50/50) branch an expected value of -1 per bit, which is enough for
     * this K=32 code to correct 1- and 2-symbol errors 100% of the time in
     * testing (test/wspr_codec_harness.c) and degrade gracefully beyond
     * that — the normal shape for a convolutional code at its correction
     * limit, not a bug. rx < 128 reads as a received '0', >= 128 as a
     * received '1' (matches a caller using the two-level 0/255 soft
     * mapping in the harness). A real AWGN-tuned table, built from an
     * actual per-symbol SNR estimate, is still Phase 1 follow-up work once
     * real captured audio is in the loop. */
    for (int rx = 0; rx < 256; rx++) {
        mettab[0][rx] = (rx < 128) ? 1 : -3;
        mettab[1][rx] = (rx < 128) ? -3 : 1;
    }
}

/* Generated by test/wspr_metric_sim.c: empirical log-likelihood-ratio
 * metric, pooled from Monte Carlo simulation of this decoder's own
 * per-symbol tone-power-difference statistic (D) across 8 amplitudes
 * (200,000 total trials), each batch normalized by its own mean(|D|)
 * before quantizing - see wspr_fano.h's usage note. Own simulation, not
 * copied from any published table (that distinction is why this table
 * exists at all rather than K9AN's - see docs/wspr-phase1-status.md's
 * licensing section). Measured (test/wspr_metric_sim.c's self-check,
 * single-message sweep): correct decode from +10.3 dB down to -24.2 dB
 * SNR (2500 Hz reference bandwidth), first failure at -26.4 dB - roughly
 * 2-3 dB more sensitive than wspr_build_hard_metric_table()'s -22.7 dB. */
static const int kSoftMetric[2][256] = {
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,1,0,0,1,1,2,2,2,3,
   3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
   3,3,3,3,3,3,3,3,2,1,0,0,0,0,0,0,
   0,0,0,0,0,0,-2,-4,-6,-9,-12,-15,-18,-20,-22,-25,
   -26,-29,-31,-35,-40,-35,-31,-28,-26,-24,-22,-21,-19,-18,-16,-14,
   -12,-10,-7,-6,-4,-3,-2,0,-2,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,-2,0,0,-2,-2,-3,-6,-8,-11,
   -11,-14,-16,-17,-18,-21,-22,-24,-26,-28,-31,-35,-40,-35,-31,-29,
   -27,-25,-22,-20,-18,-15,-13,-11,-4,-2,0,0,0,0,0,0,
   0,0,0,0,0,0,1,2,2,3,3,3,3,3,3,3,
   3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
   3,3,2,2,2,2,1,0,1,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

void wspr_build_soft_metric_table(int mettab[2][256])
{
    for (int bit = 0; bit < 2; bit++) {
        for (int b = 0; b < 256; b++) {
            mettab[bit][b] = kSoftMetric[bit][b];
        }
    }
}

/* ---- Fano sequential decoder ----
 *
 * CLEAN-ROOM IMPLEMENTATION. Fano's sequential decoding algorithm is public
 * academic material dating to R. Fano's 1963 paper - decades before WSJT-X
 * existed - and is described in numerous independent textbook/encyclopedia
 * sources (e.g. the Wikipedia "Sequential decoding" article, standard
 * coding-theory course notes). An earlier version of this file ported
 * WSJT-X's own fano.c (Phil Karn KA9Q 1994 / Joe Taylor K1JT) "near-
 * verbatim" - that file ships as part of the GPLv3-licensed WSJT-X source
 * tree, and this project is MIT-licensed, so that port was a licensing
 * mistake, caught and corrected here (see docs/wspr-phase1-status.md).
 * What follows is written from the algorithm's published RULES (which
 * aren't anyone's copyrightable expression), using this module's own data
 * layout and control flow - not Karn's `struct node` array-of-precomputed-
 * branch-metrics design. The one deliberately-preserved algorithmic detail
 * is that the message's known 31-bit all-zero flush tail constrains the
 * search (a `1` bit there can never be the correct answer, so it's a
 * correctness matter, not a style choice) - see the WSPR_FLUSH_BITS use
 * below.
 *
 * The rules, in this implementation's own terms:
 *  - The path is a sequence of chosen bits from depth 0 (nothing decided)
 *    to depth WSPR_ENC_BITS (message + flush fully decided). At each
 *    depth d we track the cumulative Fano metric gamma[d] and the
 *    convolutional encoder's state entering depth d.
 *  - A dynamic threshold T (a multiple of `delta`) gates which moves are
 *    allowed: from depth d, a candidate next bit is acceptable only if
 *    gamma[d] + branch_metric >= T.
 *  - At each depth we always try the BETTER of the two candidate bits
 *    first; only if we return to that depth after failing deeper do we try
 *    the WORSE one. `stage[d]` tracks which of those has been attempted.
 *  - Reaching a NEW deepest point ever visited tightens T upward (as far
 *    as it can go while staying <= the new gamma) - this is what stops the
 *    search wandering forever on a good path.
 *  - When both bits at the current depth fail T, back up one depth. If
 *    that shallower depth's own gamma is itself below T (or we're already
 *    at the root), no further backing up is meaningful at this threshold -
 *    T must be loosened instead, and depth 0 gets a fresh look with the
 *    lower threshold, since T loosening is what makes previously-rejected
 *    branches reachable again.
 */

static int fano_encode_step(uint32_t encstate)
{
    int p1 = parity32(encstate & WSPR_POLY1);
    int p2 = parity32(encstate & WSPR_POLY2);
    return (p1 << 1) | p2;
}

typedef enum { STAGE_FRESH, STAGE_TRIED_BETTER, STAGE_EXHAUSTED } fano_stage_t;

/* Whether depth d (given its stage) still has an untried candidate bit
 * under the CURRENT threshold - shared by both the forward-attempt logic
 * and the backward cascade so the two can never disagree about what
 * "exhausted" means. `allow_bit1` is false in the known-all-zero flush
 * tail, where there is only ever one legal bit to try. */
static int has_untried_branch(fano_stage_t stage, int allow_bit1)
{
    if (stage == STAGE_FRESH) return 1;
    if (stage == STAGE_TRIED_BETTER && allow_bit1) return 1;
    return 0;
}

/* A metric SOURCE abstracts "how do we score extending the path at depth
 * d with bit 0 or bit 1" - either a fixed byte-quantized lookup table
 * (the original design) or arbitrary precomputed per-position values (for
 * per-symbol reliability weighting, which a fixed table can't express at
 * all - see wspr_fano_decode_weighted() and docs/wspr-phase1-status.md).
 * Both public entry points share the one search loop below through this
 * abstraction, so the two can't drift apart in behavior. */
typedef struct {
    const uint8_t *soft;      /* mettab path: deinterleaved soft bytes, or NULL */
    int (*mettab)[256];
    const int (*precomputed)[2]; /* weighted path: per-raw-position [bit0,bit1] metrics, or NULL */
} metric_source_t;

/* Fills branch_metric[0], branch_metric[1] with the Fano metric of
 * extending the path at depth d (whose encoder state is `enc_state`) with
 * input bit 0 or bit 1 respectively, using whichever metric source is
 * active. Encode step d covers raw/deinterleaved-order positions 2d and
 * 2d+1 (POLY1's and POLY2's output bits respectively). */
static void branch_metrics(uint32_t enc_state, const metric_source_t *src,
                            unsigned int d, int branch_metric[2])
{
    for (int bit = 0; bit < 2; bit++) {
        uint32_t next_state = (enc_state << 1) | (uint32_t)bit;
        int code = fano_encode_step(next_state); /* 2 bits: (POLY1<<1)|POLY2 */
        int bit_hi = (code >> 1) & 1, bit_lo = code & 1;
        if (src->precomputed) {
            branch_metric[bit] = src->precomputed[2 * d][bit_hi]
                                + src->precomputed[2 * d + 1][bit_lo];
        } else {
            const uint8_t *sym = &src->soft[2 * d];
            branch_metric[bit] = src->mettab[bit_hi][sym[0]] + src->mettab[bit_lo][sym[1]];
        }
    }
}

static int fano_search(const metric_source_t *src, int delta,
                        unsigned int maxcycles_per_bit,
                        wspr_msg_bytes_t *msg_out,
                        unsigned int *metric_out, unsigned int *cycles_out)
{
    const unsigned int N = WSPR_ENC_BITS;
    const unsigned int WSPR_FLUSH_START = N - 31; /* depths >= this: bit must be 0 */

    long *gamma = (long *)malloc((size_t)(N + 1) * sizeof(long));
    uint32_t *enc_state = (uint32_t *)malloc((size_t)(N + 1) * sizeof(uint32_t));
    fano_stage_t *stage = (fano_stage_t *)malloc((size_t)(N + 1) * sizeof(fano_stage_t));
    if (!gamma || !enc_state || !stage) {
        free(gamma); free(enc_state); free(stage);
        return 0;
    }

    unsigned int d = 0;
    gamma[0] = 0;
    enc_state[0] = 0;
    stage[0] = STAGE_FRESH;
    long T = 0;
    unsigned int deepest_reached = 0;
    unsigned int maxcycles = maxcycles_per_bit * N;
    unsigned int cycle;
    int success = 0;

    for (cycle = 1; cycle <= maxcycles; cycle++) {
        int allow_bit1 = (d < WSPR_FLUSH_START);

        if (has_untried_branch(stage[d], allow_bit1)) {
            int bm[2];
            branch_metrics(enc_state[d], src, d, bm);
            /* In the flush region bit 1 can never be correct, so it isn't
             * a real candidate at all - forcing better_bit=0 there (rather
             * than letting a noisy bm[1]>bm[0] pick it) keeps the search
             * from ever proposing an impossible flush value. */
            int better_bit = (!allow_bit1 || bm[0] >= bm[1]) ? 0 : 1;
            int try_bit = (stage[d] == STAGE_FRESH) ? better_bit : (1 - better_bit);

            long trial_gamma = gamma[d] + bm[try_bit];
            stage[d] = (try_bit == better_bit) ? STAGE_TRIED_BETTER : STAGE_EXHAUSTED;

            if (trial_gamma >= T) {
                d++;
                gamma[d] = trial_gamma;
                enc_state[d] = (enc_state[d - 1] << 1) | (uint32_t)try_bit;
                stage[d] = STAGE_FRESH;
                if (d > deepest_reached) {
                    deepest_reached = d;
                    while (T + delta <= trial_gamma) T += delta;
                }
                if (d == N) { success = 1; break; }
            }
            /* else: attempt failed the threshold: stage[d] is already
             * updated above, so next cycle either tries the remaining
             * branch here or (if that was the last one) falls into the
             * backtrack case below. Depth doesn't move. */
            continue;
        }

        /* Depth d has no untried branch left under the current threshold.
         * Cascade backward through ancestors that are ALSO exhausted,
         * stopping either at one with something left to try, or at the
         * point where going back further is meaningless (root, or the
         * next ancestor's own gamma is already below T) - at which point
         * T loosens and THIS depth (not the root) gets a fresh look,
         * since a lower threshold is what can make its already-tried
         * branches viable again. Not restarting from the root is what
         * keeps this from re-walking the whole tree on every loosening. */
        for (;;) {
            if (d == 0 || gamma[d - 1] < T) {
                T -= delta;
                stage[d] = STAGE_FRESH;
                break;
            }
            d--;
            if (has_untried_branch(stage[d], d < WSPR_FLUSH_START)) break;
        }
    }

    if (metric_out) *metric_out = (unsigned int)gamma[d];
    if (cycles_out) *cycles_out = cycle;

    if (msg_out) {
        memset(msg_out->dat, 0, sizeof(msg_out->dat));
        if (success) {
            /* enc_state[k] holds, in its low 8 bits, input bits (k-8..k-1]
             * once k>=8 (a 32-bit shift register that's had at least 8
             * fresh bits shifted in packs them MSB-first in the low byte -
             * the same reasoning as the encode side's bit ordering). Byte
             * b of the message is enc_state[8*(b+1)]'s low byte. */
            for (int b = 0; b < 7; b++) {
                unsigned int k = 8 * (unsigned int)(b + 1);
                if (k <= N) msg_out->dat[b] = (uint8_t)enc_state[k];
            }
        }
    }

    free(gamma);
    free(enc_state);
    free(stage);
    return success ? 1 : 0;
}

int wspr_fano_decode(const uint8_t deinterleaved_soft[WSPR_NSYM],
                      int mettab[2][256], int delta,
                      unsigned int maxcycles_per_bit,
                      wspr_msg_bytes_t *msg_out,
                      unsigned int *metric_out, unsigned int *cycles_out)
{
    metric_source_t src = { deinterleaved_soft, mettab, NULL };
    return fano_search(&src, delta, maxcycles_per_bit, msg_out, metric_out, cycles_out);
}

int wspr_fano_decode_weighted(const int branch_metric[WSPR_NSYM][2],
                               int delta, unsigned int maxcycles_per_bit,
                               wspr_msg_bytes_t *msg_out,
                               unsigned int *metric_out, unsigned int *cycles_out)
{
    metric_source_t src = { NULL, NULL, branch_metric };
    return fano_search(&src, delta, maxcycles_per_bit, msg_out, metric_out, cycles_out);
}
