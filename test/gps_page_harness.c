/* Host test for the GPS page layout (main/ui/gps_page.c).
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main -I main/unit_gps -o gps_page_harness \
 *       test/gps_page_harness.c main/ui/gps_page.c -lm && ./gps_page_harness
 *
 * WHY. The page has to look like the QMX's own "Hardware tests | GPS viewer",
 * which was captured off the bench on 2026-10-08 - so the reference is an
 * exact column layout, not a judgement. The failures this catches are all
 * PLAUSIBLE-LOOKING ones:
 *
 *  - A latitude printed as decimal degrees instead of degrees and decimal
 *    minutes. 55.714487 and "55 42.869" are the same place; 55 42.869 read as
 *    decimal degrees is 200 km away, and nothing on screen says which it is.
 *  - A grid square one square out. JO65FR and JO65FQ both look right.
 *  - A satellite drawn in the wrong quadrant, because azimuth was treated as
 *    a clock face or the plot's x and y radii were made equal on a character
 *    cell that is twice as tall as it is wide.
 *  - The QMX-source page quietly hiding the rows the radio cannot fill,
 *    instead of showing them empty, which would make it a different page.
 *
 * It renders with no LVGL, no driver and no clock, so it runs anywhere.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "ui/gps_page.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

/* The bench's own fix, 2026-10-08, as the QMX viewer printed it:
 *   Grid      JO65FR
 *   Latitude  55 42.869013 N
 *   Longitude 12 28.290529 E
 * 55 deg 42.869013 min = 55.714484 deg; 12 deg 28.290529 min = 12.471509 deg.
 */
#define BENCH_LAT 55.714483550
#define BENCH_LON 12.471508817

static void fill(unit_gps_info_t *in)
{
    memset(in, 0, sizeof(*in));
    in->valid    = true;
    in->fix_type = 3;
    in->has_time = true;
    in->year = 2026; in->mon = 10; in->mday = 8;
    in->hour = 16;   in->min = 38; in->sec  = 19;
    in->has_pos = true;
    in->lat_deg = BENCH_LAT;
    in->lon_deg = BENCH_LON;
    in->has_alt = true;
    in->alt_m   = 15.222f;
    in->fix_sats = 14;
    in->tot_sats = 20;
    in->avg_snr  = 28;

    struct { int prn, el, az, snr; const char *t; bool used; } s[] = {
        { 13, 84, 241, 31, "GP", true  },
        { 12, 78, 239, 30, "GP", true  },
        { 24, 47, 147, 31, "GP", false },
        { 25, 46, 255, 24, "GP", false },
        { 19, 36,  57, 28, "GP", true  },
        { 38, -1,  -1, -1, "GP", false },   /* in view, nothing else reported */
        { 40, 81, 112, -1, "GB", false },   /* Beidou, in view but not tracked */
    };
    in->n_sats = (int)(sizeof(s) / sizeof(s[0]));
    for (int i = 0; i < in->n_sats; i++) {
        in->sat[i].prn      = (uint8_t)s[i].prn;
        in->sat[i].elev_deg = (int8_t)s[i].el;
        in->sat[i].azim_deg = (int16_t)s[i].az;
        in->sat[i].snr_db   = (int8_t)s[i].snr;
        in->sat[i].talker[0] = s[i].t[0];
        in->sat[i].talker[1] = s[i].t[1];
        in->sat[i].talker[2] = '\0';
        in->sat[i].used      = s[i].used;
    }
}

// The value column must sit where the QMX puts it, or the two pages read as
// two designs rather than one page with two sources.
static void expect_row(char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                       int row, const char *label, const char *value)
{
    CHECK(strncmp(lines[row], label, strlen(label)) == 0,
          "row %d should start \"%s\", got \"%.20s\"\n", row, label, lines[row]);
    CHECK(strncmp(lines[row] + 10, value, strlen(value)) == 0,
          "row %d column 10 should be \"%s\", got \"%.20s\"\n",
          row, value, lines[row] + 10);
}

static void test_left_column(void)
{
    unit_gps_info_t in;
    char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1];
    fill(&in);
    gps_page_render(&in, GPS_PAGE_SRC_MODULE, lines, NULL, NULL);

    expect_row(lines, 0, "Validity",  "A");
    expect_row(lines, 1, "Fix",       "3D");
    expect_row(lines, 2, "UT date",   "08-OCT-26");
    expect_row(lines, 3, "UT time",   "16:38:19");
    expect_row(lines, 4, "Grid",      "JO65FR");
    expect_row(lines, 5, "Latitude",  "55 42.869");
    expect_row(lines, 6, "Longitude", "12 28.290");
    expect_row(lines, 7, "Altitude",  "15.222");
    expect_row(lines, 8, "Fix sats",  "14");
    expect_row(lines, 9, "Tot sats",  "20");
    expect_row(lines, 10, "Avg SNR",  "28");
}

/* Degrees and decimal MINUTES. A decimal-degrees number in this field is read
 * as degrees-and-minutes by eye and is wrong by a factor of 1.667 in the
 * fraction - plausible on a map, and silent. */
static void test_latlon_is_degrees_minutes(void)
{
    unit_gps_info_t in;
    char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1];
    fill(&in);
    gps_page_render(&in, GPS_PAGE_SRC_MODULE, lines, NULL, NULL);
    CHECK(strstr(lines[5], "55 42.869013 N") != NULL,
          "latitude should be \"55 42.869013 N\", got \"%.30s\"\n", lines[5] + 10);
    CHECK(strstr(lines[6], "12 28.290529 E") != NULL,
          "longitude should be \"12 28.290529 E\", got \"%.30s\"\n", lines[6] + 10);

    // Southern and western hemispheres must show S and W, not a minus sign.
    in.lat_deg = -33.8688;
    in.lon_deg = 151.2093;
    gps_page_render(&in, GPS_PAGE_SRC_MODULE, lines, NULL, NULL);
    CHECK(strstr(lines[5], " S") != NULL, "negative latitude should read S\n");
    CHECK(strstr(lines[5], "-") == NULL, "latitude should not carry a minus\n");
    in.lon_deg = -58.3816;
    gps_page_render(&in, GPS_PAGE_SRC_MODULE, lines, NULL, NULL);
    CHECK(strstr(lines[6], " W") != NULL, "negative longitude should read W\n");
}

static void test_grid(void)
{
    char g[7];
    // The bench, as the QMX itself reported it.
    gps_page_grid(BENCH_LAT, BENCH_LON, g);
    CHECK(strcmp(g, "JO65FR") == 0, "bench grid \"%s\", expected JO65FR\n", g);

    // Two published references, to catch a field/square/subsquare mix-up.
    gps_page_grid(0.0, 0.0, g);
    CHECK(strncmp(g, "JJ00", 4) == 0, "0,0 grid \"%s\", expected JJ00aa\n", g);
    gps_page_grid(-33.8688, 151.2093, g);   // Sydney
    CHECK(strncmp(g, "QF56", 4) == 0, "Sydney grid \"%s\", expected QF56\n", g);
}

static void test_sat_table(void)
{
    unit_gps_info_t in;
    char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1];
    fill(&in);
    gps_page_render(&in, GPS_PAGE_SRC_MODULE, lines, NULL, NULL);

    CHECK(strncmp(lines[12], " 13 84 241 31  GPS", 18) == 0,
          "sat row 0 \"%.20s\"\n", lines[12]);
    /* A satellite with no elevation/azimuth/SNR keeps its columns BLANK, as
     * the QMX does - printing 0 or -1 would read as a real measurement. */
    CHECK(strncmp(lines[17], " 38            GPS", 18) == 0,
          "sat row with nothing reported \"%.20s\"\n", lines[17]);
    CHECK(strstr(lines[18], "Beidou") != NULL,
          "constellation should spell out Beidou, got \"%.24s\"\n", lines[18]);
}

/* Azimuth 0 is NORTH and at the TOP; 90 is east and to the right. Getting this
 * wrong draws a correct-looking sky with every satellite in the wrong place. */
static void test_sky_quadrants(void)
{
    unit_gps_info_t in;
    char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1];
    memset(&in, 0, sizeof(in));
    in.n_sats = 4;
    const int az[4] = { 0, 90, 180, 270 };
    for (int i = 0; i < 4; i++) {
        in.sat[i].prn = (uint8_t)(11 + i);
        in.sat[i].elev_deg = 10;             /* near the rim */
        in.sat[i].azim_deg = (int16_t)az[i];
        in.sat[i].snr_db = 20;
        in.sat[i].talker[0] = 'G'; in.sat[i].talker[1] = 'P';
    }
    gps_page_render(&in, GPS_PAGE_SRC_MODULE, lines, NULL, NULL);

    int row[4] = { -1, -1, -1, -1 };
    for (int r = 0; r < GPS_PAGE_ROWS; r++) {
        for (int i = 0; i < 4; i++) {
            char want[4];
            snprintf(want, sizeof(want), "%d", 11 + i);
            const char *p = strstr(lines[r] + 28, want);
            if (p && row[i] < 0) row[i] = r;
        }
    }
    CHECK(row[0] >= 0 && row[0] < 4, "azimuth 0 should be near the TOP, row %d\n", row[0]);
    CHECK(row[2] > 17, "azimuth 180 should be near the BOTTOM, row %d\n", row[2]);
    CHECK(row[1] >= 9 && row[1] <= 13, "azimuth 90 should be mid-height, row %d\n", row[1]);
    CHECK(row[3] >= 9 && row[3] <= 13, "azimuth 270 should be mid-height, row %d\n", row[3]);
}

/* ⛔ THE RIM IS SEPARATED DOTS, AND IT MUST STILL READ AS CLOSED.
 *
 * This test has been WRONG ONCE in the direction it is now testing against.
 * It used to assert `top >= 8` - that the rim's top row carried a continuous
 * RUN of cells - because a run was the evidence that the outline had stopped
 * being "two lenses put together (a UFO)", 2026-10-09. The operator then saw
 * the result on the glass and rejected it: the runs read as straight lines
 * welded onto a circle, and he marked them in red on a screenshot. A test can
 * pin the wrong property perfectly.
 *
 * So both properties are pinned here now, and the first one is the one that
 * reverses:
 *
 *   1. NO TWO RIM DOTS TOUCH. Chebyshev distance >= 2 between any pair, so no
 *      run can exist in any direction - horizontal, vertical or diagonal.
 *   2. The ring is still CLOSED: every 30-degree sector around the centre
 *      contains at least one rim dot. That is what rules out the UFO without
 *      demanding the runs that caused this.
 */
/* Mirrors of gps_page.c's private plot geometry. The file already hardcodes
 * the plot's first column (28) in other tests, so this is the existing
 * convention - but it is a MIRROR, and a change to the plot geometry has to
 * come here too. */
#define T_PLOT_CX  54
#define T_PLOT_CY  11
#define T_PLOT_RX  24
#define T_PLOT_RY  11

/* ⛔ THE RINGS ARE NOT CHARACTERS, AND THIS TEST HAS NOW BEEN WRONG TWICE.
 *
 * Round 1 asserted the rim's top row carried a continuous RUN of cells - that
 * was the evidence the outline had stopped being "two lenses (a UFO)".
 * Rejected on the glass: the runs read as straight lines welded to a circle.
 * Round 2 forbade horizontal and diagonal neighbours. Also rejected: breaking
 * the runs just left an evenly spaced ". . .", measured here as 11 dotted
 * horizontal runs of three or more.
 *
 * ⭐ The conclusion is that the 15x27 px character cell is the wrong
 * instrument for a curve, at any spacing. gps_page.c now reports the ring
 * GEOMETRY and each view draws a real thin faint-grey oval - LVGL on the
 * Tab5, SVG in the browser. So what is testable here is (a) that NO ring is
 * drawn into the grid any more, and (b) that the geometry is right, because
 * both views now trust it completely.
 */
static void test_no_ring_characters_in_the_grid(void)
{
    unit_gps_info_t in;
    char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1];
    memset(&in, 0, sizeof(in));
    gps_page_render(&in, GPS_PAGE_SRC_MODULE, lines, NULL, NULL);

    /* With no satellites, the only dots left in the plot must be the two
     * AXES: the centre column and the centre row. Those stay as characters on
     * purpose - they are straight lines, which the grid draws perfectly, and
     * they are in the screenshot the operator approved. */
    int stray = 0, sr = -1, sc = -1;
    for (int r = 0; r < GPS_PAGE_ROWS; r++)
        for (int c = 28; c < GPS_PAGE_COLS; c++) {
            if (lines[r][c] != '.') continue;
            if (r == T_PLOT_CY || c == T_PLOT_CX) continue;   /* an axis */
            stray++;
            if (sr < 0) { sr = r; sc = c; }
        }
    CHECK(stray == 0,
          "%d ring characters still in the grid (first at row %d col %d) - the "
          "views draw the rings now, and a dotted one underneath is the look "
          "that was rejected three times\n", stray, sr, sc);

    /* The axes are still there - this test must not pass by the plot having
     * vanished altogether. */
    int axis = 0;
    for (int r = 0; r < GPS_PAGE_ROWS; r++) if (lines[r][T_PLOT_CX] == '.') axis++;
    CHECK(axis >= 10, "the N-S axis has only %d dots - the plot is gone\n", axis);
}

/* The geometry both views draw from. Nothing checks these on the glass: a
 * wrong radius there just looks like a slightly different plot, and the two
 * surfaces would drift apart without anyone noticing. */
static void test_ring_geometry(void)
{
    gps_page_ring_t rg[GPS_PAGE_MAX_RINGS];
    int n = gps_page_rings(rg, GPS_PAGE_MAX_RINGS);

    CHECK(n == 2, "expected 2 rings (horizon + 45 deg), got %d\n", n);
    if (n < 2) return;

    /* The horizon fills the plot; 45 degrees is half way in. */
    CHECK(rg[0].elev_deg == 0 && rg[1].elev_deg == 45,
          "rings should be 0 and 45 degrees, got %d and %d\n",
          rg[0].elev_deg, rg[1].elev_deg);
    CHECK(rg[0].rx == (double)T_PLOT_RX && rg[0].ry == (double)T_PLOT_RY,
          "the horizon ring is %gx%g, expected %dx%d\n",
          rg[0].rx, rg[0].ry, T_PLOT_RX, T_PLOT_RY);
    CHECK(rg[1].rx == rg[0].rx / 2.0 && rg[1].ry == rg[0].ry / 2.0,
          "the 45-degree ring should be half the horizon, got %gx%g\n",
          rg[1].rx, rg[1].ry);

    /* ⛔ CELL CENTRES, NOT CELL CORNERS. Both views multiply by their own cell
     * size, so a half-cell error here puts every ring half a character off
     * the axes it is supposed to be concentric with - on both surfaces at
     * once, which is exactly the kind of fault a shared source hides. */
    for (int i = 0; i < n; i++) {
        CHECK(rg[i].cx == (double)T_PLOT_CX + 0.5 && rg[i].cy == (double)T_PLOT_CY + 0.5,
              "ring %d centre is (%g,%g), expected cell centres (%g,%g)\n",
              i, rg[i].cx, rg[i].cy, (double)T_PLOT_CX + 0.5, (double)T_PLOT_CY + 0.5);
    }
}

/* The QMX page must keep the rows it cannot fill, showing them empty. Hiding
 * them would make it a different page, which is exactly what the operator
 * asked to avoid. */
static void test_qmx_source_keeps_its_rows(void)
{
    unit_gps_info_t in;
    char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1];
    fill(&in);
    gps_page_render(&in, GPS_PAGE_SRC_QMX, lines, NULL, NULL);

    expect_row(lines, 8,  "Fix sats", "-");
    expect_row(lines, 9,  "Tot sats", "-");
    expect_row(lines, 10, "Avg SNR",  "-");
    expect_row(lines, 4, "Grid", "JO65FR");      /* this one it CAN fill */
    CHECK(strstr(gps_page_title(GPS_PAGE_SRC_QMX, -1), "QMX") != NULL,
          "QMX title should name the radio: \"%s\"\n",
          gps_page_title(GPS_PAGE_SRC_QMX, -1));
}

/* ⛔ THE TITLE NAMES THE PORT, AND MUST NOT CLAIM A VERSION.
 *
 * It read "Module v2.1 on the M-Bus" for every Tab5-side receiver, hardcoded -
 * including a Unit GPS v1.1 on PORT.A, which is a different module on a
 * different connector. GPS_PAGE_SRC_MODULE covers both, so the source alone
 * cannot say which, and nothing read a version from the hardware.
 *
 * The version is NOT recoverable: the baud rate does not discriminate, because
 * the v2.1's ATGM336H-6N defaults to 9600 and runs at 115200 on this bench. So
 * these checks pin the port AND assert the absence of a version number - the
 * second half is the one that would catch someone helpfully putting it back. */
static void test_title_names_the_port_not_a_version(void)
{
    const char *porta = gps_page_title(GPS_PAGE_SRC_MODULE, UNIT_GPS_PORTA_RX_GPIO);
    const char *mbus  = gps_page_title(GPS_PAGE_SRC_MODULE, UNIT_GPS_MBUS_RX_GPIO);

    CHECK(strstr(porta, "PORT.A") != NULL, "PORT.A title: \"%s\"\n", porta);
    CHECK(strstr(mbus,  "M-Bus")  != NULL, "M-Bus title: \"%s\"\n", mbus);
    CHECK(strcmp(porta, mbus) != 0, "the two ports must not share a title\n");

    CHECK(strstr(porta, "v2.1") == NULL && strstr(mbus, "v2.1") == NULL,
          "no title may claim v2.1 - nothing checks it\n");
    CHECK(strstr(porta, "v1.1") == NULL && strstr(mbus, "v1.1") == NULL,
          "no title may claim v1.1 either\n");

    /* A pin we do not recognise still says which line is being read - the one
     * thing a mis-wired receiver needs to show. */
    const char *odd = gps_page_title(GPS_PAGE_SRC_MODULE, 7);
    CHECK(strstr(odd, "GPIO7") != NULL, "unknown pin should print itself: \"%s\"\n", odd);

    /* And the QMX never acquires a port, whatever pin is passed. */
    CHECK(strstr(gps_page_title(GPS_PAGE_SRC_QMX, UNIT_GPS_MBUS_RX_GPIO), "M-Bus") == NULL,
          "the QMX receiver has no Tab5 port\n");
}

static void test_no_fix_says_so(void)
{
    unit_gps_info_t in;
    char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1];
    memset(&in, 0, sizeof(in));
    in.has_time = true;
    in.year = 2026; in.mon = 10; in.mday = 8;
    in.hour = 16; in.min = 0; in.sec = 0;
    in.avg_snr = -1;
    gps_page_render(&in, GPS_PAGE_SRC_MODULE, lines, NULL, NULL);

    expect_row(lines, 0, "Validity", "V");
    expect_row(lines, 4, "Grid", "-");
    /* NOT 0 0: a position of zero is a real place in the Atlantic, and a page
     * that shows it cannot be told from one that has a fix there. */
    CHECK(strstr(lines[5], "0 0.000000") == NULL,
          "no position must not render as 0,0: \"%.30s\"\n", lines[5] + 10);
}

static void dump(void)
{
    unit_gps_info_t in;
    char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1];
    fill(&in);
    gps_page_render(&in, GPS_PAGE_SRC_MODULE, lines, NULL, NULL);
    printf("\n--- rendered page ---\n");
    for (int r = 0; r < GPS_PAGE_ROWS; r++) {
        const char *l = lines[r];
        int end = GPS_PAGE_COLS;
        while (end > 0 && l[end - 1] == ' ') end--;
        printf("%02d|%.*s\n", r, end, l);
    }
}

int main(int argc, char **argv)
{
    printf("gps_page harness\n");
    test_left_column();
    test_latlon_is_degrees_minutes();
    test_grid();
    test_sat_table();
    test_sky_quadrants();
    test_no_ring_characters_in_the_grid();
    test_ring_geometry();
    test_qmx_source_keeps_its_rows();
    test_no_fix_says_so();
    test_title_names_the_port_not_a_version();

    if (argc > 1 && strcmp(argv[1], "--dump") == 0) dump();

    if (g_fail) { printf("\nFAILURES ABOVE\n"); return 1; }
    printf("\nALL PASS\n");
    return 0;
}
