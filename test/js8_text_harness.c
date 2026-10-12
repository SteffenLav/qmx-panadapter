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

/* ---- J9: the Compound frames ---------------------------------------------
 *
 * ft8_screen.c picks the station and the grid out of the RENDERED LINE, not
 * out of the frame, so the shape js8_text.c chooses is a contract with it.
 * Those two rules are reimplemented here, exactly as split_msg3() is above:
 * if they drift, a compound station enters the table under its own locator and
 * nothing fails loudly.
 */

/* ft8_screen.c extract_call(): token 2, or token 3 when token 1 is "CQ" and
 * token 2 has no digit. Fewer than two tokens is a refusal. */
static int screen_extract_call(const char* msg, char* out, size_t cap)
{
    const char* t[3] = {0,0,0};
    size_t len[3] = {0,0,0};
    int n = 0;
    while (*msg == ' ') msg++;
    while (*msg && n < 3) {
        t[n] = msg;
        size_t l = 0;
        while (msg[l] && msg[l] != ' ') l++;
        len[n] = l; n++; msg += l;
        while (*msg == ' ') msg++;
    }
    if (n < 2) return 0;
    int pick = 1;
    int first_is_cq = (len[0] == 2 && t[0][0] == 'C' && t[0][1] == 'Q');
    if (first_is_cq && n >= 3) {
        int has_digit = 0;
        for (size_t i = 0; i < len[1]; i++)
            if (t[1][i] >= '0' && t[1][i] <= '9') has_digit = 1;
        if (!has_digit) pick = 2;
    }
    size_t o = len[pick];
    if (o >= cap) o = cap - 1;
    memcpy(out, t[pick], o);
    out[o] = '\0';
    return o > 0;
}

/* ft8_screen.c extract_grid(): the LAST token, if it looks like a locator. */
static void screen_extract_grid(const char* msg, char* out, size_t cap)
{
    out[0] = '\0';
    const char* p = msg;
    const char* last = 0; size_t ll = 0;
    while (*p == ' ') p++;
    while (*p) {
        const char* tok = p; size_t l = 0;
        while (p[l] && p[l] != ' ') l++;
        last = tok; ll = l; p += l;
        while (*p == ' ') p++;
    }
    if (!last || ll != 4) return;
    if (last[0] < 'A' || last[0] > 'R') return;
    if (last[1] < 'A' || last[1] > 'R') return;
    if (last[2] < '0' || last[2] > '9') return;
    if (last[3] < '0' || last[3] > '9') return;
    size_t o = ll; if (o >= cap) o = cap - 1;
    memcpy(out, last, o); out[o] = '\0';
}

/* A compound call with a grid. The station must come out of the rendered line
 * as the STATION and not as its locator - which is what "<call> <grid>", the
 * reference's own shape, would have given. */
static void test_compound_with_grid(void)
{
    js8_compound_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.call, sizeof(c.call), "PJ4/K1ABC");
    snprintf(c.grid, sizeof(c.grid), "JO65");
    c.cmd = JS8_CMD_NONE;
    c.num = JS8_NUM_NONE;

    uint8_t frame[JS8_FRAME_BYTES];
    char out[JS8_TEXT_MAX];
    CHECK(js8_pack_compound(&c, frame), "pack compound+grid\n");
    CHECK(js8_frame_to_text(frame, out, sizeof(out)), "render compound+grid\n");
    CHECK(strcmp(out, "HB PJ4/K1ABC JO65") == 0, "got '%s'\n", out);

    char call[24] = "", grid[8] = "";
    CHECK(screen_extract_call(out, call, sizeof(call)), "extract_call refused '%s'\n", out);
    CHECK(strcmp(call, "PJ4/K1ABC") == 0, "station read as '%s'\n", call);
    screen_extract_grid(out, grid, sizeof(grid));
    CHECK(strcmp(grid, "JO65") == 0, "grid read as '%s'\n", grid);

    /* NOT "CQ". A Compound frame is a station identifying itself, and
     * spelling it CQ would invite a reply to something that never called. */
    CHECK(strncmp(out, "CQ ", 3) != 0, "must not read as a CQ: '%s'\n", out);

    js8_compound_t back;
    CHECK(js8_unpack_compound(frame, &back), "unpack compound+grid\n");
    CHECK(strcmp(back.call, "PJ4/K1ABC") == 0, "call round-trip '%s'\n", back.call);
    CHECK(strcmp(back.grid, "JO65") == 0, "grid round-trip '%s'\n", back.grid);
    CHECK(back.cmd == JS8_CMD_NONE, "cmd should be none, got %u\n", back.cmd);
    CHECK(back.directed == false, "type should be FrameCompound\n");
}

/* No grid and no command: the 16-bit extra is nmaxgrid, which means NEITHER.
 * Two tokens, so extract_call still works - a one-token line would be dropped
 * by it outright. */
static void test_compound_bare_call(void)
{
    js8_compound_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.call, sizeof(c.call), "OZ1LAV/P");
    c.cmd = JS8_CMD_NONE;
    c.num = JS8_NUM_NONE;

    uint8_t frame[JS8_FRAME_BYTES];
    char out[JS8_TEXT_MAX];
    CHECK(js8_pack_compound(&c, frame), "pack bare compound\n");
    CHECK(js8_frame_to_text(frame, out, sizeof(out)), "render bare compound\n");
    CHECK(strcmp(out, "HB OZ1LAV/P") == 0, "got '%s'\n", out);

    char call[24] = "";
    CHECK(screen_extract_call(out, call, sizeof(call)), "extract_call refused '%s'\n", out);
    CHECK(strcmp(call, "OZ1LAV/P") == 0, "station read as '%s'\n", call);

    js8_compound_t back;
    CHECK(js8_unpack_compound(frame, &back), "unpack bare compound\n");
    CHECK(back.grid[0] == '\0', "grid should be empty, got '%s'\n", back.grid);
    CHECK(back.cmd == JS8_CMD_NONE, "cmd should be none\n");
}

/* A compound DIRECTED frame carries a command and ONE callsign. The lead
 * "<....>" fills the addressee slot so extract_call() still reads token 2. */
static void test_compound_directed_command(void)
{
    js8_compound_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.call, sizeof(c.call), "PJ4/K1ABC");
    c.cmd = JS8_CMD_73;
    c.num = JS8_NUM_NONE;
    c.directed = true;

    uint8_t frame[JS8_FRAME_BYTES];
    char out[JS8_TEXT_MAX];
    CHECK(js8_pack_compound(&c, frame), "pack compound 73\n");
    CHECK(js8_frame_to_text(frame, out, sizeof(out)), "render compound 73\n");
    CHECK(strcmp(out, "<....> PJ4/K1ABC 73") == 0, "got '%s'\n", out);

    char call[24] = "";
    CHECK(screen_extract_call(out, call, sizeof(call)), "extract_call refused '%s'\n", out);
    CHECK(strcmp(call, "PJ4/K1ABC") == 0, "station read as '%s'\n", call);

    js8_compound_t back;
    CHECK(js8_unpack_compound(frame, &back), "unpack compound 73\n");
    CHECK(back.cmd == JS8_CMD_73, "cmd round-trip %u\n", back.cmd);
    CHECK(back.directed == true, "type should be FrameCompoundDirected\n");
    CHECK(back.grid[0] == '\0', "a command frame carries no grid, got '%s'\n", back.grid);
}

/* SNR shares its byte with the number: bit 7 marks the SNR commands and the
 * low 6 bits carry packNum's 1..62. Getting that wrong reports a different
 * signal strength, which is exactly the kind of error nothing on air flags. */
static void test_compound_snr_round_trips(void)
{
    static const int reports[] = { -30, -7, 0, +5, +31 };
    for (unsigned i = 0; i < sizeof(reports) / sizeof(reports[0]); i++)
    {
        js8_compound_t c;
        memset(&c, 0, sizeof(c));
        snprintf(c.call, sizeof(c.call), "PJ4/K1ABC");
        c.cmd = JS8_CMD_SNR;
        c.num = reports[i];
        c.directed = true;

        uint8_t frame[JS8_FRAME_BYTES];
        char out[JS8_TEXT_MAX], want[64], rpt[8];
        CHECK(js8_pack_compound(&c, frame), "pack SNR %d\n", reports[i]);
        CHECK(js8_frame_to_text(frame, out, sizeof(out)), "render SNR %d\n", reports[i]);
        js8_fmt_report(reports[i], rpt, sizeof(rpt));
        snprintf(want, sizeof(want), "<....> PJ4/K1ABC SNR %s", rpt);
        CHECK(strcmp(out, want) == 0, "SNR %d: got '%s' want '%s'\n",
              reports[i], out, want);

        js8_compound_t back;
        CHECK(js8_unpack_compound(frame, &back), "unpack SNR %d\n", reports[i]);
        CHECK(back.cmd == JS8_CMD_SNR, "SNR %d: cmd %u\n", reports[i], back.cmd);
        CHECK(back.num == reports[i], "SNR %d: num %d\n", reports[i], back.num);
    }

    /* HEARTBEAT SNR is the only other number-carrying command, and it is
     * selected by bit 6 of the same byte. If js8_pack_cmd() forgot it, this
     * would come back as plain SNR with the right number - a wrong command
     * that reads as plausible. */
    js8_compound_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.call, sizeof(c.call), "OZ1LAV/P");
    c.cmd = JS8_CMD_HEARTBEAT_SNR;
    c.num = -12;
    uint8_t frame[JS8_FRAME_BYTES];
    js8_compound_t back;
    CHECK(js8_pack_compound(&c, frame), "pack HEARTBEAT SNR\n");
    CHECK(js8_unpack_compound(frame, &back), "unpack HEARTBEAT SNR\n");
    CHECK(back.cmd == JS8_CMD_HEARTBEAT_SNR, "cmd %u, not HEARTBEAT SNR\n", back.cmd);
    CHECK(back.num == -12, "num %d\n", back.num);
}

/* The 16-bit extra is a PARTITION, and the boundaries are where an off-by-one
 * turns a command into a locator. nbasegrid is inclusive on the grid side;
 * 32401..32409 and nmaxgrid itself mean neither. */
static void test_extra_partition_boundaries(void)
{
    struct { uint16_t extra; const char* what; } cases[] = {
        { 0,                  "grid"    },
        { JS8_NBASEGRID,      "grid"    },   /* inclusive */
        { JS8_NBASEGRID + 1,  "neither" },
        { JS8_NUSERGRID - 1,  "neither" },
        { JS8_NUSERGRID,      "cmd"     },
        { JS8_NMAXGRID - 1,   "cmd"     },
        { JS8_NMAXGRID,       "neither" },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        /* Build the frame by hand: the packer cannot reach the "neither"
         * band, and that band is precisely what a real station's frame may
         * carry. */
        uint16_t extra = cases[i].extra;
        uint64_t call = js8_pack_alnum50("OZ1LAV/P");
        CHECK(call != 0, "pack alnum50\n");
        uint8_t frame[JS8_FRAME_BYTES];
        memset(frame, 0, sizeof(frame));
        int pos = 0;
        #define PUT(v, n) do { for (int b = (n) - 1; b >= 0; b--) { \
            if (((uint64_t)(v) >> b) & 1u) frame[pos / 8] |= (uint8_t)(0x80u >> (pos % 8)); \
            pos++; } } while (0)
        PUT(JS8_FRAME_COMPOUND, 3);
        PUT(call, 50);
        PUT((extra >> 5) & 0x7FFu, 11);
        PUT((uint8_t)((extra & 0x1Fu) << 3), 8);
        #undef PUT
        CHECK(pos == 72, "built %d bits\n", pos);

        js8_compound_t c;
        CHECK(js8_unpack_compound(frame, &c), "unpack extra=%u\n", extra);
        int is_grid = (c.grid[0] != '\0');
        int is_cmd  = (c.cmd != JS8_CMD_NONE);
        if (strcmp(cases[i].what, "grid") == 0)
            CHECK(is_grid && !is_cmd, "extra=%u should be a grid (grid='%s' cmd=%u)\n",
                  extra, c.grid, c.cmd);
        else if (strcmp(cases[i].what, "cmd") == 0)
            CHECK(is_cmd && !is_grid, "extra=%u should be a command (grid='%s' cmd=%u)\n",
                  extra, c.grid, c.cmd);
        else
            CHECK(!is_cmd && !is_grid, "extra=%u should be neither (grid='%s' cmd=%u)\n",
                  extra, c.grid, c.cmd);
    }
}

/* A Heartbeat's extra and a Compound's extra are the SAME 16 bits read two
 * completely different ways. Reading one as the other is the mistake this
 * guards: bit 15 is the CQ flag in a Heartbeat and part of the number in a
 * Compound, so a Compound command would decode as a southern-hemisphere grid
 * and a CQ. */
static void test_heartbeat_and_compound_read_extra_differently(void)
{
    js8_compound_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.call, sizeof(c.call), "OZ1LAV/P");
    c.cmd = JS8_CMD_73;
    c.num = JS8_NUM_NONE;
    uint8_t frame[JS8_FRAME_BYTES];
    CHECK(js8_pack_compound(&c, frame), "pack compound 73\n");

    /* The same bits, read as a Heartbeat, must be REFUSED - the frame type
     * guard is what stops the misreading, so it has to be there. */
    js8_heartbeat_t hb;
    CHECK(!js8_unpack_heartbeat(frame, &hb),
          "a Compound frame must not unpack as a Heartbeat\n");

    /* And the converse. */
    js8_heartbeat_t h2;
    memset(&h2, 0, sizeof(h2));
    snprintf(h2.call, sizeof(h2.call), "OZ1LAV");
    snprintf(h2.grid, sizeof(h2.grid), "JO65");
    h2.is_cq = true;
    uint8_t hframe[JS8_FRAME_BYTES];
    js8_compound_t c2;
    CHECK(js8_pack_heartbeat(&h2, hframe), "pack heartbeat\n");
    CHECK(!js8_unpack_compound(hframe, &c2),
          "a Heartbeat frame must not unpack as a Compound\n");
}

/* The longest line J9 can produce must still fit the row that stores it.
 * ft8_test.c drops rather than truncates, so one character over is a decode
 * that silently never appears - the exact fault J9 exists to end. */
static void test_longest_line_fits_the_row(void)
{
    js8_compound_t c;
    memset(&c, 0, sizeof(c));
    /* 11 characters, the field's absolute maximum - and it has to have its
     * slashes AT POSITIONS 3 AND 7, because that is the only 11-character
     * shape the 50-bit codec can carry. "PJ4/K1ABC/P" is 11 characters too
     * and is REFUSED, which is the point of alnum50_can_carry(). */
    snprintf(c.call, sizeof(c.call), "PJ4/K1A/BCD");
    c.cmd = JS8_CMD_HEARTBEAT_SNR;
    c.num = 31;
    c.directed = true;

    uint8_t frame[JS8_FRAME_BYTES];
    char out[JS8_TEXT_MAX];
    CHECK(js8_pack_compound(&c, frame), "pack longest\n");
    CHECK(js8_frame_to_text(frame, out, sizeof(out)), "render longest\n");
    /* ⛔ ASSERT THE WHOLE STRING, NOT JUST THE LENGTH. A length-only
     * check passed a mutant that put rest[] back to 16 characters: the
     * line came out as "... HEARTBEAT SNR +" - 33 characters, comfortably
     * under the limit, with the REPORT cut to its sign. Shorter than the
     * bound and wrong is exactly the failure a bound cannot see. */
    CHECK(strcmp(out, "<....> PJ4/K1A/BCD HEARTBEAT SNR +31") == 0,
          "got '%s'\n", out);
    /* FT8_TEXT_MAX_LEN is 40 in main/ui/ft8_screen.h; 39 usable. Hardcoded
     * here because the harness does not include firmware headers. */
    CHECK(strlen(out) < 39, "longest line is %u chars: '%s'\n",
          (unsigned)strlen(out), out);
    printf("  longest J9 line: %u chars '%s'\n", (unsigned)strlen(out), out);
}

/* A callsign that will not pack must refuse, not shorten: a truncated
 * callsign is a different station. */
static void test_unpackable_callsign_refuses(void)
{
    js8_compound_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.call, sizeof(c.call), "   ");     /* nothing in the alphabet */
    c.cmd = JS8_CMD_NONE;
    c.num = JS8_NUM_NONE;
    uint8_t frame[JS8_FRAME_BYTES];
    CHECK(!js8_pack_compound(&c, frame), "an empty callsign must refuse\n");

    /* A report outside packNum's range is not the report we would log. */
    js8_compound_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.call, sizeof(d.call), "OZ1LAV/P");
    d.cmd = JS8_CMD_SNR;
    d.num = 40;
    CHECK(!js8_pack_compound(&d, frame), "+40 must refuse, not clamp\n");

    /* A callsign the 50-bit FIELD cannot hold. "PJ4/K1ABC/P" is 11
     * characters, which looks like it fits, but the field allows '/' only at
     * positions 3 and 7 - the packer inserts a space at 7 and the last
     * character falls off, so it would transmit as "PJ4/K1ABC/". A different
     * station. It must refuse, in BOTH packers that use this codec. */
    js8_compound_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.call, sizeof(e.call), "PJ4/K1ABC/P");
    e.cmd = JS8_CMD_NONE;
    e.num = JS8_NUM_NONE;
    CHECK(!js8_pack_compound(&e, frame),
          "a callsign the field cannot carry must refuse\n");

    js8_heartbeat_t h;
    memset(&h, 0, sizeof(h));
    snprintf(h.call, sizeof(h.call), "PJ4/K1ABC/P");
    snprintf(h.grid, sizeof(h.grid), "JO65");
    h.is_cq = true;
    CHECK(!js8_pack_heartbeat(&h, frame),
          "the Heartbeat packer has the same field and must refuse too\n");

    /* And the 11-character shape that IS carryable must still pack. */
    js8_compound_t g;
    memset(&g, 0, sizeof(g));
    snprintf(g.call, sizeof(g.call), "PJ4/K1A/BCD");
    g.cmd = JS8_CMD_NONE;
    g.num = JS8_NUM_NONE;
    CHECK(js8_pack_compound(&g, frame),
          "slashes at 3 and 7 are carryable and must NOT refuse\n");
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
    test_compound_with_grid();
    test_compound_bare_call();
    test_compound_directed_command();
    test_compound_snr_round_trips();
    test_extra_partition_boundaries();
    test_heartbeat_and_compound_read_extra_differently();
    test_longest_line_fits_the_row();
    test_unpackable_callsign_refuses();
    test_unmapped_command_is_spelled();

    if (g_fail) { printf("\nFAILED: %d check(s)\n", g_fail); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
