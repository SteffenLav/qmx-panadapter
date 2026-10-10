#ifndef _INCLUDE_JS8_MESSAGE_H_
#define _INCLUDE_JS8_MESSAGE_H_

#include <stdint.h>
#include <stdbool.h>

#include "js8.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* JS8 message pack/unpack - J2 of docs/js8-feasibility.md.
 *
 * Two frame types carry a whole QSO: FrameHeartbeat for CQ, FrameDirected for
 * the report exchange and the sign-off. Each is 72 bits; js8_pack_payload()
 * from js8.h adds the 3-bit transmission type and the CRC-12 to make the 87
 * the LDPC code takes.
 *
 * Everything here is read off JS8Call 2.3.1 (tag v2.3.1, fd721e8b67ee)
 * varicode.cpp: packCallsign, packAlphaNumeric50, packGrid, packNum,
 * packDirectedMessage and packCompoundFrame.
 *
 * ⛔ NO INDEPENDENT ARBITER EXISTS FOR THIS LAYER. J1's LDPC work could be
 * checked against JS8Call's own Fortran compiled with gfortran; varicode.cpp
 * is Qt C++ and the repository ships no test vectors, so the harness can only
 * prove self-consistency plus a handful of values computed by hand from the
 * published formulas. A real JS8Call station decoding our frame is the only
 * thing that settles it - and NOT a second Tab5, which would share our
 * mistakes exactly.
 *
 * ⚠ The feasibility note had the Directed layout wrong. It described
 * "cmd(5) + portable_from(1) + portable_to(1) + num(6)" as four fields; the
 * source packs the last three as ONE 8-bit value, and the frame begins with a
 * 3-bit frame-type flag that the note placed outside it. The real layouts are
 * at the two pack functions below.
 */

/* Frame type. ⛔ THE FIELD IS NOT ALWAYS 3 BITS WIDE.
 *
 * Types 0..3 use three bits. The data frames use only the top TWO, and the
 * third bit is payload - so a frame beginning 10x is a data frame whichever
 * way x falls, and the same for 11x. The protocol does this because no other
 * type starts with a 1, so that bit is free to carry message content.
 *
 * Reading all three bits and treating the result as eight distinct types was
 * wrong in two ways at once: it produced phantom types 5 and 7 that cannot
 * mean anything, and it would have split one sender's data frames across two
 * buckets depending on the value of a payload bit. Use js8_frame_type(),
 * which collapses them. */
typedef enum {
    JS8_FRAME_HEARTBEAT         = 0,  /* [000] CQ and HB, carries a grid */
    JS8_FRAME_COMPOUND          = 1,  /* [001] not implemented - out of scope */
    JS8_FRAME_COMPOUND_DIRECTED = 2,  /* [010] not implemented - out of scope */
    JS8_FRAME_DIRECTED          = 3,  /* [011] the exchange */
    JS8_FRAME_DATA              = 4,  /* [10x] free text, per-character coding */
    JS8_FRAME_DATA_COMPRESSED   = 6,  /* [11x] free text, word-indexed coding */
} js8_frame_type_t;

/* True for the two data frames, so callers do not have to know which codes
 * those are. */
static inline bool js8_frame_is_data(js8_frame_type_t t)
{
    return t == JS8_FRAME_DATA || t == JS8_FRAME_DATA_COMPRESSED;
}

/* ---- the 3-bit transmission type (itype) ------------------------------
 *
 * ⛔ THIS IS NOT THE FRAME TYPE. js8_frame_type_t above is read from the
 * FIRST BITS OF THE 72-BIT FRAME and says what the frame carries. These three
 * bits are a SEPARATE field appended after the frame, before the CRC
 * (js8_pack_payload), and they say where the frame sits in a multi-frame
 * TRANSMISSION. Confusing the two is easy and silent.
 *
 * From JS8Call's varicode.h, verbatim:
 *
 *     enum TransmissionType {
 *         JS8Call      = 0, // [000] <- any other frame of the message
 *         JS8CallFirst = 1, // [001] <- the first frame of a message
 *         JS8CallLast  = 2, // [010] <- the last frame of a message
 *         JS8CallData  = 4, // [100] <- flagged frame (no frame type header)
 *     };
 *
 * and mainwindow.cpp sets them per frame as it sends:
 *
 *     if (m_txFrameCountSent > 0)   bits &= ~Varicode::JS8CallFirst;
 *     if (m_txFrameQueue.isEmpty()) bits |=  Varicode::JS8CallLast;
 *
 * so they are FLAGS, and a single-frame message is FIRST|LAST. Zero means a
 * middle frame - the one value a standalone message must NOT use. */
#define JS8_ITYPE_MIDDLE  0
#define JS8_ITYPE_FIRST   1
#define JS8_ITYPE_LAST    2
#define JS8_ITYPE_SINGLE  (JS8_ITYPE_FIRST | JS8_ITYPE_LAST)   /* 3 */

#define JS8_FRAME_BYTES 9     /* 72 bits, MSB first */

/* The directed-command vocabulary, by its index. Only the ones a plain QSO
 * needs are named here; the rest are listed in js8_message.c so a RECEIVED
 * command can still be spelled out rather than shown as a number. */
#define JS8_CMD_SNR_QUERY  0
#define JS8_CMD_GRID       15
#define JS8_CMD_RR         21
#define JS8_CMD_SNR        25
#define JS8_CMD_NO         26
#define JS8_CMD_YES        27
#define JS8_CMD_73         28

/* "No number in this frame". packNum maps -30..+31 onto 1..62, so 0 is free
 * and is what the reference sends when the command carries no number. */
#define JS8_NUM_NONE (-128)

typedef struct {
    char    from[16];      /* our callsign, or a group token like "@ALLCALL" */
    char    to[16];
    uint8_t cmd;           /* 0..31, the directed_cmds index */
    int     num;           /* -30..+31, or JS8_NUM_NONE */
    bool    portable_from; /* the callsign ended "/P", stripped into this bit */
    bool    portable_to;
} js8_directed_t;

typedef struct {
    char    call[16];
    char    grid[5];       /* 4 characters, or "" for none */
    bool    is_cq;         /* true = CQ, false = HB */
    uint8_t bits3;         /* which CQ/HB wording, 0..7 - see js8_cq_text() */
} js8_heartbeat_t;

/* [3][28][28][5][8] = 72. The last byte is portable_from<<7 |
 * portable_to<<6 | packNum(num). Returns false when either callsign cannot be
 * packed, which is how the reference reports it too (packed == 0). */
bool js8_pack_directed(const js8_directed_t* msg, uint8_t frame[JS8_FRAME_BYTES]);
bool js8_unpack_directed(const uint8_t frame[JS8_FRAME_BYTES], js8_directed_t* out);

/* [3][50][11][8] = 72, where the 11 and the top 5 of the last byte are one
 * 16-bit value: bit 15 = CQ (not HB), bits 14..0 = the packed grid or 0x7FFF
 * for none. The bottom 3 bits of the last byte are the wording index. */
bool js8_pack_heartbeat(const js8_heartbeat_t* msg, uint8_t frame[JS8_FRAME_BYTES]);
bool js8_unpack_heartbeat(const uint8_t frame[JS8_FRAME_BYTES], js8_heartbeat_t* out);

/* The frame type without unpacking the rest - the first 3 bits. */
js8_frame_type_t js8_frame_type(const uint8_t frame[JS8_FRAME_BYTES]);

/* Spelling of a received command, e.g. 25 -> "SNR", 28 -> "73". Returns a
 * string literal, never allocated, "?" for an index with no wording. */
const char* js8_cmd_text(uint8_t cmd);

/* Spelling of a Heartbeat frame's bits3, e.g. CQ 0 -> "CQ CQ CQ". */
const char* js8_cq_text(bool is_cq, uint8_t bits3);

/* ---- the pieces, exposed because each one can be wrong on its own -------- */

/* 28-bit callsign. *portable is set when a trailing "/P" was stripped.
 * Returns 0 for anything that will not pack, as the reference does. */
uint32_t js8_pack_callsign(const char* call, bool* portable);
void     js8_unpack_callsign(uint32_t packed, bool portable, char out[16]);

/* 50-bit callsign, used by Heartbeat frames only. A different codec from the
 * 28-bit one: base 38 over an 11-character field with a '/' flag at positions
 * 3 and 7, so it carries compound calls the 28-bit codec cannot. */
uint64_t js8_pack_alnum50(const char* call);
void     js8_unpack_alnum50(uint64_t packed, char out[16]);

/* 15-bit grid. 0x7FFF means "no grid" and is what an empty or short locator
 * packs to. */
uint16_t js8_pack_grid(const char* grid);
void     js8_unpack_grid(uint16_t packed, char out[5]);

/* -30..+31 to 1..62; anything outside clamps. JS8_NUM_NONE packs to 0. */
uint8_t js8_pack_num(int num);
int     js8_unpack_num(uint8_t packed);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_JS8_MESSAGE_H_
