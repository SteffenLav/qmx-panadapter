#pragma once
/* ITA2 (CCITT-2, "Baudot") 5-bit code for RTTY - T196.
 *
 * Code values are the 5 data bits read as a number with bit 1 (the first bit
 * sent after the start bit) as the LSB. So E = 1 (bit 1 only), T = 16 (bit 5
 * only), FIGS = 27, LTRS = 31. Table and its cross-check: docs/rtty-feasibility.md
 * "Baudot/ITA2". FIGS column is the US-TTY variant most ham software uses.
 *
 * Pure C, no ESP-IDF: the host harness (test/rtty_harness.c) links this file. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTTY_CODE_FIGS 27
#define RTTY_CODE_LTRS 31

typedef struct {
    bool figs;   /* current shift state; starts in LTRS */
    bool usos;   /* unshift on space: a SPACE also returns to LTRS */
} rtty_baudot_t;

void rtty_baudot_init(rtty_baudot_t *b, bool usos);

/* Decode one 5-bit code. Returns the ASCII character, or 0 when the code
 * prints nothing (blank, a shift code). CR returns '\r', LF '\n', BELL '\a'. */
int rtty_baudot_decode(rtty_baudot_t *b, uint8_t code);

/* Encode one ASCII character (lower case is folded to upper). Writes 0..2
 * codes to out: a shift code first when the character is in the other table.
 * Returns the count, or -1 when the character has no ITA2 code. */
int rtty_baudot_encode(rtty_baudot_t *b, char c, uint8_t out[2]);

#ifdef __cplusplus
}
#endif
