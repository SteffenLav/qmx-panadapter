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

/* ⛔ APPEND ONLY. ft8_op_mode_protocol() and the settings layer carry these
 * as integers, so re-ordering silently changes what a saved configuration
 * means. New protocols go at the end. */
typedef enum
{
    FTX_PROTOCOL_FT4,
    FTX_PROTOCOL_FT8,
    FTX_PROTOCOL_JS8,       ///< JS8 "Normal" (submode A) - FT8's timing, different tables
    FTX_PROTOCOL_JS8_FAST,  ///< JS8 submode B - 1200 NSPS, 10 s, 10 Hz, MODIFIED Costas
    FTX_PROTOCOL_JS8_TURBO, ///< JS8 submode C -  600 NSPS,  6 s, 20 Hz, MODIFIED Costas
    FTX_PROTOCOL_JS8_SLOW   ///< JS8 submode E - 3840 NSPS, 30 s, 3.125 Hz, MODIFIED Costas
} ftx_protocol_t;

/* ⚠ ULTRA (submode I, 384 NSPS, 4 s, 31.25 Hz) IS DELIBERATELY ABSENT.
 * JS8Call 2.3.1 defines it and then disables it in its own calling code
 * (JS8Submode.cpp: "a known, but unused, submode"). Adding a mode the
 * reference implementation will not transmit gives an operator a control with
 * nobody on the other end. */

/* ⛔ ASK THESE, DO NOT WRITE "proto == FTX_PROTOCOL_FT4 ? a : b".
 *
 * That shape was in fourteen places when JS8 was added, and every one of them
 * put JS8 on the FT8 branch. For the slot period that is RIGHT - JS8 Normal is
 * a 15 s cycle, the same as FT8 - but it is right BY ACCIDENT, and the next
 * protocol or the next question will not be so lucky. A two-way ternary cannot
 * express a three-way fact, and it fails silently when it is wrong.
 *
 * So the facts live here, once, and the call sites ask. */

/* ⭐ THE SUBMODE FACTS, from JS8Call 2.3.1 (tag v2.3.1), commons.h and
 * lib/js8/js8[abce]_params.f90, cross-checked against JS8Submode.cpp:
 *
 *   submode  NSPS  period  start delay  tone spacing  symbol  signal  Costas
 *   Normal A 1920   15 s      500 ms      6.250 Hz    160 ms  12.64s  ORIGINAL
 *   Fast   B 1200   10 s      200 ms     10.000 Hz    100 ms   7.90s  MODIFIED
 *   Turbo  C  600    6 s      100 ms     20.000 Hz     50 ms   3.95s  MODIFIED
 *   Slow   E 3840   30 s      500 ms      3.125 Hz    320 ms  25.28s  MODIFIED
 *
 * tone spacing = 12000 / NSPS and symbol period = NSPS / 12000, exactly, so
 * the two are reciprocal and neither is rounded.
 *
 * ⚠ TWO SOURCES DISAGREE ABOUT SLOW AND ONLY ONE IS THE PERIOD.
 * js8e_params.f90 says NTXDUR=28 while commons.h says JS8E_TX_SECONDS=30.
 * They are different quantities: JS8Submode.cpp takes the PERIOD from
 * JS8E_TX_SECONDS (30 s), and the Fortran's NTXDUR sizes the DECODER'S
 * analysis window (NMAX = NTXDUR*12000), which trims Slow's dead tail. For
 * every other submode the two are equal, so the distinction is invisible
 * there. Reading the params file alone gives 28 and is wrong for the slot. */

/// Slot period in milliseconds. FT8 and JS8 Normal 15000, FT4 7500,
/// JS8 Fast 10000, Turbo 6000, Slow 30000. All divide 60 s.
int ftx_protocol_slot_ms(ftx_protocol_t protocol);

/// Symbol period in microseconds: NSPS / 12000. Exact for every protocol here.
int ftx_protocol_symbol_period_us(ftx_protocol_t protocol);

/// Tone spacing in Hz: 12000 / NSPS. The reciprocal of the symbol period.
float ftx_protocol_tone_spacing_hz(ftx_protocol_t protocol);

/* How far into the slot JS8Call starts its own transmission
 * (JS8[ABCE]_START_DELAY_MS). Zero for FT8/FT4, which key on the boundary.
 *
 * ⚠ NOT APPLIED BY THIS FIRMWARE TODAY. JS8 Normal has been decoded on air
 * by real stations with key-down at +1 ms, so JS8Call's decoder plainly
 * tolerates the 500 ms difference at 160 ms symbols. Whether it still does at
 * Turbo's 50 ms symbols is unmeasured. Exposed here so the question has a
 * number attached rather than being rediscovered. */
int ftx_protocol_start_delay_ms(ftx_protocol_t protocol);

/* True for JS8 in ANY submode.
 *
 * ⛔ USE THIS, NOT `protocol == FTX_PROTOCOL_JS8`. That spelling means
 * "JS8 Normal specifically" and there were 29 of it when the submodes were
 * added - every one of which meant "is this JS8 at all". The compiler cannot
 * tell the two apart, so the distinction has to be in the name. */
bool ftx_protocol_is_js8(ftx_protocol_t protocol);

/// JS8Call's own submode number (NSUBMODE): A=0, B=1, C=2, E=4. -1 for FT8/FT4.
/// For labels and the API only - it never goes on the air.
int ftx_protocol_js8_submode(ftx_protocol_t protocol);

/* The three 7-symbol Costas arrays for the sync blocks at symbols 0, 36 and
 * 72. Normal uses ONE array three times (NCOSTAS=1, "original"); every other
 * submode uses THREE DIFFERENT ones (NCOSTAS=2, "modified") - genjs8.f90.
 * Returns NULL for FT8/FT4, which have their own tables. */
const uint8_t (*ftx_protocol_js8_costas(ftx_protocol_t protocol))[7];

/// Tones per symbol: FT4 is 4-FSK, FT8 and JS8 are 8-FSK.
int ftx_protocol_num_tones(ftx_protocol_t protocol);

/// Channel symbols in one burst: FT4 105, FT8 and every JS8 submode 79.
/// burst length = num_symbols * symbol period, which is how ft8_slot_gate.c
/// derives every timing gate rather than tabulating them per protocol.
int ftx_protocol_num_symbols(ftx_protocol_t protocol);

/* ⛔ THE IDENTITY: "FT8", "FT4", "JS8" - and EVERY JS8 submode is "JS8".
 * This is the ADIF MODE field, the PSK Reporter mode and what the web page
 * compares against; it is not a place to say which submode is running. Never
 * allocated. See the long note at the definition for what broke when it did
 * name the submodes. */
const char* ftx_protocol_name(ftx_protocol_t protocol);

/* The human label, which DOES name the submode ("JS8 Turbo"). Logs, the
 * drawer, the API's display field - never ADIF, never the air. */
const char* ftx_protocol_label(ftx_protocol_t protocol);

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
