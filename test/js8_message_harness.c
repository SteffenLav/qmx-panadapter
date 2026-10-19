/* Host test for the JS8 message layer (J2 of docs/js8-feasibility.md):
 * FrameDirected and FrameHeartbeat pack/unpack.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -o js8_message_harness test/js8_message_harness.c \
 *       components/ft8_lib/ft8/js8_message.c components/ft8_lib/ft8/js8_codec.c \
 *       components/ft8_lib/ft8/js8_tables.c components/ft8_lib/ft8/ldpc.c \
 *       components/ft8_lib/ft8/constants.c -lm && ./js8_message_harness
 *
 * ⛔ WHAT THIS CANNOT PROVE, SAID FIRST.
 *
 * J1 had a real arbiter: JS8Call's own encode174.f90 compiled with gfortran.
 * This layer has none. varicode.cpp is Qt C++ - QString, QMap,
 * QRegularExpression - and the repository ships no test vectors. So the
 * reference values below were computed in Python FROM THE PUBLISHED FORMULAS,
 * not from a running copy of JS8Call. That catches a transcription slip in the
 * C; it cannot catch a misreading of the source, because both readings would
 * be mine.
 *
 * Only a real JS8Call station decoding our frame settles it. NOT a second
 * Tab5: two copies of this code share their mistakes exactly and would work
 * each other perfectly while being unintelligible to everyone else.
 *
 * What it does prove, each one a trap that produces a plausible frame rather
 * than a visible failure:
 *
 *  1. The mixed-radix callsign codec, against hand-computed values - including
 *     the permutation rule, where " K1ABC" packs and "K1ABC " does not, and
 *     the reference keeps the LAST permutation that matches.
 *  2. packGrid against hand-computed values, in the reference's own convention
 *     where longitude is POSITIVE WEST. A sign error there still round-trips
 *     through our own unpack and only shows on the air.
 *  3. The frame bit layout: 3-bit type first, 72 bits total, and the last byte
 *     carrying portable_from, portable_to and the number together - which the
 *     feasibility note had as three separate fields.
 *  4. A whole QSO ladder through pack -> unpack.
 *  5. End to end through J1: message -> frame -> payload+CRC -> LDPC encode ->
 *     belief-propagation decode -> payload -> message, with bit errors
 *     injected. That exercises the two layers together, which is the only
 *     place a bit-order disagreement between them would show.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "js8_message.h"
#include "ldpc.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

/* ---- 1. callsign --------------------------------------------------------- */

static void test_callsign(void)
{
    struct { const char* call; uint32_t packed; } v[] = {
        { "OZ1LAV", 176977893u },   /* six characters, no padding needed */
        { "K1ABC",  259047992u },   /* five - only " K1ABC" fits the grammar */
        { "W7STF",  261541661u },
        { "N0LUF",  259627334u },
        { "G4ABC",  258319721u },
        { "VK3X",   223675424u },   /* four - here it is the TRAILING pad */
        { "N0A",    259619498u },   /* three */
    };
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++)
    {
        bool p = false;
        uint32_t got = js8_pack_callsign(v[i].call, &p);
        CHECK(got == v[i].packed, "pack %s = %u, expected %u\n",
              v[i].call, got, v[i].packed);
        CHECK(!p, "%s is not portable\n", v[i].call);

        char back[16];
        js8_unpack_callsign(got, false, back);
        CHECK(strcmp(back, v[i].call) == 0,
              "unpack %u = \"%s\", expected \"%s\"\n", got, back, v[i].call);
    }
}

/* "/P" is not part of the callsign: it is stripped and carried as one bit, so
 * OZ1LAV and OZ1LAV/P pack to the SAME 28 bits. */
static void test_callsign_portable(void)
{
    bool p = false;
    uint32_t a = js8_pack_callsign("OZ1LAV", &p);
    CHECK(!p, "plain call should not set portable\n");
    uint32_t b = js8_pack_callsign("OZ1LAV/P", &p);
    CHECK(p, "/P should set portable\n");
    CHECK(a == b, "the 28 bits should be identical: %u vs %u\n", a, b);

    char back[16];
    js8_unpack_callsign(b, true, back);
    CHECK(strcmp(back, "OZ1LAV/P") == 0, "unpack portable = \"%s\"\n", back);
}

static void test_callsign_rejects(void)
{
    bool p;
    /* Nothing that cannot meet the six-character grammar may pack. A silent 0
     * here is the reference's own signal, and js8_pack_directed() refuses the
     * whole frame on it rather than sending a frame addressed to nobody. */
    CHECK(js8_pack_callsign("", &p) == 0, "empty must not pack\n");
    CHECK(js8_pack_callsign("A", &p) == 0, "one character must not pack\n");
    CHECK(js8_pack_callsign("TOOLONGCALL", &p) == 0, "11 characters must not pack\n");
    CHECK(js8_pack_callsign("ABCDEF", &p) == 0,
          "no digit in the third position must not pack\n");
}

/* The group tokens are not callsigns and must survive the trip intact. */
static void test_basecalls(void)
{
    bool p;
    uint32_t v = js8_pack_callsign("@ALLCALL", &p);
    CHECK(v != 0, "@ALLCALL should pack\n");
    char back[16];
    js8_unpack_callsign(v, false, back);
    CHECK(strcmp(back, "@ALLCALL") == 0, "@ALLCALL round trip = \"%s\"\n", back);
}

/* ---- 2. grid ------------------------------------------------------------- */

static void test_grid(void)
{
    /* Hand-computed: JO65 gives dlong -13.04 and dlat 55.52, so
     * ((-13 + 180) / 2) * 180 + (55 + 90) = 83 * 180 + 145 = 15085.
     * The truncation is toward zero, not floor - that matters because dlong is
     * negative across most of the eastern hemisphere. */
    CHECK(js8_pack_grid("JO65") == 15085, "JO65 = %u, expected 15085\n",
          js8_pack_grid("JO65"));
    CHECK(js8_pack_grid("FN42") == 22632, "FN42 = %u, expected 22632\n",
          js8_pack_grid("FN42"));

    const char* grids[] = { "JO65", "FN42", "IO91", "RR99" };
    for (unsigned i = 0; i < sizeof(grids) / sizeof(grids[0]); i++)
    {
        char back[5];
        js8_unpack_grid(js8_pack_grid(grids[i]), back);
        CHECK(strcmp(back, grids[i]) == 0, "grid %s round trips to \"%s\"\n",
              grids[i], back);
    }

    /* ⛔ SOUTHERN GRIDS LOSE A SQUARE, AND THAT IS JS8CALL'S BEHAVIOUR, NOT A
     * DEFECT HERE.
     *
     * packGrid assigns a float latitude to an int, which truncates TOWARD ZERO
     * rather than flooring. North of the equator that is harmless - JO65's
     * 55.52 becomes 55 - but QF56's -33.48 becomes -33, one square out.
     *
     * Checked against the published formulas in Python BEFORE this expectation
     * was written, because the first instinct was that the C was wrong:
     * QF56 -> 2577 -> QF57 and AA00 -> 32221 -> AA01 in the reference too.
     *
     * Asserted rather than corrected. Being "right" here would put every
     * southern station's grid one square away from what every JS8Call on the
     * air shows, which is a worse answer than agreeing with the network. */
    char sh[5];
    js8_unpack_grid(js8_pack_grid("QF56"), sh);
    CHECK(strcmp(sh, "QF57") == 0,
          "QF56 should come back QF57, bug-compatible with JS8Call; got \"%s\"\n", sh);
    js8_unpack_grid(js8_pack_grid("AA00"), sh);
    CHECK(strcmp(sh, "AA01") == 0,
          "AA00 should come back AA01, bug-compatible with JS8Call; got \"%s\"\n", sh);

    /* A missing or short locator is a real state - a station that has not been
     * given one - and must come back EMPTY rather than as a square. */
    char back[5];
    CHECK(js8_pack_grid("") == 0x7FFF, "empty grid should be 0x7FFF\n");
    CHECK(js8_pack_grid("JO") == 0x7FFF, "short grid should be 0x7FFF\n");
    js8_unpack_grid(0x7FFF, back);
    CHECK(back[0] == '\0', "0x7FFF should unpack to nothing, got \"%s\"\n", back);
}

/* ---- 3. number ----------------------------------------------------------- */

static void test_num(void)
{
    CHECK(js8_pack_num(-10) == 21, "-10 -> %u, expected 21\n", js8_pack_num(-10));
    CHECK(js8_pack_num(31) == 62, "+31 -> %u, expected 62\n", js8_pack_num(31));
    CHECK(js8_pack_num(-30) == 1, "-30 -> %u, expected 1\n", js8_pack_num(-30));
    /* Out of range CLAMPS, it does not wrap - a report of -40 must not arrive
     * as +22. */
    CHECK(js8_pack_num(-40) == 1, "-40 should clamp to -30\n");
    CHECK(js8_pack_num(99) == 62, "+99 should clamp to +31\n");
    /* 0 is reserved for "no number", so it is distinguishable from a report of
     * 0 dB, which packs to 31. */
    CHECK(js8_pack_num(JS8_NUM_NONE) == 0, "NONE -> 0\n");
    CHECK(js8_pack_num(0) == 31, "0 dB -> 31\n");
    CHECK(js8_unpack_num(0) == JS8_NUM_NONE, "0 -> NONE\n");
    CHECK(js8_unpack_num(31) == 0, "31 -> 0 dB\n");
}

/* ---- 4. frame layout ----------------------------------------------------- */

static void test_directed_layout(void)
{
    js8_directed_t m = { 0 };
    snprintf(m.from, sizeof(m.from), "OZ1LAV");
    snprintf(m.to, sizeof(m.to), "K1ABC");
    m.cmd = JS8_CMD_SNR;
    m.num = -12;

    uint8_t f[JS8_FRAME_BYTES];
    CHECK(js8_pack_directed(&m, f), "directed should pack\n");

    /* Frame type is the FIRST three bits, which is what the feasibility note
     * placed outside the frame entirely. */
    CHECK(js8_frame_type(f) == JS8_FRAME_DIRECTED,
          "type = %d, expected 3\n", (int)js8_frame_type(f));

    /* The last byte is portable_from | portable_to | packNum, as one value -
     * the note had them as three fields in a different order. */
    CHECK(f[8] == js8_pack_num(-12),
          "last byte = 0x%02X, expected 0x%02X\n", f[8], js8_pack_num(-12));

    js8_directed_t b;
    CHECK(js8_unpack_directed(f, &b), "directed should unpack\n");
    CHECK(strcmp(b.from, "OZ1LAV") == 0, "from = \"%s\"\n", b.from);
    CHECK(strcmp(b.to, "K1ABC") == 0, "to = \"%s\"\n", b.to);
    CHECK(b.cmd == JS8_CMD_SNR, "cmd = %u\n", b.cmd);
    CHECK(b.num == -12, "num = %d\n", b.num);
    CHECK(strcmp(js8_cmd_text(b.cmd), "SNR") == 0,
          "cmd text = \"%s\"\n", js8_cmd_text(b.cmd));
}

static void test_heartbeat_layout(void)
{
    js8_heartbeat_t m = { 0 };
    snprintf(m.call, sizeof(m.call), "OZ1LAV");
    snprintf(m.grid, sizeof(m.grid), "JO65");
    m.is_cq = true;
    m.bits3 = 0;

    uint8_t f[JS8_FRAME_BYTES];
    CHECK(js8_pack_heartbeat(&m, f), "heartbeat should pack\n");
    CHECK(js8_frame_type(f) == JS8_FRAME_HEARTBEAT,
          "type = %d, expected 0\n", (int)js8_frame_type(f));

    js8_heartbeat_t b;
    CHECK(js8_unpack_heartbeat(f, &b), "heartbeat should unpack\n");
    CHECK(strcmp(b.call, "OZ1LAV") == 0, "call = \"%s\"\n", b.call);
    CHECK(strcmp(b.grid, "JO65") == 0, "grid = \"%s\"\n", b.grid);
    CHECK(b.is_cq, "is_cq lost\n");
    CHECK(strcmp(js8_cq_text(b.is_cq, b.bits3), "CQ CQ CQ") == 0,
          "cq text = \"%s\"\n", js8_cq_text(b.is_cq, b.bits3));

    /* HB and CQ differ only in one bit, and getting it backwards would answer
     * every CQ with a beacon. */
    m.is_cq = false;
    CHECK(js8_pack_heartbeat(&m, f), "HB should pack\n");
    CHECK(js8_unpack_heartbeat(f, &b), "HB should unpack\n");
    CHECK(!b.is_cq, "HB came back as CQ\n");
    CHECK(strcmp(js8_cq_text(b.is_cq, b.bits3), "HB") == 0,
          "hb text = \"%s\"\n", js8_cq_text(b.is_cq, b.bits3));
}

/* The Heartbeat frame uses a DIFFERENT callsign codec from the Directed one -
 * 50 bits, base 38, with '/' flags - so it has to be exercised separately or a
 * CQ would go out under the wrong call. */
static void test_alnum50(void)
{
    const char* calls[] = { "OZ1LAV", "K1ABC", "N0A", "VK3X", "W7STF" };
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++)
    {
        char back[16];
        uint64_t v = js8_pack_alnum50(calls[i]);
        CHECK(v != 0, "%s should pack to 50 bits\n", calls[i]);
        CHECK(v < (1ULL << 50), "%s overflowed 50 bits\n", calls[i]);
        js8_unpack_alnum50(v, back);
        CHECK(strcmp(back, calls[i]) == 0,
              "50-bit %s round trips to \"%s\"\n", calls[i], back);
    }
}

/* ---- 5. a whole QSO ------------------------------------------------------ */

static void test_qso_ladder(void)
{
    uint8_t f[JS8_FRAME_BYTES];

    /* CQ */
    js8_heartbeat_t cq = { 0 };
    snprintf(cq.call, sizeof(cq.call), "OZ1LAV");
    snprintf(cq.grid, sizeof(cq.grid), "JO65");
    cq.is_cq = true;
    CHECK(js8_pack_heartbeat(&cq, f), "CQ pack\n");
    js8_heartbeat_t cqb;
    CHECK(js8_unpack_heartbeat(f, &cqb) && strcmp(cqb.call, "OZ1LAV") == 0 &&
          strcmp(cqb.grid, "JO65") == 0 && cqb.is_cq, "CQ round trip\n");

    /* Their report to us, ours back, then the sign-off. */
    struct { const char* from; const char* to; uint8_t cmd; int num; } steps[] = {
        { "K1ABC",  "OZ1LAV", JS8_CMD_SNR, -12 },
        { "OZ1LAV", "K1ABC",  JS8_CMD_SNR, -8  },
        { "K1ABC",  "OZ1LAV", JS8_CMD_RR,  JS8_NUM_NONE },
        { "OZ1LAV", "K1ABC",  JS8_CMD_73,  JS8_NUM_NONE },
    };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
    {
        js8_directed_t m = { 0 };
        snprintf(m.from, sizeof(m.from), "%s", steps[i].from);
        snprintf(m.to, sizeof(m.to), "%s", steps[i].to);
        m.cmd = steps[i].cmd;
        m.num = steps[i].num;
        CHECK(js8_pack_directed(&m, f), "step %u pack\n", i);

        js8_directed_t b;
        CHECK(js8_unpack_directed(f, &b), "step %u unpack\n", i);
        CHECK(strcmp(b.from, steps[i].from) == 0 &&
              strcmp(b.to, steps[i].to) == 0 &&
              b.cmd == steps[i].cmd && b.num == steps[i].num,
              "step %u: %s -> %s %s %d came back %s -> %s %s %d\n", i,
              steps[i].from, steps[i].to, js8_cmd_text(steps[i].cmd), steps[i].num,
              b.from, b.to, js8_cmd_text(b.cmd), b.num);
    }
}

/* A frame addressed to a callsign that will not pack must be REFUSED, not sent
 * with a zero in it - a frame addressed to nobody is worse than no frame. */
static void test_refuses_bad_call(void)
{
    js8_directed_t m = { 0 };
    uint8_t f[JS8_FRAME_BYTES];
    snprintf(m.from, sizeof(m.from), "OZ1LAV");
    snprintf(m.to, sizeof(m.to), "NOPE!");
    CHECK(!js8_pack_directed(&m, f), "an unpackable 'to' must refuse the frame\n");
}

/* ---- 6. end to end through J1 -------------------------------------------- */

static void to_llr(const uint8_t cw[JS8_LDPC_N], float llr[JS8_LDPC_N], float amp)
{
    for (int i = 0; i < JS8_LDPC_N; i++) llr[i] = cw[i] ? amp : -amp;
}

/* message -> frame -> payload + CRC -> LDPC -> decode -> message.
 *
 * This is the only test that exercises the two layers TOGETHER, and a bit-order
 * disagreement between them shows nowhere else: each side round trips perfectly
 * against itself. */
static void test_end_to_end(void)
{
    js8_directed_t m = { 0 };
    snprintf(m.from, sizeof(m.from), "OZ1LAV");
    snprintf(m.to, sizeof(m.to), "W7STF");
    m.cmd = JS8_CMD_SNR;
    m.num = -15;

    uint8_t frame[JS8_FRAME_BYTES];
    CHECK(js8_pack_directed(&m, frame), "e2e pack\n");

    uint8_t payload[JS8_LDPC_K];
    js8_pack_payload(frame, 0, payload);
    CHECK(js8_check_payload_crc(payload), "e2e CRC\n");

    uint8_t cw[JS8_LDPC_N];
    js8_encode174(payload, cw);

    /* Eight bits flipped, the same load the J1 harness uses. */
    unsigned seed = 31;
    uint8_t noisy[JS8_LDPC_N];
    memcpy(noisy, cw, sizeof(noisy));
    for (int k = 0; k < 8; k++)
    {
        seed = seed * 1103515245u + 12345u;
        noisy[(seed >> 16) % JS8_LDPC_N] ^= 1u;
    }

    float   llr[JS8_LDPC_N];
    uint8_t plain[JS8_LDPC_N];
    int     errs = 0;
    to_llr(noisy, llr, 2.0f);
    bp_decode_code(&kJS8_LDPC_code_174_87, llr, 30, 0, plain, &errs);
    CHECK(errs == 0, "e2e decode left %d unsatisfied checks\n", errs);

    uint8_t itmp[JS8_LDPC_N], got_payload[JS8_LDPC_K];
    for (int i = 0; i < JS8_LDPC_N; i++) itmp[i] = plain[kJS8_LDPC_colorder[i]];
    memcpy(got_payload, itmp + JS8_LDPC_M, JS8_LDPC_K);
    CHECK(memcmp(got_payload, payload, JS8_LDPC_K) == 0, "e2e payload differs\n");
    CHECK(js8_check_payload_crc(got_payload), "e2e CRC after decode\n");

    /* Back to a frame: the payload's first 72 bits, one bit per byte. */
    uint8_t got_frame[JS8_FRAME_BYTES];
    memset(got_frame, 0, sizeof(got_frame));
    for (int i = 0; i < JS8_FRAME_BITS; i++)
        got_frame[i / 8] |= (uint8_t)((got_payload[i] & 1u) << (7 - (i % 8)));
    CHECK(memcmp(got_frame, frame, JS8_FRAME_BYTES) == 0, "e2e frame differs\n");

    js8_directed_t b;
    CHECK(js8_unpack_directed(got_frame, &b), "e2e unpack\n");
    CHECK(strcmp(b.from, "OZ1LAV") == 0 && strcmp(b.to, "W7STF") == 0 &&
          b.cmd == JS8_CMD_SNR && b.num == -15,
          "e2e message came back as %s -> %s %s %d\n",
          b.from, b.to, js8_cmd_text(b.cmd), b.num);
}

int main(void)
{
    printf("JS8 message layer\n");
    test_callsign();
    test_callsign_portable();
    test_callsign_rejects();
    test_basecalls();
    test_grid();
    test_num();
    test_directed_layout();
    test_heartbeat_layout();
    test_alnum50();
    test_qso_ladder();
    test_refuses_bad_call();
    test_end_to_end();

    if (g_fail) { printf("\nFAILED: %d check(s)\n", g_fail); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
