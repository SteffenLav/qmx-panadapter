//
// LDPC decoder for FT8.
//
// given a 174-bit codeword as an array of log-likelihood of zero,
// return a 174-bit corrected codeword, or zero-length array.
// last 87 bits are the (systematic) plain-text.
// this is an implementation of the sum-product algorithm
// from Sarah Johnson's Iterative Error Correction book.
// codeword[i] = log ( P(x=0) / P(x=1) )
//

#include "ldpc.h"
#include "constants.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>

static int ldpc_check(uint8_t codeword[]);
static float fast_tanh(float x);
static float fast_atanh(float x);

// codeword is 174 log-likelihoods.
// plain is a return value, 174 ints, to be 0 or 1.
// max_iters is how hard to try.
// ok == 87 means success.
void ldpc_decode(float codeword[], int max_iters, uint8_t plain[], int* ok)
{
    float m[FTX_LDPC_M][FTX_LDPC_N]; // ~60 kB
    float e[FTX_LDPC_M][FTX_LDPC_N]; // ~60 kB
    int min_errors = FTX_LDPC_M;

    for (int j = 0; j < FTX_LDPC_M; j++)
    {
        for (int i = 0; i < FTX_LDPC_N; i++)
        {
            m[j][i] = codeword[i];
            e[j][i] = 0.0f;
        }
    }

    for (int iter = 0; iter < max_iters; iter++)
    {
        for (int j = 0; j < FTX_LDPC_M; j++)
        {
            for (int ii1 = 0; ii1 < kFTX_LDPC_Num_rows[j]; ii1++)
            {
                int i1 = kFTX_LDPC_Nm[j][ii1] - 1;
                float a = 1.0f;
                for (int ii2 = 0; ii2 < kFTX_LDPC_Num_rows[j]; ii2++)
                {
                    int i2 = kFTX_LDPC_Nm[j][ii2] - 1;
                    if (i2 != i1)
                    {
                        a *= fast_tanh(-m[j][i2] / 2.0f);
                    }
                }
                e[j][i1] = -2.0f * fast_atanh(a);
            }
        }

        for (int i = 0; i < FTX_LDPC_N; i++)
        {
            float l = codeword[i];
            for (int j = 0; j < 3; j++)
                l += e[kFTX_LDPC_Mn[i][j] - 1][i];
            plain[i] = (l > 0) ? 1 : 0;
        }

        int errors = ldpc_check(plain);

        if (errors < min_errors)
        {
            // Update the current best result
            min_errors = errors;

            if (errors == 0)
            {
                break; // Found a perfect answer
            }
        }

        for (int i = 0; i < FTX_LDPC_N; i++)
        {
            for (int ji1 = 0; ji1 < 3; ji1++)
            {
                int j1 = kFTX_LDPC_Mn[i][ji1] - 1;
                float l = codeword[i];
                for (int ji2 = 0; ji2 < 3; ji2++)
                {
                    if (ji1 != ji2)
                    {
                        int j2 = kFTX_LDPC_Mn[i][ji2] - 1;
                        l += e[j2][i];
                    }
                }
                m[j1][i] = l;
            }
        }
    }

    *ok = min_errors;
}

//
// does a 174-bit codeword pass the FT8's LDPC parity checks?
// returns the number of parity errors.
// 0 means total success.
//
static int ldpc_check(uint8_t codeword[])
{
    int errors = 0;

    for (int m = 0; m < FTX_LDPC_M; ++m)
    {
        uint8_t x = 0;
        for (int i = 0; i < kFTX_LDPC_Num_rows[m]; ++i)
        {
            x ^= codeword[kFTX_LDPC_Nm[m][i] - 1];
        }
        if (x != 0)
        {
            ++errors;
        }
    }
    return errors;
}

/* ⭐ stall_limit: give up after this many iterations with NO improvement in
 * min_errors. 0 disables it and restores the original behaviour.
 *
 * The only other exit is SUCCESS (errors == 0), so without this a candidate
 * that will never decode burns every one of max_iters. On a busy band most of
 * the 140 candidates are exactly that, and they, not the decodes, are what
 * dec_ms is spent on - measured 2026-10-04 at 0.13 ms for a failure against
 * 0.02 ms for a success on the host bench.
 *
 * A candidate that is still converging keeps lowering min_errors, so it is
 * never cut short. What a stall limit costs is the candidate that sits on a
 * plateau and then breaks through late.
 *
 * Measured over the 35 ground-truth WAVs in test/wav_reference with
 * test/ft8_decode_bench.c (cand=140, min score 5, 30 iters - the device's own
 * settings), on top of the multi-symbol fallback already being off:
 *
 *     stall off   405 decodes   565.6 ms
 *     stall 8     403 decodes   340.3 ms     <- chosen
 *     stall 5     396 decodes   263.3 ms
 *     stall 3     388 decodes   191.3 ms
 *
 * 8 costs 2 decodes in 405 (0.5%) for 40% of the time.
 *
 * ⛔ FT8 ONLY, and that is deliberate. The corpus is all FT8; FT4 uses a
 * DIFFERENT parity matrix and is not measured here. An FT8-derived LDPC limit
 * was applied to FT4 once before - FT8_LDPC_MAX_ITERS 30->15, 2026-06-29 -
 * and it badly hurt FT4 decode, confirmed by a field report immediately after
 * release. ftx_decode_candidate() passes 0 for FT4 until someone measures it
 * with an FT4 corpus. */
void bp_decode(float codeword[], int max_iters, int stall_limit, uint8_t plain[], int* ok)
{
    float tov[FTX_LDPC_N][3];
    float toc[FTX_LDPC_M][7];

    int min_errors = FTX_LDPC_M;
    int last_improve = 0;

    // initialize message data
    for (int n = 0; n < FTX_LDPC_N; ++n)
    {
        tov[n][0] = tov[n][1] = tov[n][2] = 0;
    }

    for (int iter = 0; iter < max_iters; ++iter)
    {
        // Do a hard decision guess (tov=0 in iter 0)
        int plain_sum = 0;
        for (int n = 0; n < FTX_LDPC_N; ++n)
        {
            plain[n] = ((codeword[n] + tov[n][0] + tov[n][1] + tov[n][2]) > 0) ? 1 : 0;
            plain_sum += plain[n];
        }

        if (plain_sum == 0)
        {
            // message converged to all-zeros, which is prohibited
            break;
        }

        // Check to see if we have a codeword (check before we do any iter)
        int errors = ldpc_check(plain);

        if (errors < min_errors)
        {
            // we have a better guess - update the result
            min_errors = errors;

            if (errors == 0)
            {
                break; // Found a perfect answer
            }
            last_improve = iter;
        }
        else if (stall_limit > 0 && (iter - last_improve) >= stall_limit)
        {
            // No better guess for stall_limit iterations - this one is not
            // going to converge, and the remaining iterations are pure cost.
            break;
        }

        // Send messages from bits to check nodes
        for (int m = 0; m < FTX_LDPC_M; ++m)
        {
            for (int n_idx = 0; n_idx < kFTX_LDPC_Num_rows[m]; ++n_idx)
            {
                int n = kFTX_LDPC_Nm[m][n_idx] - 1;
                // for each (n, m)
                float Tnm = codeword[n];
                for (int m_idx = 0; m_idx < 3; ++m_idx)
                {
                    if ((kFTX_LDPC_Mn[n][m_idx] - 1) != m)
                    {
                        Tnm += tov[n][m_idx];
                    }
                }
                toc[m][n_idx] = fast_tanh(-Tnm / 2);
            }
        }

        // send messages from check nodes to variable nodes
        for (int n = 0; n < FTX_LDPC_N; ++n)
        {
            for (int m_idx = 0; m_idx < 3; ++m_idx)
            {
                int m = kFTX_LDPC_Mn[n][m_idx] - 1;
                // for each (n, m)
                float Tmn = 1.0f;
                for (int n_idx = 0; n_idx < kFTX_LDPC_Num_rows[m]; ++n_idx)
                {
                    if ((kFTX_LDPC_Nm[m][n_idx] - 1) != n)
                    {
                        Tmn *= toc[m][n_idx];
                    }
                }
                tov[n][m_idx] = -2 * fast_atanh(Tmn);
            }
        }
    }

    *ok = min_errors;
}

// Ideas for approximating tanh/atanh:
// * https://varietyofsound.wordpress.com/2011/02/14/efficient-tanh-computation-using-lamberts-continued-fraction/
// * http://functions.wolfram.com/ElementaryFunctions/ArcTanh/10/0001/
// * https://mathr.co.uk/blog/2017-09-06_approximating_hyperbolic_tangent.html
// * https://math.stackexchange.com/a/446411

static float fast_tanh(float x)
{
    if (x < -4.97f)
    {
        return -1.0f;
    }
    if (x > 4.97f)
    {
        return 1.0f;
    }
    float x2 = x * x;
    // float a = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    // float b = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
    // float a = x * (10395.0f + x2 * (1260.0f + x2 * 21.0f));
    // float b = 10395.0f + x2 * (4725.0f + x2 * (210.0f + x2));
    float a = x * (945.0f + x2 * (105.0f + x2));
    float b = 945.0f + x2 * (420.0f + x2 * 15.0f);
    return a / b;
}

static float fast_atanh(float x)
{
    float x2 = x * x;
    // float a = x * (-15015.0f + x2 * (19250.0f + x2 * (-5943.0f + x2 * 256.0f)));
    // float b = (-15015.0f + x2 * (24255.0f + x2 * (-11025.0f + x2 * 1225.0f)));
    // float a = x * (-1155.0f + x2 * (1190.0f + x2 * -231.0f));
    // float b = (-1155.0f + x2 * (1575.0f + x2 * (-525.0f + x2 * 25.0f)));
    float a = x * (945.0f + x2 * (-735.0f + x2 * 64.0f));
    float b = (945.0f + x2 * (-1050.0f + x2 * 225.0f));
    return a / b;
}
