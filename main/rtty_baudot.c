/* ITA2 / Baudot codec - see rtty_baudot.h. T196. */
#include "rtty_baudot.h"

#include <ctype.h>
#include <stddef.h>

/* 0 = prints nothing (blank, or a shift code at 27/31).
 * Cross-checked 2026-11-12 against the bit-order form of ITA2 (bits 1..5):
 * A 11000, B 10011, E 10000, Q 11101, T 00001, Z 10001, M 00111, CR 00010,
 * LF 01000, SP 00100 - each matches the value here read LSB-first. */
static const char LTRS[32] = {
    0,   'E', '\n', 'A', ' ', 'S', 'I', 'U',
    '\r','D', 'R', 'J', 'N', 'F', 'C', 'K',
    'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q',
    'O', 'B', 'G', 0,   'M', 'X', 'V', 0,
};
/* Digits follow the QWERTYUIOP -> 1234567890 row: Q=1 ... P=0. */
static const char FIGS[32] = {
    0,   '3', '\n', '-', ' ', '\a','8', '7',
    '\r','$', '4', '\'',',', '!', ':', '(',
    '5', '"', ')', '2', '#', '6', '0', '1',
    '9', '?', '&', 0,   '.', '/', ';', 0,
};

void rtty_baudot_init(rtty_baudot_t *b, bool usos)
{
    b->figs = false;
    b->usos = usos;
}

int rtty_baudot_decode(rtty_baudot_t *b, uint8_t code)
{
    code &= 0x1F;
    if (code == RTTY_CODE_LTRS) { b->figs = false; return 0; }
    if (code == RTTY_CODE_FIGS) { b->figs = true;  return 0; }
    char c = b->figs ? FIGS[code] : LTRS[code];
    if (c == ' ' && b->usos) b->figs = false;
    return (unsigned char)c;
}

static int find(const char *t, char c)
{
    for (int i = 0; i < 32; i++)
        if (t[i] && t[i] == c && i != RTTY_CODE_FIGS && i != RTTY_CODE_LTRS) return i;
    return -1;
}

int rtty_baudot_encode(rtty_baudot_t *b, char c, uint8_t out[2])
{
    c = (char)toupper((unsigned char)c);
    if (c == 0) return -1;
    int n = 0;
    int l = find(LTRS, c), f = find(FIGS, c);
    if (l >= 0 && f >= 0) {
        /* SP, CR, LF: identical in both tables, no shift needed. */
        out[n++] = (uint8_t)l;
        if (c == ' ' && b->usos) b->figs = false;   /* the receiver unshifts too */
        return n;
    }
    if (l >= 0) {
        if (b->figs) { out[n++] = RTTY_CODE_LTRS; b->figs = false; }
        out[n++] = (uint8_t)l;
        return n;
    }
    if (f >= 0) {
        if (!b->figs) { out[n++] = RTTY_CODE_FIGS; b->figs = true; }
        out[n++] = (uint8_t)f;
        return n;
    }
    return -1;
}
