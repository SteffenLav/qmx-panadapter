#ifndef _INCLUDE_JS8_H_
#define _INCLUDE_JS8_H_

#include <stdint.h>
#include <stdbool.h>

#include "constants.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* JS8 "Normal" (submode A) codec primitives.
 *
 * Normal shares FT8's physical layer exactly - 12000 Hz, NSPS 1920, 79 symbols
 * of 21 sync + 58 data, 15 s cycle - and differs in the tables. Everything
 * here is taken from JS8Call 2.3.1 (tag v2.3.1, fd721e8b67ee):
 * lib/js8/genjs8.f90, lib/js8/js8dec.f90, lib/js8/js8a_params.f90,
 * lib/ft8/encode174.f90, lib/ft8/bpdecode174.f90, lib/ft8/chkcrc12a.f90,
 * lib/crc12.cpp.
 *
 * Three differences from FT8 are easy to miss and each one reads as "never
 * decodes" if it is got wrong:
 *
 *  1. NO GRAY MAP. FT8 maps a 3-bit group through kFT8_Gray_map; JS8 uses
 *     straight binary - genjs8.f90 builds the tone as c0*4 + c1*2 + c2, and
 *     js8dec.f90's bit metrics group the tones the same plain-binary way.
 *  2. The code is (174,87), a different code from FT8's (174,91) - not a
 *     different rate of the same one - and its codeword columns are
 *     PERMUTED by colorder after encoding.
 *  3. The CRC-12 is XORed with 42 after computation (genjs8.f90, and
 *     chkcrc12a.f90 does the same before comparing).
 */

#define JS8_LDPC_N      (174) ///< Codeword bits
#define JS8_LDPC_K      (87)  ///< Payload bits: 75 message + 12 CRC
#define JS8_LDPC_M      (87)  ///< Parity checks
#define JS8_MSG_BITS    (75)  ///< 72-bit varicode frame + 3-bit transmission type
#define JS8_FRAME_BITS  (72)  ///< The varicode frame itself
#define JS8_CRC_BITS    (12)
#define JS8_CRC_POLY    (0xC06u) ///< Truncated poly, lib/crc12.cpp
#define JS8_CRC_XOR     (42u)    ///< Applied after computation, genjs8.f90

#define JS8_NN          (79) ///< Channel symbols
#define JS8_ND          (58) ///< Data symbols
#define JS8_NS          (21) ///< Sync symbols (3 x Costas 7)

/// Costas 7x7, NCOSTAS=1 ("original") - the same array for all three blocks.
extern const uint8_t kJS8_Costas_pattern[7];

/* ⭐ JS8 HAS NO GRAY CODING, and this identity map is how that is said in
 * code rather than as a special case in the inner loop.
 *
 * FT8 maps each 3-bit group through kFT8_Gray_map before choosing a tone;
 * genjs8.f90 builds the tone as c0*4 + c1*2 + c2, and js8dec.f90's bit metrics
 * group the tones the same plain-binary way. Feeding the decoder this map
 * instead of a branch keeps the extraction function shared and makes the
 * difference a table, where every other FT8-vs-JS8 difference already lives. */
extern const uint8_t kJS8_Gray_map[8];

/// The (174,87) code, for bp_decode_code().
extern const ftx_ldpc_code_t kJS8_LDPC_code_174_87;

/// Dense generator matrix: row i gives parity bit i as a dot product over the
/// 87 payload bits, bitpacked MSB first. Bit 87 of each row is padding and is
/// zero.
extern const uint8_t kJS8_LDPC_generator[JS8_LDPC_M][11];

/// Column permutation applied after encoding, 0-based (ldpc_174_87_params.f90).
extern const uint8_t kJS8_LDPC_colorder[JS8_LDPC_N];

/// Encode 87 payload bits (75 message + 12 CRC) into a 174-bit codeword.
/// Mirrors lib/ft8/encode174.f90 including the colorder permutation.
void js8_encode174(const uint8_t payload[JS8_LDPC_K], uint8_t codeword[JS8_LDPC_N]);

/// CRC-12 over `nbits` bits of `data` (MSB first), polynomial `poly`, initial
/// remainder 0, no reflection. The caller supplies the augmentation: JS8 feeds
/// 88 bits whose last 13 are zero. Exposed with an explicit polynomial so the
/// harness can check the engine against a catalogued CRC-12.
uint16_t js8_crc12_raw(const uint8_t* data, int nbits, uint16_t poly);

/// Build the 87-bit payload: 72-bit frame, 3-bit transmission type, CRC-12.
/// `frame` is 9 bytes, MSB first. Output is one bit per byte, 0 or 1.
void js8_pack_payload(const uint8_t frame[9], uint8_t itype, uint8_t payload[JS8_LDPC_K]);

/// Recompute the CRC over a received payload and compare it with the carried
/// one. Returns true when they match.
bool js8_check_payload_crc(const uint8_t payload[JS8_LDPC_K]);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_JS8_H_
