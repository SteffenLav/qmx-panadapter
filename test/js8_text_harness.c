/* Host test for js8_text.c - the seam where a JS8 frame becomes the one line
 * of text the rest of the firmware speaks, and back again.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -o js8_text_harness test/js8_text_harness.c \
 *       components/ft8_lib/ft8/js8_text.c \
 *       components/ft8_lib/ft8/js8_message.c \
 *       components/ft8_lib/ft8/js8_codec.c \
 *       components/ft8_lib/ft8/js8_tables.c \
 *       && ./js8_text_harness
 *
 * WHAT IT IS FOR. ft8_qso.c decides the whole exchange from split_msg3()'s
 * third field: "RR73", "73", 'R' plus a signed number, or otherwise a report.
 * If this file renders a frame into anything else, the ladder does not fail
 * loudly - it waits, times out, and grey-lists a station that answered
 * correctly. So the checks below are mostly about the EXACT strings, and the
 * split rules are reimplemented here so a drift between the two is caught.
 *
 * ⛔ WHAT IT CANNOT PROVE. That the mapping is the one JS8Call uses. The
 * command vocabulary is read off varicode.cpp, but which command a station
 * actually sends at each step of a QSO is a behaviour of the application, and
 * no test here observes one. A real JS8Call station settles it. Never a second
 * Tab5.
 */
#include <stdio.h>
#include <string.h>

#include "js8_text.h"
#include "js8_message.h"   /* JS8_ITYPE_* - the transmission-type flags */

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

/* ft8_qso.c's split_msg3(), reimplemented. Not shared: that function lives in
 * main/ and pulls in the whole app. Any divergence between this copy and the
 * real one makes the test lie, so it is the one duplication here with a reason,
 * and the rules are three lines each. */
static int split3(const char* text, char* a, char* b, char* rest)
{
    const char* p = text;
    while (*p == ' ') p++;
    const char* s1 = p;
    while (*p && *p != ' ') p++;
    if (p == s1) return 0;
    memcpy(a, s1, (size_t)(p - s1));
    a[p - s1] = '\0';

    while (*p == ' ') p++;
    const char* s2 = p;
    while (*p && *p != ' ') p++;
    if (p == s2) return 0;
    memcpy(b, s2, (size_t)(p - s2));
    b[p - s2] = '\0';

    while (*p == ' ') p++;
    snprintf(rest, 32, "%s", p);
    return 1;
}

/* text -> frame -> text. The identity the ladder depends on: what we transmit
 * for a given line is what a receiver of our own frame would read back. */
static void expect_round_trip(const char* text)
{
    uint8_t frame[JS8_FRAME_BYTES], itype = 9;
    if (!js8_text_to_frame(text, frame, &itype))
    {
        g_fail++;
        printf("  FAIL: '%s' would not pack\n", text);
        return;
    }
    /* ⛔ FIRST|LAST, not 0. Every line js8_text_to_frame() builds is a
     * complete one-frame transmission, and JS8Call marks such a frame as both
     * the first and the last of its message. This asserted 0 until 2026-10-10,
     * which varicode.h documents as "any other frame of the message" - a
     * MIDDLE frame. The test was faithfully pinning a bug. */
    CHECK(itype == JS8_ITYPE_SINGLE, "'%s' gave itype %d, expected %d\n",
          text, (int)itype, JS8_ITYPE_SINGLE);

    char back[JS8_TEXT_MAX];
    if (!js8_frame_to_text(frame, back, sizeof(back)))
    {
        g_fail++;
        printf("  FAIL: '%s' packed but would not render\n", text);
        return;
    }
    CHECK(strcmp(text, back) == 0, "'%s' round-tripped to '%s'\n", text, back);
}

static void expect_refused(const char* text, const char* why)
{
    uint8_t frame[JS8_FRAME_BYTES];
    CHECK(!js8_text_to_frame(text, frame, NULL),
          "'%s' should be refused (%s)\n", text, why);
}

/* Every rung of the ladder, in the exact spelling ft8_qso.c compares against. */
static void test_ladder_round_trips(void)
{
    expect_round_trip("CQ OZ1LAV JO65");
    expect_round_trip("CQ OZ1LAV");
    expect_round_trip("HB OZ1LAV JO65");
    expect_round_trip("N2VGU OZ1LAV -07");
    expect_round_trip("N2VGU OZ1LAV +02");
    expect_round_trip("N2VGU OZ1LAV R-07");
    expect_round_trip("N2VGU OZ1LAV R+31");
    expect_round_trip("N2VGU OZ1LAV RR73");
    expect_round_trip("N2VGU OZ1LAV 73");
    /* packNum's full range, which is wider than FT8's -24..+15. */
    expect_round_trip("N2VGU OZ1LAV -30");
    expect_round_trip("N2VGU OZ1LAV +31");
}

/* ⛔ The ladder reads the THIRD FIELD. These checks are what stops a rendering
 * that is merely readable from being wrong: "R-07" and "-07" are different
 * rungs, and "RR73" and "73" end the QSO in different directions. */
static void test_rest_field_is_what_the_ladder_expects(void)
{
    struct { const char* text; const char* want_rest; } cases[] = {
        { "N2VGU OZ1LAV -07",  "-07"  },
        { "N2VGU OZ1LAV R-07", "R-07" },
        { "N2VGU OZ1LAV RR73", "RR73" },
        { "N2VGU OZ1LAV 73",   "73"   },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint8_t frame[JS8_FRAME_BYTES];
        char out[JS8_TEXT_MAX], a[16], b[16], rest[32];
        CHECK(js8_text_to_frame(cases[i].text, frame, NULL), "pack %s\n", cases[i].text);
        CHECK(js8_frame_to_text(frame, out, sizeof(out)), "render %s\n", cases[i].text);
        CHECK(split3(out, a, b, rest), "split '%s'\n", out);
        CHECK(strcmp(a, "N2VGU") == 0, "to = '%s'\n", a);
        CHECK(strcmp(b, "OZ1LAV") == 0, "from = '%s'\n", b);
        CHECK(strcmp(rest, cases[i].want_rest) == 0,
              "rest = '%s', expected '%s'\n", rest, cases[i].want_rest);
    }
}

/* ⛔ A grid has no Directed form, and this is the one refusal with a
 * consequence for the caller: FT8 opens a pounce with "<them> <me> <grid>", so
 * ft8_qso.c must send a report instead. If this ever starts passing, something
 * has invented a frame for it. */
static void test_grid_opening_is_refused(void)
{
    expect_refused("N2VGU OZ1LAV JO65", "no Directed grid step in JS8");
    CHECK(js8_text_rest_is_grid("JO65"), "JO65 is a grid\n");
    CHECK(js8_text_rest_is_grid("AA00"), "AA00 is a grid\n");
    CHECK(!js8_text_rest_is_grid("RR73"), "RR73 is not a grid\n");
    CHECK(!js8_text_rest_is_grid("-07"), "-07 is not a grid\n");
    CHECK(!js8_text_rest_is_grid("73"), "73 is not a grid\n");
    CHECK(!js8_text_rest_is_grid(""), "empty is not a grid\n");
}

static void test_other_refusals(void)
{
    expect_refused("N2VGU OZ1LAV HELLO", "free text has no frame");
    expect_refused("N2VGU OZ1LAV +40", "outside packNum's -30..+31");
    expect_refused("N2VGU OZ1LAV -31", "outside packNum's -30..+31");
    expect_refused("N2VGU", "no third field");
    expect_refused("", "empty");
    /* A bare number is NOT a report: "73" is the sign-off, and a report without
     * a sign would make the two indistinguishable. */
    uint8_t frame[JS8_FRAME_BYTES];
    char out[JS8_TEXT_MAX];
    CHECK(js8_text_to_frame("N2VGU OZ1LAV 73", frame, NULL), "73 packs\n");
    CHECK(js8_frame_to_text(frame, out, sizeof(out)), "73 renders\n");
    CHECK(strcmp(out, "N2VGU OZ1LAV 73") == 0, "73 rendered as '%s'\n", out);
}

/* A CQ must be spelled so that the existing CQ detection sees it - ft8_qso.c
 * and the robot both test strncmp(text, "CQ ", 3). An HB must NOT. */
static void test_cq_detection(void)
{
    uint8_t frame[JS8_FRAME_BYTES];
    char out[JS8_TEXT_MAX];

    CHECK(js8_text_to_frame("CQ OZ1LAV JO65", frame, NULL), "CQ packs\n");
    CHECK(js8_frame_to_text(frame, out, sizeof(out)), "CQ renders\n");
    CHECK(strncmp(out, "CQ ", 3) == 0, "'%s' must start with 'CQ '\n", out);

    CHECK(js8_text_to_frame("HB OZ1LAV JO65", frame, NULL), "HB packs\n");
    CHECK(js8_frame_to_text(frame, out, sizeof(out)), "HB renders\n");
    CHECK(strncmp(out, "CQ ", 3) != 0, "a heartbeat must not read as a CQ: '%s'\n", out);
}

/* ⛔ THE OPERATOR'S CQ TEXT IS FREE-FORM, and the qualifier comes BEFORE the
 * callsign: the Call CQ presets in ft8_cq_modal.c are hand-edited and read
 * "CQ DX OZ1LAV JO65", "CQ POTA OZ1LAV". Taking the second token as the
 * callsign - which is what the first version of this file did - transmits a CQ
 * from a station called "DX". JS8 has no field for the qualifier, so it is
 * dropped; the callsign and the locator are what survive. */
static void test_cq_qualifier_before_the_callsign(void)
{
    struct { const char* in; const char* want; } cases[] = {
        { "CQ OZ1LAV JO65",      "CQ OZ1LAV JO65" },
        { "CQ OZ1LAV",           "CQ OZ1LAV"      },
        { "CQ DX OZ1LAV JO65",   "CQ OZ1LAV JO65" },
        { "CQ POTA OZ1LAV",      "CQ OZ1LAV"      },
        { "CQ FD OZ1LAV JO65",   "CQ OZ1LAV JO65" },
        { "CQ OZ1LAV POTA",      "CQ POTA"        },  /* see below */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint8_t frame[JS8_FRAME_BYTES];
        char out[JS8_TEXT_MAX];
        if (!js8_text_to_frame(cases[i].in, frame, NULL))
        {
            /* The last case has no callsign in the last position and no digit
             * in "POTA", so it must be REFUSED rather than produce a CQ from
             * "POTA". Recorded as a refusal, not as a wrong string. */
            CHECK(strcmp(cases[i].in, "CQ OZ1LAV POTA") == 0,
                  "'%s' should have packed\n", cases[i].in);
            continue;
        }
        CHECK(strcmp(cases[i].in, "CQ OZ1LAV POTA") != 0,
              "'CQ OZ1LAV POTA' must be refused - the last token is not a call\n");
        CHECK(js8_frame_to_text(frame, out, sizeof(out)), "render '%s'\n", cases[i].in);
        CHECK(strcmp(out, cases[i].want) == 0,
              "'%s' -> '%s', expected '%s'\n", cases[i].in, out, cases[i].want);
    }
}

/* A CQ with no callsign at all must not pack. The 50-bit codec is plain
 * alphanumeric and refuses almost nothing, so without an explicit test for
 * "looks like a callsign" these go out as a CQ from a word. */
static void test_cq_without_a_callsign_is_refused(void)
{
    expect_refused("CQ DX", "no callsign, just a qualifier");
    expect_refused("CQ POTA", "no callsign, just a qualifier");
    expect_refused("CQ CQ", "no callsign at all");
}

/* The report formatter's range is JS8's, not FT8's. Using FT8's would throw
 * away 6 dB at the bottom and 16 at the top of what the frame can carry. */
static void test_report_range(void)
{
    char buf[8];
    js8_fmt_report(-7, buf, sizeof(buf));
    CHECK(strcmp(buf, "-07") == 0, "-7 formatted as '%s'\n", buf);
    js8_fmt_report(2, buf, sizeof(buf));
    CHECK(strcmp(buf, "+02") == 0, "+2 formatted as '%s'\n", buf);
    js8_fmt_report(-40, buf, sizeof(buf));
    CHECK(strcmp(buf, "-30") == 0, "-40 clamped to '%s', expected -30\n", buf);
    js8_fmt_report(99, buf, sizeof(buf));
    CHECK(strcmp(buf, "+31") == 0, "99 clamped to '%s', expected +31\n", buf);
}

/* An unimplemented frame type must refuse rather than be rendered with the
 * wrong codec - it would put a different station's callsign on the screen. */
static void test_compound_frames_refuse(void)
{
    uint8_t frame[JS8_FRAME_BYTES];
    char out[JS8_TEXT_MAX];
    for (int t = 1; t <= 2; t++)
    {
        memset(frame, 0x5A, sizeof(frame));
        frame[0] = (uint8_t)((frame[0] & 0x1Fu) | (t << 5));
        CHECK(!js8_frame_to_text(frame, out, sizeof(out)),
              "frame type %d should refuse to render\n", t);
    }
}

/* Any command we did not map is still spelled out, so an operator sees what
 * arrived. Checked through a packed frame rather than by calling js8_cmd_text,
 * which would only test the table against itself. */
static void test_unmapped_command_is_spelled(void)
{
    js8_directed_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.to, sizeof(d.to), "OZ1LAV");
    snprintf(d.from, sizeof(d.from), "N2VGU");
    d.cmd = JS8_CMD_YES;
    d.num = JS8_NUM_NONE;

    uint8_t frame[JS8_FRAME_BYTES];
    char out[JS8_TEXT_MAX];
    CHECK(js8_pack_directed(&d, frame), "pack YES\n");
    CHECK(js8_frame_to_text(frame, out, sizeof(out)), "render YES\n");
    CHECK(strcmp(out, "OZ1LAV N2VGU YES") == 0, "got '%s'\n", out);
}

int main(void)
{
    printf("JS8 text seam\n");
    test_ladder_round_trips();
    test_rest_field_is_what_the_ladder_expects();
    test_grid_opening_is_refused();
    test_other_refusals();
    test_cq_detection();
    test_cq_qualifier_before_the_callsign();
    test_cq_without_a_callsign_is_refused();
    test_report_range();
    test_compound_frames_refuse();
    test_unmapped_command_is_spelled();

    if (g_fail) { printf("\nFAILED: %d check(s)\n", g_fail); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
