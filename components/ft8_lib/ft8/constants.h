#ifndef _INCLUDE_CONSTANTS_H_
#define _INCLUDE_CONSTANTS_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define FT8_SYMBOL_PERIOD (0.160f) ///< FT8 symbol duration, defines tone deviation in Hz and symbol rate
#define FT8_SLOT_TIME     (15.0f)  ///< FT8 slot period

#define FT4_SYMBOL_PERIOD (0.048f) ///< FT4 symbol duration, defines tone deviation in Hz and symbol rate
#define FT4_SLOT_TIME     (7.5f)   ///< FT4 slot period

// Define FT8 symbol counts
// FT8 message structure:
//     S D1 S D2 S
// S  - sync block (7 symbols of Costas pattern)
// D1 - first data block (29 symbols each encoding 3 bits)
#define FT8_ND          (58) ///< Data symbols
#define FT8_NN          (79) ///< Total channel symbols (FT8_NS + FT8_ND)
#define FT8_LENGTH_SYNC (7)  ///< Length of each sync group
#define FT8_NUM_SYNC    (3)  ///< Number of sync groups
#define FT8_SYNC_OFFSET (36) ///< Offset between sync groups

// Define FT4 symbol counts
// FT4 message structure:
//     R Sa D1 Sb D2 Sc D3 Sd R
// R  - ramping symbol (no payload information conveyed)
// Sx - one of four _different_ sync blocks (4 symbols of Costas pattern)
// Dy - data block (29 symbols each encoding 2 bits)
#define FT4_ND          (87)  ///< Data symbols
#define FT4_NR          (2)   ///< Ramp symbols (beginning + end)
#define FT4_NN          (105) ///< Total channel symbols (FT4_NS + FT4_ND + FT4_NR)
#define FT4_LENGTH_SYNC (4)   ///< Length of each sync group
#define FT4_NUM_SYNC    (4)   ///< Number of sync groups
#define FT4_SYNC_OFFSET (33)  ///< Offset between sync groups

// Define LDPC parameters
#define FTX_LDPC_N       (174)                  ///< Number of bits in the encoded message (payload with LDPC checksum bits)
#define FTX_LDPC_K       (91)                   ///< Number of payload bits (including CRC)
#define FTX_LDPC_M       (83)                   ///< Number of LDPC checksum bits (FTX_LDPC_N - FTX_LDPC_K)
#define FTX_LDPC_N_BYTES ((FTX_LDPC_N + 7) / 8) ///< Number of whole bytes needed to store 174 bits (full message)
#define FTX_LDPC_K_BYTES ((FTX_LDPC_K + 7) / 8) ///< Number of whole bytes needed to store 91 bits (payload + CRC only)

/* Largest M over every code the decoder is built with. FT8/FT4 use (174,91),
 * M=83. JS8 Normal uses (174,87), M=87 - a different code, not a different
 * rate of the same one. bp_decode() sizes its working arrays by this so the
 * stack frame does not depend on which protocol is being decoded. */
#define FTX_LDPC_M_MAX (87)

/* A parity-check code, as the belief-propagation decoder needs it.
 *
 * ldpc.c used to name kFTX_LDPC_Nm/Mn/Num_rows directly, which bound the
 * engine to FT8's (174,91) code. The loops were already generic over the
 * dimensions; only the table SYMBOLS were fixed. Passing the tables in is what
 * lets a second code share the engine. N is 174 for every code here, so it
 * stays a compile-time constant. */
typedef struct
{
    int M;                   ///< Number of parity checks (rows)
    const uint8_t (*Nm)[7];  ///< [M][7]  bit indices of each check, 1-based; 0 pads a short row
    const uint8_t (*Mn)[3];  ///< [FTX_LDPC_N][3] check indices of each bit, 1-based
    const uint8_t* Num_rows; ///< [M] number of used entries in the matching Nm row
} ftx_ldpc_code_t;

/// The (174,91) code used by FT8 and FT4.
extern const ftx_ldpc_code_t kFTX_LDPC_code_174_91;

// Define CRC parameters
#define FT8_CRC_POLYNOMIAL ((uint16_t)0x2757u) ///< CRC-14 polynomial without the leading (MSB) 1
#define FT8_CRC_WIDTH      (14)

typedef enum
{
    FTX_PROTOCOL_FT4,
    FTX_PROTOCOL_FT8,
    FTX_PROTOCOL_JS8   ///< JS8 "Normal" - FT8's timing, different tables
} ftx_protocol_t;

/* ⛔ ASK THESE, DO NOT WRITE "proto == FTX_PROTOCOL_FT4 ? a : b".
 *
 * That shape was in fourteen places when JS8 was added, and every one of them
 * put JS8 on the FT8 branch. For the slot period that is RIGHT - JS8 Normal is
 * a 15 s cycle, the same as FT8 - but it is right BY ACCIDENT, and the next
 * protocol or the next question will not be so lucky. A two-way ternary cannot
 * express a three-way fact, and it fails silently when it is wrong.
 *
 * So the facts live here, once, and the call sites ask. */

/// Slot period in milliseconds: FT8 and JS8 15000, FT4 7500.
int ftx_protocol_slot_ms(ftx_protocol_t protocol);

/// Tones per symbol: FT4 is 4-FSK, FT8 and JS8 are 8-FSK.
int ftx_protocol_num_tones(ftx_protocol_t protocol);

/// "FT8", "FT4", "JS8" - for logs and labels, never allocated.
const char* ftx_protocol_name(ftx_protocol_t protocol);

/* Can this build DECODE and TRANSMIT the protocol?
 *
 * ⛔ FALSE FOR JS8 TODAY, AND THAT IS THE POINT. J1 and J2 built the codec and
 * the message layer; nothing has taught decode.c to extract JS8's likelihoods
 * or checked its CRC-12, and ft8_tx.c cannot build its tones. Selecting JS8
 * must REFUSE loudly rather than quietly behave as FT8 - a mode that silently
 * sends something else is worse than a mode that is missing. */
bool ftx_protocol_is_implemented(ftx_protocol_t protocol);

/// Costas 7x7 tone pattern for synchronization
extern const uint8_t kFT8_Costas_pattern[7];
extern const uint8_t kFT4_Costas_pattern[4][4];

/// Gray code map to encode 8 symbols (tones)
extern const uint8_t kFT8_Gray_map[8];
extern const uint8_t kFT4_Gray_map[4];

extern const uint8_t kFT4_XOR_sequence[10];

/// Parity generator matrix for (174,91) LDPC code, stored in bitpacked format (MSB first)
extern const uint8_t kFTX_LDPC_generator[FTX_LDPC_M][FTX_LDPC_K_BYTES];

/// LDPC(174,91) parity check matrix, containing 83 rows,
/// each row describes one parity check,
/// each number is an index into the codeword (1-origin).
/// The codeword bits mentioned in each row must xor to zero.
/// From WSJT-X's ldpc_174_91_c_reordered_parity.f90.
extern const uint8_t kFTX_LDPC_Nm[FTX_LDPC_M][7];

/// Mn from WSJT-X's bpdecode174.f90. Each row corresponds to a codeword bit.
/// The numbers indicate which three parity checks (rows in Nm) refer to the codeword bit.
/// The numbers use 1 as the origin (first entry).
extern const uint8_t kFTX_LDPC_Mn[FTX_LDPC_N][3];

/// Number of rows (columns in C/C++) in the array Nm.
extern const uint8_t kFTX_LDPC_Num_rows[FTX_LDPC_M];

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_CONSTANTS_H_
