/* Order-0 ordered-statistics decoding for the (174,91) FT8 LDPC code.
 * See osd.h for what it is worth, what it costs, and where 34 comes from. */

#include "osd.h"
#include "crc.h"

#include <math.h>
#include <string.h>

#define N FTX_LDPC_N   /* 174 */
#define K FTX_LDPC_K   /*  91 */

/* A row of the generator as a bitset over the 174 columns. */
typedef struct { uint64_t w[3]; } row_t;

static inline int  rget(const row_t* r, int c) { return (int)((r->w[c >> 6] >> (c & 63)) & 1u); }
static inline void rset(row_t* r, int c)       { r->w[c >> 6] |= (uint64_t)1u << (c & 63); }
static inline void rxor(row_t* a, const row_t* b)
{
    a->w[0] ^= b->w[0]; a->w[1] ^= b->w[1]; a->w[2] ^= b->w[2];
}

/* The systematic generator, built once. Row i is e_i in the first 91 columns
 * and the parity bits of message bit i in the last 83.
 *
 * kFTX_LDPC_generator is [83][12]: row j is the parity-check row for parity
 * bit j, bitpacked MSB first over the 91 message bits. So parity column K+j
 * of generator row i is bit i of that row - the transpose, which is why this
 * reads index-swapped. encode.c walks it the other way for the same reason. */
static row_t s_G[K];
static bool  s_ready = false;

void ft8_osd_init(void)
{
    if (s_ready) return;
    memset(s_G, 0, sizeof s_G);
    for (int i = 0; i < K; i++)
    {
        rset(&s_G[i], i);
        for (int j = 0; j < FTX_LDPC_M; j++)
        {
            if ((kFTX_LDPC_generator[j][i >> 3] >> (7 - (i & 7))) & 1)
            {
                rset(&s_G[i], K + j);
            }
        }
    }
    s_ready = true;
}

/* The 91 information bits of a codeword, packed as ftx_extract_crc() wants
 * them, and the CRC check decode.c applies after BP. Same arithmetic as there:
 * the CRC is over the 77-bit message zero-extended to 82 bits. */
static bool cw_crc_ok(const uint8_t cw[N])
{
    uint8_t a91[FTX_LDPC_K_BYTES];
    memset(a91, 0, sizeof a91);
    for (int i = 0; i < K; i++)
        if (cw[i]) a91[i / 8] |= (uint8_t)(0x80u >> (i % 8));
    uint16_t extracted = ftx_extract_crc(a91);
    a91[9] &= 0xF8;
    a91[10] &= 0x00;
    return extracted == ftx_compute_crc(a91, 96 - 14);
}

bool ft8_osd_decode(const float log174[N], int order, int max_ndis, int max_ndis1,
                    uint8_t plain174[N])
{
    if (!s_ready) return false;      /* ft8_osd_init() was never called */

    /* Bit positions ordered by reliability, most reliable first. Insertion
     * sort: N is 174 and this is not where the time goes. */
    int idx[N];
    for (int i = 0; i < N; i++) idx[i] = i;
    for (int i = 1; i < N; i++)
    {
        int k = idx[i]; float v = fabsf(log174[k]); int j = i - 1;
        while (j >= 0 && fabsf(log174[idx[j]]) < v) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = k;
    }

    uint8_t hard[N];
    for (int i = 0; i < N; i++) hard[i] = (log174[i] > 0) ? 1 : 0;

    /* Most-reliable basis. Walk the columns in reliability order and keep the
     * ones that raise the rank, row-reducing as we go so each kept column
     * ends up with a single 1, in its own row. A dependent column is skipped,
     * which is the whole point: the 91 kept columns are the most reliable
     * INDEPENDENT ones, and the hard decisions on them pin down a codeword. */
    row_t M[K];
    memcpy(M, s_G, sizeof M);
    int basis[K], nb = 0;

    for (int t = 0; t < N && nb < K; t++)
    {
        int c  = idx[t];
        int pr = -1;
        for (int r = nb; r < K; r++) { if (rget(&M[r], c)) { pr = r; break; } }
        if (pr < 0) continue;

        row_t tmp = M[nb]; M[nb] = M[pr]; M[pr] = tmp;
        for (int r = 0; r < K; r++)
        {
            if (r != nb && rget(&M[r], c)) rxor(&M[r], &M[nb]);
        }
        basis[nb] = c;
        nb++;
    }
    /* The code has rank 91, so this cannot fail - but a corrupted generator
     * table would show up here rather than as a wrong decode. */
    if (nb < K) return false;

    /* Order 0: the codeword is the sum of the reduced rows whose basis bit is
     * set. Column basis[k] is 1 only in row k after the reduction, so the
     * result agrees with the hard decision on every basis position. */
    row_t acc; acc.w[0] = acc.w[1] = acc.w[2] = 0;
    for (int k = 0; k < K; k++)
    {
        if (hard[basis[k]]) rxor(&acc, &M[k]);
    }

    if (order <= 0)
    {
        /* Unchanged from the order-0-only version: no CRC here, decode.c
         * checks it on the returned codeword. */
        int ndis = 0;
        for (int i = 0; i < N; i++)
        {
            plain174[i] = (uint8_t)rget(&acc, i);
            if (plain174[i] != hard[i]) ndis++;
        }
        return !(max_ndis > 0 && ndis > max_ndis);
    }

    /* ⭐ ORDER 1: the order-0 codeword and the 91 codewords that differ from
     * it in one basis bit - flipping basis bit k is one XOR with row k. Each
     * must pass the CRC; among those that do, the smallest soft distance
     * (sum |llr| over the bits that disagree with the hard decision) wins.
     *
     * ⛔ 92 CRC trials a candidate is where false accepts come from (2^-14
     * each), so the FLIPPED codewords get their own screen, max_ndis1. It is
     * not applied to the order-0 codeword: a shared screen cost true order-0
     * decodes as it tightened. Measured 2026-11-21 on 307 recorded 40 m slots
     * against WSJT-X's jt9 as referee (tools/ft8_pair_ab.py --jt9 3), order 1
     * against order 0, test/ft8_osd_proto_harness.c -DOSD_MAX_NDIS1:
     *   flips <= 26   +105 / -5 real    +1 not in jt9
     *   flips <= 30   +137 / -8 real    +3 not in jt9     <- chosen
     *   flips <= 34   +145 / -9 real   +17 not in jt9
     *   no screen     +154 / -11 real  +299 not in jt9 (about 1 a slot)
     *
     * ⚠ STACK: M[] (91 x 24 B) plus these locals is ~3.7 KB, as order 0. */
    bool found = false;
    float best_d = 0.0f;
    row_t best = acc;
    uint8_t cw[N];
    for (int flip = -1; flip < K; flip++)
    {
        row_t t = acc;
        if (flip >= 0) rxor(&t, &M[flip]);
        for (int i = 0; i < N; i++) cw[i] = (uint8_t)rget(&t, i);
        if (!cw_crc_ok(cw)) continue;

        float d = 0.0f; int ndis = 0;
        for (int i = 0; i < N; i++)
        {
            if (cw[i] != hard[i]) { d += fabsf(log174[i]); ndis++; }
        }
        if (max_ndis > 0 && ndis > max_ndis) continue;
        if (flip >= 0 && max_ndis1 > 0 && ndis > max_ndis1) continue;
        if (!found || d < best_d) { found = true; best_d = d; best = t; }
    }
    if (!found) return false;
    for (int i = 0; i < N; i++) plain174[i] = (uint8_t)rget(&best, i);
    return true;
}

bool ft8_osd_order0(const float log174[N], int max_ndis, uint8_t plain174[N])
{
    return ft8_osd_decode(log174, 0, max_ndis, 0, plain174);
}
