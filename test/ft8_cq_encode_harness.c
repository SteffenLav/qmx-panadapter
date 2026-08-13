// Host test for what ftx_message_encode() actually does with the CQ presets
// operators type into the CQ editor.
//
// Build (from the repo root):
//   gcc -O2 -I components/ft8_lib/ft8 -o ft8_cq_encode_harness \
//       test/ft8_cq_encode_harness.c \
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

int main(void)
{
    int fails = 0;

    printf("\nWhat operators actually typed (Don WB0LQW's three presets):\n");
    fails += try_one("CQ WB0LQW DN70", 1);        /* works on the air */
    fails += try_one("CQ POTA WB0LQW", 1);        /* keyed, nothing decoded */
    fails += try_one("CQ QRP WB0LQW", 1);         /* keyed, nothing decoded */
    fails += try_one("CQ POTA WB0LQW DN70", 1);   /* refused to key */
    fails += try_one("CQ QRP WB0LQW DN70", 1);    /* refused to key */

    printf("\nFor comparison - forms the 77-bit protocol is known to carry:\n");
    try_one("CQ DX WB0LQW DN70", 1);
    try_one("CQ TEST WB0LQW DN70", 1);
    try_one("CQ 123 WB0LQW DN70", 1);
    try_one("CQ WB0LQW", 1);

    printf("\nAnd the character budget, since free text is the last resort:\n");
    printf("  free text carries 13 characters. Lengths above:\n");
    const char *l[] = { "CQ WB0LQW DN70", "CQ POTA WB0LQW", "CQ QRP WB0LQW",
                        "CQ POTA WB0LQW DN70", "CQ QRP WB0LQW DN70" };
    for (int i = 0; i < 5; i++)
        printf("    %-24s %2d chars%s\n", l[i], (int)strlen(l[i]),
               strlen(l[i]) > 13 ? "   (too long for free text)" : "");

    printf("\n%d case(s) did not survive a round trip.\n", fails);
    return fails ? 1 : 0;
}
