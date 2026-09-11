#include "cw_callsign.h"
#include "util/dxcc.h"

#include <string.h>
#include <ctype.h>

#define BASE_MIN   3
#define BASE_MAX   8     /* 3DA0ABC is 7; leave one for the odd long allocation */
#define AFFIX_MAX  4     /* /P /QRP /MM /9 /PJ4 */
#define MAX_PARTS  3

static bool all_alnum(const char *s, size_t n)
{
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i])) return false;
    return true;
}

/* The ITU prefix shapes that sit in front of the area digit. This is the test
 * that kills most decoder noise: "TEE5A" has a perfectly good area digit and a
 * perfectly good suffix, and is rejected here because THREE LETTERS is not a
 * prefix shape. Two letters is (SM, DL, OT); digit-letter is (2E, 9A, 4X);
 * digit-letter-letter is (3DA). Letter-letter-letter is not. */
static bool prefix_shape_ok(const char *p, size_t n)
{
    switch (n) {
    case 1: return isalpha((unsigned char)p[0]);
    case 2: return (isalpha((unsigned char)p[0]) || isdigit((unsigned char)p[0]))
                   && isalpha((unsigned char)p[1]);
    case 3: return isdigit((unsigned char)p[0]) && isalpha((unsigned char)p[1])
                   && isalpha((unsigned char)p[2]);
    default: return false;
    }
}

static bool base_ok(const char *s, size_t n)
{
    if (n < BASE_MIN || n > BASE_MAX) return false;
    if (!all_alnum(s, n)) return false;

    /* Split on the LAST digit: that is the area digit. Using the last one
     * rather than the first is what makes 3DA0ABC and 2E0ABC work - their
     * prefixes contain digits of their own. */
    size_t d = n;
    for (size_t i = n; i-- > 0; ) {
        if (isdigit((unsigned char)s[i])) { d = i; break; }
    }
    if (d == n) return false;              /* no digit at all: not a callsign */

    if (!prefix_shape_ok(s, d)) return false;

    size_t suf = n - d - 1;
    if (suf < 1 || suf > 4) return false;  /* a call cannot end on its area digit */
    for (size_t i = d + 1; i < n; i++)
        if (!isalpha((unsigned char)s[i])) return false;

    return true;
}

bool cw_call_structure_ok(const char *t)
{
    if (!t) return false;
    size_t n = strlen(t);
    /* ⛔ 12 IS NOT ARBITRARY - it is cw_qso.c's CALL_MAX, i.e. the width of
     * every buffer that stores a captured call. Accepting a longer token would
     * hand the capture something it must truncate, and a truncated callsign is
     * a DIFFERENT callsign. Rejecting is the honest failure: the operator sees
     * no card rather than a wrong one. The cost is the vanishingly rare triple
     * form (PJ4/K1ABC/QRP, 13); widen CALL_MAX first if that ever matters. */
    if (n < BASE_MIN || n > 12) return false;

    /* Walk the '/'-separated parts. Exactly one must be a base call; every
     * other part must be a short alphanumeric affix. */
    const char *part[MAX_PARTS];
    size_t      plen[MAX_PARTS];
    int         np = 0;

    const char *start = t;
    for (const char *p = t; ; p++) {
        if (*p == '/' || *p == '\0') {
            if (np >= MAX_PARTS) return false;
            part[np] = start;
            plen[np] = (size_t)(p - start);
            if (plen[np] == 0) return false;      /* "//", leading or trailing '/' */
            np++;
            if (*p == '\0') break;
            start = p + 1;
        }
    }

    int bases = 0;
    for (int i = 0; i < np; i++) {
        if (base_ok(part[i], plen[i])) { bases++; continue; }
        if (plen[i] > AFFIX_MAX || !all_alnum(part[i], plen[i])) return false;
    }
    return bases == 1;
}

bool cw_call_is_plausible(const char *t)
{
    if (!cw_call_structure_ok(t)) return false;
    /* dxcc_lookup() does a longest-prefix match over the allocation table and
     * returns NULL when nothing matches, which is exactly the question here:
     * is this prefix allocated to anybody at all? It normalises the '/' affixes
     * itself, so the whole token goes in. */
    return dxcc_lookup(t) != NULL;
}
