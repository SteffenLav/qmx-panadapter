#ifndef _INCLUDE_LDPC_H_
#define _INCLUDE_LDPC_H_

#include <stdint.h>

#include "constants.h"

#ifdef __cplusplus
extern "C"
{
#endif

// codeword is 174 log-likelihoods.
// plain is a return value, 174 ints, to be 0 or 1.
// iters is how hard to try.
// ok == 87 means success.
void ldpc_decode(float codeword[], int max_iters, uint8_t plain[], int* ok);

// stall_limit: abandon a candidate after that many iterations with no
// improvement; 0 keeps the original "run every iteration" behaviour.
// The measurement that set it is at the definition in ldpc.c.
// Decodes with the (174,91) code - FT8 and FT4.
void bp_decode(float codeword[], int max_iters, int stall_limit, uint8_t plain[], int* ok);

// As bp_decode(), but for an explicit code. `code` selects the parity-check
// tables, so a second code (JS8's (174,87)) shares this engine rather than
// getting a copy of it. *ok is the number of UNSATISFIED checks, so success is
// 0 and the worst case is code->M - do not compare it against a literal 87.
void bp_decode_code(const ftx_ldpc_code_t* code, float codeword[], int max_iters, int stall_limit,
                    uint8_t plain[], int* ok);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_LDPC_H_
