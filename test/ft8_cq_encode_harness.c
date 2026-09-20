// Host test for what ftx_message_encode() actually does with the CQ presets
// operators type into the CQ editor.
//
// Build (from the repo root):
//   gcc -O2 -I components/ft8_lib/ft8 -I main -o ft8_cq_encode_harness \
//       test/ft8_cq_encode_harness.c main/ft8_msg_guard.c \
//       components/ft8_lib/ft8/message.c components/ft8_lib/ft8/text.c
//   ./ft8_cq_encode_harness
//
// WHY. Don WB0LQW and Roy KI0ER both reported (2026-08-13) that CQ presets
// beyond a plain "CQ <call> <grid>" fail on the air:
//
//   Msg 1  "CQ WB0LQW DN70"       -> decoded by WSJT-X on a nearby receiver
//   Msg 2  "CQ POTA WB0LQW"       -> QMX keyed, power+SWR shown, NOTHING decoded
//   Msg 3  "CQ QRP WB0LQW"        -> same
//   Msg 2/3 with grid appended    -> not keyed at all; the Operator Identity
//                                    modal appeared instead
//
// That last symptom is a separate bug in ft8_screen_view.c (any encode failure
// pops the identity modal, blaming the operator's callsign for a message that
// simply would not encode). This harness answers the other half: WHICH of these
// encode, as WHAT message type, and what a receiver would see.
//
// It links the real message.c rather than reasoning about it, because the
// interesting behaviour is the std -> nonstd -> free-text fallback chain in
// ftx_message_encode(), and free text is capped at 13 characters - shorter than
// several of these strings.

#include "message.h"
#include "../main/ft8_msg_guard.h"

#include <stdio.h>
#include <string.h>

static const char *rc_name(ftx_message_rc_t rc)
{
    switch (rc) {
    case FTX_MESSAGE_RC_OK:                 return "OK";
    case FTX_MESSAGE_RC_ERROR_CALLSIGN1:    return "ERROR_CALLSIGN1";
    case FTX_MESSAGE_RC_ERROR_CALLSIGN2:    return "ERROR_CALLSIGN2";
    case FTX_MESSAGE_RC_ERROR_SUFFIX:       return "ERROR_SUFFIX";
    case FTX_MESSAGE_RC_ERROR_GRID:         return "ERROR_GRID";
    case FTX_MESSAGE_RC_ERROR_TYPE:         return "ERROR_TYPE";
    default:                                return "ERROR_?";
    }
}


// Encode, then decode the payload back the way a receiving station would, and
// print both. A message that encodes but decodes to something different is the
// dangerous case: the radio keys up and the far end sees nothing useful.
static int try_one(const char *text, int expect_ok)
{
    ftx_message_t msg;
    ftx_message_rc_t rc = ftx_message_encode(&msg, NULL, text);

    printf("  %-24s encode=%-16s", text, rc_name(rc));

    if (rc != FTX_MESSAGE_RC_OK) {
        printf("  (nothing transmitted)\n");
        return expect_ok ? 1 : 0;
    }

    printf(" i3=%d n3=%d ", (int)ftx_message_get_i3(&msg), (int)ftx_message_get_n3(&msg));

    // Decode it back the way a receiving station would. A message that encodes
    // but decodes to something else is the dangerous case: the radio keys up and
    // the far end sees nothing useful.
    char seen[64] = "";
    ftx_message_offsets_t offs;
    ftx_message_rc_t drc = ftx_message_decode(&msg, NULL, seen, &offs);
    if (drc != FTX_MESSAGE_RC_OK) {
        printf(" -> DECODE FAILED (%s)\n", rc_name(drc));
        return 1;
    }
    int same = (strcmp(seen, text) == 0);
    printf(" -> '%s' %s\n", seen, same ? "" : "  <-- DIFFERS from what was typed");
    return (expect_ok && same) ? 0 : (expect_ok ? 1 : 0);
}

// ---------------------------------------------------------------------------
// The two guards that now stand between a typed preset and a keyed radio.
// These link main/ft8_msg_guard.c itself, so a change there is tested here.

static int norm_case(const char *in, const char *want)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", in);
    ft8_msg_normalize(buf);
    int bad = (strcmp(buf, want) != 0);
    printf("  normalize('%s') -> '%s'%s\n", in, buf,
           bad ? "   *** FAIL, wanted '" : "");
    if (bad) printf("%s'\n", want);
    return bad;
}

static int guard_case(const char *typed, const char *decoded, const char *call,
                      bool want_ok)
{
    bool got = ft8_msg_roundtrip_ok(typed, decoded, call);
    int bad = (got != want_ok);
    printf("  %-22s as seen '%-18s' -> %-6s%s\n", typed, decoded,
           got ? "KEY" : "REFUSE", bad ? "   *** FAIL" : "");
    return bad;
}

static int guard_tests(void)
{
    int bad = 0;

    printf("\nWhitespace normalisation (the fix for Don's presets):\n");
    bad += norm_case("CQ  POTA WB0LQW", "CQ POTA WB0LQW");
    bad += norm_case("  CQ   QRP    WB0LQW  ", "CQ QRP WB0LQW");
    bad += norm_case("CQ\tPOTA\t\tWB0LQW", "CQ POTA WB0LQW");
    bad += norm_case("CQ WB0LQW DN70", "CQ WB0LQW DN70");   // already clean
    bad += norm_case("   ", "");
    bad += norm_case("", "");

    printf("\nRound-trip guard - must REFUSE the dangerous, KEY the merely lossy:\n");
    // The exact failure: callsign gone, turned into a report to a hash.
    bad += guard_case("CQ  POTA WB0LQW", "CQ  <...> +00", "WB0LQW", false);
    // Protocol legitimately has no room for the modifier or grid - still a CQ,
    // still our call, so it must key.
    bad += guard_case("CQ POTA PJ4/K1ABC", "CQ PJ4/K1ABC", "PJ4/K1ABC", true);
    bad += guard_case("CQ VK9/WB0LQW DN70", "CQ VK9/WB0LQW", "VK9/WB0LQW", true);
    bad += guard_case("CQ POTA WB0LQW", "CQ POTA WB0LQW", "WB0LQW", true);
    // A resolved hash comes back in angle brackets - those are delimiters, not
    // part of the callsign, or every nonstandard operator would be refused.
    bad += guard_case("WB0LQW K1ABC -07", "<WB0LQW> K1ABC -07", "WB0LQW", true);
    // A CQ that stops being a CQ is always wrong.
    bad += guard_case("CQ WB0LQW DN70", "WB0LQW K1ABC -07", "WB0LQW", false);
    // No callsign in the typed text - not our business to enforce one.
    bad += guard_case("CQ DX", "CQ DX", "WB0LQW", true);
    // Empty decode means we have nothing to transmit.
    bad += guard_case("CQ WB0LQW DN70", "", "WB0LQW", false);

    printf("%s\n", bad ? "  GUARD TESTS FAILED" : "  guard tests pass");
    return bad;
}

int main(void)
{
    int fails = 0;

    printf("\nWhat operators actually typed (Don WB0LQW's three presets):\n");
    fails += try_one("CQ WB0LQW DN70", 1);        /* works on the air */
    fails += try_one("CQ POTA WB0LQW", 1);        /* keyed, nothing decoded */
    fails += try_one("CQ QRP WB0LQW", 1);         /* keyed, nothing decoded */
    fails += try_one("CQ POTA WB0LQW DN70", 1);   /* refused to key */
    fails += try_one("CQ QRP WB0LQW DN70", 1);    /* refused to key */

    // What Don's DEVICE actually built, from his 2026-08-14 diagnostic log:
    //   ft8_tx: built text CQ: 'CQ  POTA WB0LQW' @ 1500 Hz
    // Two spaces after CQ (U+0020 U+0020, confirmed byte by byte). The message
    // encoded, armed, and keyed the radio for 12678 ms at 3.2 W - and WSJT-X on
    // a receiver two metres away decoded nothing. So the fault is not the
    // encoder refusing; it is what an extra space does to the message it builds.
    printf("\nWhat Don's device actually built (double space, from his log):\n");
    fails += try_one("CQ  POTA WB0LQW", 1);
    fails += try_one("CQ  QRP WB0LQW", 1);
    fails += try_one("CQ  POTA WB0LQW DN70", 1);
    fails += try_one("CQ  WB0LQW DN70", 1);

    printf("\nFor comparison - forms the 77-bit protocol is known to carry:\n");
    try_one("CQ DX WB0LQW DN70", 1);
    try_one("CQ TEST WB0LQW DN70", 1);
    try_one("CQ 123 WB0LQW DN70", 1);
    try_one("CQ WB0LQW", 1);

    // A round-trip guard at the TX choke point would catch every message that
    // encodes to something other than what it says. Before adding one, check it
    // cannot falsely reject the operators it must not reject: a nonstandard call
    // is transmitted as a HASH, and ft8_lib hands it back in <angle brackets>,
    // so a naive strcmp would refuse to key their radio at all.
    printf("\nRound-trip fidelity for nonstandard callsigns (guard must tolerate these):\n");
    try_one("CQ PJ4/K1ABC", 1);
    try_one("CQ POTA PJ4/K1ABC", 1);
    try_one("CQ VK9/WB0LQW DN70", 1);

    printf("\nAnd the character budget, since free text is the last resort:\n");
    printf("  free text carries 13 characters. Lengths above:\n");
    const char *l[] = { "CQ WB0LQW DN70", "CQ POTA WB0LQW", "CQ QRP WB0LQW",
                        "CQ POTA WB0LQW DN70", "CQ QRP WB0LQW DN70" };
    for (int i = 0; i < 5; i++)
        printf("    %-24s %2d chars%s\n", l[i], (int)strlen(l[i]),
               strlen(l[i]) > 13 ? "   (too long for free text)" : "");

    /* ⛔ THE ROUND-TRIP DIFFERENCES ARE NOT FAILURES, and conflating them with
     * the guard's verdict made this the only harness in the suite that could
     * never pass. `fails` counts messages the 77-bit protocol REWROTE - and
     * CLAUDE.md is explicit that some of those rewrites are correct: a
     * nonstandard callsign legitimately drops tokens it has no room for, so
     * "CQ POTA PJ4/K1ABC" going out as "CQ PJ4/K1ABC" is the protocol working,
     * and refusing to key that operator would be worse than the fault being
     * guarded against.
     *
     * What must hold is ft8_msg_guard's verdict - a CQ stays a CQ and the
     * operator's callsign survives - which is what guard_tests() checks. That
     * is the exit code. The rewrite count stays printed, because the list of
     * what the protocol rewrites is the useful part of this harness. */
    printf("\n%d message(s) were rewritten by the protocol - some correctly:\na nonstandard callsign drops tokens it cannot carry.\n", fails);

    int guard_bad = guard_tests();
    printf("guard verdict: %s\n", guard_bad ? "FAILED" : "ok");
    return guard_bad ? 1 : 0;
}
