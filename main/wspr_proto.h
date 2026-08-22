#pragma once
/* WSPR type-1 message packing/unpacking — callsign + 4-char grid + power (dBm)
 * <-> the 50-bit message WSPR actually transmits.
 *
 * Portable on purpose (no ESP deps) so test/wspr_codec_harness.c can link the
 * REAL functions rather than a copy of them — same convention as
 * main/util/db_gridlines.c and main/ft8_msg_guard.c.
 *
 * wspr_unpack_message() is ported BYTE-IDENTICAL (not re-derived) from
 * WSJT-X's own lib/wsprd/wsprd_utils.c (unpack50/unpackcall/unpackgrid) —
 * that is the authoritative decoder, so it is the ground truth this module
 * is tested against. wspr_pack_message() is built from two independently-
 * maintained encoder sources (WsprryPi's wspr.cpp for the callsign mixed-
 * radix packing + power correction table, robertostling/wspr-tools'
 * encode.py for the grid-locator formula) — see docs/wspr-phase0-research.md
 * for the full source list and cross-check. The harness proves the two
 * halves agree by round-tripping pack -> unpack for many messages; that is
 * the real test, not this comment.
 *
 * Only the plain 6-character "standard callsign" type-1 message is
 * implemented (matches the scope doc: compound/hashed calls are a type-3
 * message, out of scope for a first cut).
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The 50-bit WSPR message, byte-packed exactly the way WSJT-X's own
 * unpack50()/encode() expect: dat[0..3] hold the 28-bit callsign field
 * (dat[3] split across bit 4), dat[3..6] hold the 22-bit grid+power field,
 * and the low 6 bits of dat[6] are zero padding (NOT part of the 31-bit
 * convolutional-encoder flush — that's separate, see wspr_fano.h). */
typedef struct {
    uint8_t dat[7];
} wspr_msg_bytes_t;

/* Pack a standard callsign + 4-char Maidenhead grid + power (dBm) into the
 * 50-bit WSPR message. `callsign` must be a "standard" callsign: 4-6
 * characters, matching a pattern with exactly one digit forming the numeral
 * (e.g. "K1ABC", "OZ1LAV", "W1AW") — no leading/trailing space needed, this
 * function finds the digit itself. `grid` must be exactly 4 characters,
 * two letters (A-R, case-insensitive) then two digits (e.g. "FN20").
 * `power_dbm` is rounded to the nearest legal WSPR power codepoint (the
 * same non-uniform table WSJT-X itself uses) and clamped to 0..60.
 *
 * Returns 1 on success, 0 if the callsign or grid doesn't fit the standard
 * shape (caller should refuse to transmit, not guess).
 */
int wspr_pack_message(const char *callsign, const char *grid, int power_dbm,
                       wspr_msg_bytes_t *out);

/* Inverse of wspr_pack_message() — ported from WSJT-X's own unpack50() /
 * unpackcall() / unpackgrid(). `callsign_out` and `grid_out` must each be at
 * least 7 and 5 bytes respectively. Returns 1 on success, 0 if the packed
 * value is out of range (which should never happen for a value this module
 * itself packed, but can for arbitrary/corrupt decoder output). */
int wspr_unpack_message(const wspr_msg_bytes_t *in, char callsign_out[7],
                         char grid_out[5], int *power_dbm_out);

#ifdef __cplusplus
}
#endif
