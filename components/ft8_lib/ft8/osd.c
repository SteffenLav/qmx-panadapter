/* Order-0 ordered-statistics decoding for the (174,91) FT8 LDPC code.
 * See osd.h for what it is worth, what it costs, and where 34 comes from. */

#include "osd.h"

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

bool ft8_osd_order0(const float log174[N], int max_ndis, uint8_t plain174[N])
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

    /* Re-encode: the codeword is the sum of the reduced rows whose basis bit
     * is set. Column basis[k] is 1 only in row k after the reduction, so the
     * result agrees with the hard decision on every basis position. */
    row_t acc; acc.w[0] = acc.w[1] = acc.w[2] = 0;
    for (int k = 0; k < K; k++)
    {
        if (hard[basis[k]]) rxor(&acc, &M[k]);
    }

    int ndis = 0;
    for (int i = 0; i < N; i++)
    {
        plain174[i] = (uint8_t)rget(&acc, i);
        if (plain174[i] != hard[i]) ndis++;
    }

    if (max_ndis > 0 && ndis > max_ndis) return false;
    return true;
}
