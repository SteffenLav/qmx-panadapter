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

/* ---- Fano sequential decoder ----
 * Ported near-verbatim from WSJT-X's own lib/wsprd/fano.c (Phil Karn KA9Q
 * 1994, modifications by Joe Taylor K1JT). Structure and variable names
 * intentionally kept close to the original so this can be diffed against
 * the source if WSJT-X's decoder is ever revised. */

struct fano_node {
    uint32_t encstate; /* encoder state of next node */
    long gamma;        /* cumulative metric to this node */
    int metrics[4];    /* metrics indexed by all possible tx syms */
    int tm[2];         /* sorted metrics for current hypotheses */
    int i;             /* current branch being tested */
};

static int fano_encode_step(uint32_t encstate)
{
    int p1 = parity32(encstate & WSPR_POLY1);
    int p2 = parity32(encstate & WSPR_POLY2);
    return (p1 << 1) | p2;
}

int wspr_fano_decode(const uint8_t deinterleaved_soft[WSPR_NSYM],
                      int mettab[2][256], int delta,
                      unsigned int maxcycles_per_bit,
                      wspr_msg_bytes_t *msg_out,
                      unsigned int *metric_out, unsigned int *cycles_out)
{
    const unsigned int nbits = WSPR_ENC_BITS;
    struct fano_node *nodes = (struct fano_node *)malloc(
        (nbits + 1) * sizeof(struct fano_node));
    if (!nodes) return 0;

    struct fano_node *lastnode = &nodes[nbits - 1];
    struct fano_node *tail = &nodes[nbits - 31];
    struct fano_node *np;
    int t, m0, m1;
    long ngamma;
    unsigned int lsym;
    unsigned int i;
    unsigned int maxcycles = maxcycles_per_bit * nbits;

    for (np = nodes; np <= lastnode; np++) {
        const uint8_t *sym = &deinterleaved_soft[2 * (np - nodes)];
        np->metrics[0] = mettab[0][sym[0]] + mettab[0][sym[1]];
        np->metrics[1] = mettab[0][sym[0]] + mettab[1][sym[1]];
        np->metrics[2] = mettab[1][sym[0]] + mettab[0][sym[1]];
        np->metrics[3] = mettab[1][sym[0]] + mettab[1][sym[1]];
    }

    np = nodes;
    np->encstate = 0;
    lsym = (unsigned int)fano_encode_step(np->encstate);
    m0 = np->metrics[lsym];
    m1 = np->metrics[3 ^ lsym];
    if (m0 > m1) {
        np->tm[0] = m0;
        np->tm[1] = m1;
    } else {
        np->tm[0] = m1;
        np->tm[1] = m0;
        np->encstate++;
    }
    np->i = 0;
    np->gamma = t = 0;

    for (i = 1; i <= maxcycles; i++) {
        ngamma = np->gamma + np->tm[np->i];
        if (ngamma >= t) {
            if (np->gamma < t + delta) {
                while (ngamma >= t + delta) t += delta;
            }
            np[1].gamma = ngamma;
            np[1].encstate = np->encstate << 1;
            if (++np == (lastnode + 1)) {
                break; /* done */
            }
            lsym = (unsigned int)fano_encode_step(np->encstate);
            if (np >= tail) {
                np->tm[0] = np->metrics[lsym];
            } else {
                m0 = np->metrics[lsym];
                m1 = np->metrics[3 ^ lsym];
                if (m0 > m1) {
                    np->tm[0] = m0;
                    np->tm[1] = m1;
                } else {
                    np->tm[0] = m1;
                    np->tm[1] = m0;
                    np->encstate++;
                }
            }
            np->i = 0;
            continue;
        }
        /* threshold violated - look backward */
        for (;;) {
            if (np == nodes || np[-1].gamma < t) {
                t -= delta;
                if (np->i != 0) {
                    np->i = 0;
                    np->encstate ^= 1;
                }
                break;
            }
            if (--np < tail && np->i != 1) {
                np->i++;
                np->encstate ^= 1;
                break;
            }
        }
    }

    if (metric_out) *metric_out = (unsigned int)np->gamma;
    if (cycles_out) *cycles_out = i + 1;

    unsigned int out_bits = (nbits >> 3) * 8; /* matches original's nbits>>=3 truncation */
    unsigned int out_bytes = out_bits / 8;
    np = &nodes[7];
    uint8_t tmp[16];
    memset(tmp, 0, sizeof(tmp));
    for (unsigned int b = 0; b < out_bytes && b < sizeof(tmp); b++) {
        tmp[b] = (uint8_t)np->encstate;
        np += 8;
    }
    if (msg_out) {
        memset(msg_out->dat, 0, sizeof(msg_out->dat));
        for (int b = 0; b < 7 && (unsigned)b < out_bytes; b++) {
            msg_out->dat[b] = tmp[b];
        }
    }

    int timed_out = (i >= maxcycles);
    free(nodes);
    return timed_out ? 0 : 1;
}
