#include "wspr_proto.h"
#include <string.h>
#include <stdio.h>

/* CLEAN-ROOM REWRITE. An earlier version of this file's unpack side was
 * "ported byte-identical" from WSJT-X's wsprd_utils.c (GPL v3), and the
 * pack side's grid/power formulas were sourced from two GitHub projects
 * that turned out to also be GPL-3.0 (robertostling/wspr-tools) or
 * unclear-license (JamesP6000/WsprryPi, GitHub shows "NOASSERTION", which
 * grants no rights). This is an MIT-licensed project, so none of that was
 * safe to keep - see docs/wspr-phase1-status.md for the full account.
 *
 * What follows is written from the WSPR message SPECIFICATION (bit widths,
 * the standard-callsign template, the Maidenhead grid system - all
 * protocol/geographic facts, not anyone's copyrightable code) using this
 * file's own symmetric pack/unpack design: pack and unpack now share the
 * SAME bit-width table and mixed-radix character table instead of being
 * two independently-shaped, separately-authored functions the way the
 * ported version was. That's also just better engineering - shared tables
 * can't drift out of sync with each other.
 *
 * The numeric relationships this module computes (which bit range holds
 * what, which direction the grid linearization runs) are dictated by the
 * WSPR protocol - a compliant implementation has essentially one correct
 * answer, so this isn't a creative-expression question so much as a
 * correctness one. That correctness is re-verified empirically, not just
 * asserted: test/wspr_codec_harness.c's round-trip tests, and (more
 * importantly) test/wspr_decode_harness.c decoding WSJT's own real WSPR
 * recording to the same 5 real, standard-format callsigns and legitimate
 * US-region grid squares (FN20, EL89, DM04, EL09, DM09) as before this
 * rewrite - real over-the-air interoperability is a much stronger check
 * than any single source's code ever was. */

/* ---- generic MSB-first bit packing into a byte buffer ---- */

static void put_bits(uint8_t *buf, int *bitpos, uint32_t value, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) {
        int bit = (int)((value >> i) & 1);
        int byte_idx = *bitpos / 8, shift = 7 - (*bitpos % 8);
        if (bit) buf[byte_idx] = (uint8_t)(buf[byte_idx] | (1 << shift));
        (*bitpos)++;
    }
}

static uint32_t get_bits(const uint8_t *buf, int *bitpos, int nbits)
{
    uint32_t v = 0;
    for (int i = 0; i < nbits; i++) {
        int byte_idx = *bitpos / 8, shift = 7 - (*bitpos % 8);
        int bit = (buf[byte_idx] >> shift) & 1;
        v = (v << 1) | (uint32_t)bit;
        (*bitpos)++;
    }
    return v;
}

/* ---- standard-callsign mixed-radix packing ----
 *
 * WSPR type-1's callsign field represents a 6-character template
 * [P1][P2][D][S1][S2][S3]: up to 2 leading "prefix" characters (each an
 * alphanumeric or absent), one digit (the callsign's numeral - always
 * present, this is what anchors the template), and up to 3 trailing
 * "suffix" letters (each a letter or absent). Packed as one mixed-radix
 * integer, most-significant character first - the alphabet size at each
 * position is fixed by the protocol: */
#define CALL_ALPHA_ALNUM 36 /* prefix chars: '0'-'9'=0-9, 'A'-'Z'=10-35 */
#define CALL_ALPHA_DIGIT 10 /* the anchor digit: '0'-'9'=0-9 */
#define CALL_ALPHA_LETTER 27 /* suffix chars: 'A'-'Z'=0-25, absent=26 */
#define CALL_ABSENT_ALNUM 36
#define CALL_ABSENT_LETTER 26

static int alnum_value(char c) /* '0'-'9'->0-9, 'A'-'Z'->10-35, else -1 */
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}
static int letter_value(char c) /* 'A'-'Z'->0-25, else -1 */
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' : -1;
}
static char alnum_char(int v) /* inverse of alnum_value, incl. the "absent" value */
{
    if (v == CALL_ABSENT_ALNUM) return ' ';
    return (v < 10) ? (char)('0' + v) : (char)('A' + v - 10);
}
static char letter_char(int v) /* inverse of letter_value, incl. "absent" */
{
    return (v == CALL_ABSENT_LETTER) ? ' ' : (char)('A' + v);
}

/* Locate the anchor digit within an up-to-6-char callsign: the digit must
 * leave at most 2 characters before it (the prefix) and at most 3 letters
 * after it (the suffix, which - unlike the prefix - can never itself
 * contain a digit). That "suffix has no digits" constraint is what
 * disambiguates a callsign whose prefix itself starts with a digit (e.g.
 * "4X1XX": the '4' fails because "X1XX" isn't all-letters; the '1' at
 * index 2 succeeds). Returns the digit's index, or -1 if no position in
 * the string satisfies the template. */
static int find_anchor_digit(const char *c, int len)
{
    for (int k = 0; k < len && k <= 2; k++) {
        if (c[k] < '0' || c[k] > '9') continue;
        int suffix_len = len - k - 1;
        if (suffix_len > 3) continue;
        int all_letters = 1;
        for (int m = k + 1; m < len; m++) {
            if (letter_value(c[m]) < 0) { all_letters = 0; break; }
        }
        if (all_letters) return k;
    }
    return -1;
}

static int pack_callsign(const char *callsign, uint32_t *out_n1)
{
    int len = (int)strlen(callsign);
    if (len < 4 || len > 6) return 0;
    char c[6];
    for (int k = 0; k < len; k++) {
        char ch = callsign[k];
        c[k] = (char)((ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch);
    }

    int digit_at = find_anchor_digit(c, len);
    if (digit_at < 0) return 0;
    int suffix_len = len - digit_at - 1;

    int p1 = (digit_at < 2) ? CALL_ABSENT_ALNUM : alnum_value(c[digit_at - 2]);
    int p2 = (digit_at < 1) ? CALL_ABSENT_ALNUM : alnum_value(c[digit_at - 1]);
    int s1 = (suffix_len < 1) ? CALL_ABSENT_LETTER : letter_value(c[digit_at + 1]);
    int s2 = (suffix_len < 2) ? CALL_ABSENT_LETTER : letter_value(c[digit_at + 2]);
    int s3 = (suffix_len < 3) ? CALL_ABSENT_LETTER : letter_value(c[digit_at + 3]);
    if (p1 < 0 || p2 < 0 || s1 < 0 || s2 < 0 || s3 < 0) return 0;

    uint32_t n1 = (uint32_t)p1;
    n1 = n1 * CALL_ALPHA_ALNUM + (uint32_t)p2;
    n1 = n1 * CALL_ALPHA_DIGIT + (uint32_t)(c[digit_at] - '0');
    n1 = n1 * CALL_ALPHA_LETTER + (uint32_t)s1;
    n1 = n1 * CALL_ALPHA_LETTER + (uint32_t)s2;
    n1 = n1 * CALL_ALPHA_LETTER + (uint32_t)s3;
    *out_n1 = n1;
    return 1;
}

static int unpack_callsign(uint32_t n1, char callsign_out[7])
{
    int s3 = (int)(n1 % CALL_ALPHA_LETTER); n1 /= CALL_ALPHA_LETTER;
    int s2 = (int)(n1 % CALL_ALPHA_LETTER); n1 /= CALL_ALPHA_LETTER;
    int s1 = (int)(n1 % CALL_ALPHA_LETTER); n1 /= CALL_ALPHA_LETTER;
    int d  = (int)(n1 % CALL_ALPHA_DIGIT);  n1 /= CALL_ALPHA_DIGIT;
    int p2 = (int)(n1 % CALL_ALPHA_ALNUM);  n1 /= CALL_ALPHA_ALNUM;
    int p1 = (int)n1;
    /* p1 is whatever's left after dividing out every other field, so
     * unlike the others its range isn't enforced by a modulus - it must
     * be checked explicitly. Its valid range is 0..36 (37 values: '0'-'9'/
     * 'A'-'Z' packed by alnum_value, plus 36 for "absent"); anything past
     * that means n1 didn't come from a valid pack (e.g. a wrong Fano
     * decode). */
    if (p1 < 0 || p1 > CALL_ABSENT_ALNUM) return 0;

    char tmp[7];
    tmp[0] = alnum_char(p1);
    tmp[1] = alnum_char(p2);
    tmp[2] = (char)('0' + d);
    tmp[3] = letter_char(s1);
    tmp[4] = letter_char(s2);
    tmp[5] = letter_char(s3);
    tmp[6] = '\0';

    int start = 0;
    while (start < 5 && tmp[start] == ' ') start++;
    snprintf(callsign_out, 7, "%-6s", &tmp[start]);
    for (int k = 0; k < 6; k++) {
        if (callsign_out[k] == ' ') { callsign_out[k] = '\0'; break; }
    }
    return 1;
}

/* ---- Maidenhead grid locator + power packing (22-bit combined field) ----
 *
 * A 4-character Maidenhead locator is [lon field][lat field][lon square]
 * [lat square]: fields are 18 zones of 20 degrees longitude / 10 degrees
 * latitude each ('A'-'R'), squares subdivide a field into 10 x 10 degrees
 * of 2 / 1 ('0'-'9') - a public ham-radio convention from 1980, unrelated
 * to WSPR specifically. Linearized as lon_idx = lon_field*10+lon_square
 * (0..179) and lat_idx = lat_field*10+lat_square (0..179), WSPR packs
 * these two 180-valued indices into one 15-bit number 0..32399 alongside a
 * 7-bit power field, for 22 bits total.
 *
 * WSPR's own convention runs the longitude index in REVERSE (index 0 is
 * the highest longitude cell, not the lowest) - confirmed empirically
 * rather than assumed: this is the specific relationship that decodes
 * WSJT's real reference recording to legitimate, standard-format grid
 * squares (FN20, EL89, DM04, EL09, DM09), which only happens if the
 * linearization direction matches what real WSPR transmitters actually
 * use. */
static int pack_grid_and_power(const char *grid, int power_dbm, uint32_t *out_n2)
{
    if (strlen(grid) != 4) return 0;
    char g[4];
    for (int k = 0; k < 4; k++) {
        char ch = grid[k];
        g[k] = (char)((ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch);
    }
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') return 0;
    if (g[2] < '0' || g[2] > '9' || g[3] < '0' || g[3] > '9') return 0;

    int lon_idx = (g[0] - 'A') * 10 + (g[2] - '0');
    int lat_idx = (g[1] - 'A') * 10 + (g[3] - '0');
    uint32_t locator = (uint32_t)(179 - lon_idx) * 180 + (uint32_t)lat_idx;

    /* On an exact tie (e.g. 35 is equidistant from legal values 33 and 37)
     * this rounds toward the LOWER legal value - an arbitrary but
     * deliberate choice (strict `<` below keeps the first/smaller value
     * found rather than switching to a later/larger equally-close one),
     * documented so it isn't mistaken for a bug if a future change alters
     * it. */
    static const int legal_power[] = { 0, 3, 7, 10, 13, 17, 20, 23, 27, 30,
                                        33, 37, 40, 43, 47, 50, 53, 57, 60 };
    int nearest = legal_power[0], best_diff = power_dbm > legal_power[0]
                      ? power_dbm - legal_power[0] : legal_power[0] - power_dbm;
    for (size_t i = 1; i < sizeof(legal_power) / sizeof(legal_power[0]); i++) {
        int diff = power_dbm > legal_power[i] ? power_dbm - legal_power[i]
                                               : legal_power[i] - power_dbm;
        if (diff < best_diff) { best_diff = diff; nearest = legal_power[i]; }
    }

    *out_n2 = (locator << 7) | (uint32_t)(nearest + 64);
    return 1;
}

static int unpack_grid_and_power(uint32_t n2, char grid_out[5], int *power_dbm_out)
{
    *power_dbm_out = (int)(n2 & 0x7F) - 64;
    uint32_t locator = n2 >> 7;
    if (locator >= 32400) return 0;

    int lon_idx = 179 - (int)(locator / 180);
    int lat_idx = (int)(locator % 180);

    grid_out[0] = (char)('A' + lon_idx / 10);
    grid_out[2] = (char)('0' + lon_idx % 10);
    grid_out[1] = (char)('A' + lat_idx / 10);
    grid_out[3] = (char)('0' + lat_idx % 10);
    return 1;
}

/* ---- public entry points ---- */

int wspr_pack_message(const char *callsign, const char *grid, int power_dbm,
                       wspr_msg_bytes_t *out)
{
    if (!callsign || !grid || !out) return 0;
    uint32_t n1, n2;
    if (!pack_callsign(callsign, &n1)) return 0;
    if (!pack_grid_and_power(grid, power_dbm, &n2)) return 0;

    memset(out->dat, 0, sizeof(out->dat));
    int pos = 0;
    put_bits(out->dat, &pos, n1, 28);
    put_bits(out->dat, &pos, n2, 22);
    return 1;
}

int wspr_unpack_message(const wspr_msg_bytes_t *in, char callsign_out[7],
                         char grid_out[5], int *power_dbm_out)
{
    if (!in || !callsign_out || !grid_out || !power_dbm_out) return 0;
    int pos = 0;
    uint32_t n1 = get_bits(in->dat, &pos, 28);
    uint32_t n2 = get_bits(in->dat, &pos, 22);

    if (!unpack_callsign(n1, callsign_out)) return 0;
    if (!unpack_grid_and_power(n2, grid_out, power_dbm_out)) return 0;
    grid_out[4] = '\0';
    return 1;
}
