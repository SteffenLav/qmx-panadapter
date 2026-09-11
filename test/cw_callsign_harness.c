/* Host harness for cw_callsign.c - the callsign sanity check the CW page's
 * "new station" card depends on.
 *
 * Build (from the repo root):
 *   gcc -I main -I main/util -o cw_callsign_harness \
 *       test/cw_callsign_harness.c main/cw_callsign.c main/util/dxcc.c
 *   ./cw_callsign_harness
 *
 * ⭐ THE REJECT VECTORS ARE REAL BYTES OFF THE AIR, not invented ones. They
 * were pulled out of 744 TB; responses captured on 20 m on 2026-09-11 with
 * the QMX's own decoder running into noise - the same capture that produced
 * the genuine OT4A / JH6KZP / DL7MKI in the accept list. Inventing garbage
 * would have tested the rule against my idea of noise instead of the radio's.
 */
#include "cw_callsign.h"

#include <stdio.h>
#include <string.h>

static int pass, fail;

static void want(const char *t, bool expect, const char *why)
{
    bool got = cw_call_is_plausible(t);
    if (got == expect) { pass++; return; }
    fail++;
    printf("FAIL  %-16s -> %s, want %s   (%s)\n",
           t, got ? "accept" : "reject", expect ? "accept" : "reject", why);
}

static void want_struct(const char *t, bool expect, const char *why)
{
    bool got = cw_call_structure_ok(t);
    if (got == expect) { pass++; return; }
    fail++;
    printf("FAIL  %-16s -> structure %s, want %s   (%s)\n",
           t, got ? "ok" : "bad", expect ? "ok" : "bad", why);
}

int main(void)
{
    /* ---- real callsigns, heard on the air the day this was written ------- */
    want("OT4A",    true,  "Belgium, repeated many times in the capture");
    want("JH6KZP",  true,  "Japan");
    want("DL7MKI",  true,  "Germany");
    want("W1AW",    true,  "1-letter prefix");
    want("SM5ABC",  true,  "2-letter prefix, 3-letter suffix");
    want("9A1A",    true,  "digit-letter prefix, 1-letter suffix");
    want("2E0ABC",  true,  "digit-letter prefix with its own area digit");
    want("G0ABC",   true,  "1-letter prefix, area 0");
    want("VK9NS",   true,  "2-letter prefix, 2-letter suffix");
    want("EA2GM/P", true,  "portable affix");
    want("PJ4/K1ABC", true, "prefix affix in front");

    /* ---- real decoder noise from the same capture ------------------------ */
    want("SC3Z75HSC", false, "9 chars, prefix would be SC3Z7 - accepted by the OLD rule");
    want("TEE5A",   false, "THREE letters before the area digit is not a prefix shape");
    want("E9VA",    false, "structurally fine, but E9 is not an allocated prefix");
    want("WUAN",    false, "no digit at all");
    want("HAKI",    false, "no digit at all");
    want("QSB",     false, "a Q code, no digit");
    want("EH1PDAENJPDA", false, "12 chars - two calls run together by the decoder");
    want("TCE",     false, "no digit");
    want("GRSPORQSO", false, "words run together");
    want("MO*JIK",  false, "holds the decoder's unresolved-symbol marker");
    want("SEK+*5C>", false, "punctuation the decoder emits under noise");

    /* ---- structural edge cases, independent of the prefix table ---------- */
    want_struct("W1AW",   true,  "baseline");
    want_struct("3DA0ABC", true, "digit-letter-letter prefix");
    want_struct("K1A",    true,  "shortest real shape");
    want_struct("W1",     false, "ends on the area digit - no suffix");
    want_struct("ABC",    false, "no digit");
    want_struct("12345",  false, "all digits");
    want_struct("K1ABCDE", false, "5-letter suffix is too long");
    want_struct("TEE5A",  false, "3-letter prefix");
    want_struct("/P",     false, "leading slash, empty part");
    want_struct("K1ABC/", false, "trailing slash, empty part");
    want_struct("A/B/C/D", false, "four parts");
    want_struct("K1ABC/W1AW", false, "two bases is not one station");
    want_struct("DL/G0ABC/P", true,  "prefix + base + suffix, within CALL_MAX");
    want_struct("PJ4/K1ABC/QRP", false,
                "13 chars: longer than cw_qso.c's CALL_MAX, and a truncated "
                "call is a different call - see the note in cw_callsign.c");
    want_struct("",       false, "empty");

    /* A suffix must be letters: "K1A2" would otherwise split at the last digit
     * and leave an empty suffix, which the length test already catches - this
     * pins the case where a digit sits INSIDE what would be the suffix. */
    want_struct("K1A2B",  false, "digit inside the suffix");

    printf("%s: %d passed, %d failed\n", fail ? "FAIL" : "PASS", pass, fail);
    return fail ? 1 : 0;
}
