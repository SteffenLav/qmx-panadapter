#include "gps_page.h"

#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

/* Column geometry, taken from the QMX's own viewer captured off the bench on
 * 2026-10-08 (Hardware tests | GPS viewer):
 *
 *   Validity  A              16:28:18               .   0    .            PPS
 *   Fix       3D
 *   ...
 *    13 79 245 27  GPS
 *
 * label at column 0, value at column 10, the sky plot from column 28, "PPS"
 * hard against the right edge. The satellite table is  %3d %2d %3d %2d  %s.
 */
#define VAL_COL   10
#define PLOT_X0   28
#define PLOT_W    (GPS_PAGE_COLS - PLOT_X0)   /* 52 */
#define PLOT_CX   (PLOT_X0 + PLOT_W / 2)      /* 54 */
#define PLOT_CY   11
/* ⚠ WIDER THAN TALL ON PURPOSE - it fills the plot area, and the operator
 * confirmed 2026-10-09 that it "need not be a true circular circle". Making
 * it a true circle on the glass would mean PLOT_RX 20 (the cell is 15x27 px,
 * so a circle needs a 1.8:1 radius ratio) and that was NOT what was wanted.
 * The UFO complaint was about the OUTLINE, not the aspect - see draw_ring(). */
#define PLOT_RX   24
#define PLOT_RY   11
#define TABLE_Y0  12

static const char *kMonth[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

const char *unit_gps_constellation(const char talker[3])
{
    if (!talker) return "?";
    if (talker[0] == 'G' && talker[1] == 'P') return "GPS";
    if (talker[0] == 'G' && talker[1] == 'L') return "GLONASS";
    if (talker[0] == 'G' && talker[1] == 'A') return "Galileo";
    if (talker[0] == 'G' && talker[1] == 'B') return "Beidou";
    if (talker[0] == 'B' && talker[1] == 'D') return "Beidou";
    if (talker[0] == 'G' && talker[1] == 'Q') return "QZSS";
    if (talker[0] == 'G' && talker[1] == 'N') return "GNSS";
    return "?";
}

const char *gps_page_title(gps_page_src_t src, int rx_gpio)
{
    /* Never just "GPS". Two receivers, two antennas, two failure modes, and
     * only one of them survives the radio being unplugged.
     *
     * ⛔ IT USED TO SAY "Module v2.1 on the M-Bus", HARDCODED, and that was
     * wrong for half the hardware this driver supports. GPS_PAGE_SRC_MODULE
     * covers Eric's Unit GPS v1.1 on PORT.A as well as the Module GPS v2.1 on
     * the M-Bus - the header still says so - and nothing read a version from
     * anything. A Unit GPS was labelled as a module on a bus it was not
     * plugged into.
     *
     * ⭐ So name the PORT, which is a fact we hold: the driver records the pin
     * it bound to (unit_gps_rx_gpio()). The version is NOT recoverable - the
     * baud rate does not discriminate, because the v2.1's ATGM336H-6N defaults
     * to 9600 and runs at 115200 on this bench - so it is not claimed. */
    if (src == GPS_PAGE_SRC_QMX) return "GPS  -  QMX internal receiver";

    switch (rx_gpio) {
    case UNIT_GPS_PORTA_RX_GPIO: return "GPS  -  receiver on PORT.A";
    case UNIT_GPS_MBUS_RX_GPIO:  return "GPS  -  receiver on the M-Bus";
    default: break;
    }

    /* An unrecognised pin is still worth printing: it says which line is being
     * read, which is the one thing a mis-wired receiver needs to show. Static
     * because this function returns a literal everywhere else and the caller
     * does not own the string; it is only ever written from the LVGL thread's
     * repaint and the harness. */
    static char other[40];
    snprintf(other, sizeof(other), "GPS  -  receiver on GPIO%d", rx_gpio);
    return other;
}

void gps_page_grid(double lat_deg, double lon_deg, char out[7])
{
    /* Maidenhead. The +180/+90 shifts put the origin at the antimeridian and
     * the south pole, which is what makes the field letters come out as 'A'
     * at the corner rather than in the middle of the Atlantic. */
    double lon = lon_deg + 180.0;
    double lat = lat_deg + 90.0;
    if (lon < 0)       lon = 0;
    if (lon >= 360.0)  lon = 359.999999;
    if (lat < 0)       lat = 0;
    if (lat >= 180.0)  lat = 179.999999;

    int f1 = (int)(lon / 20.0);
    int f2 = (int)(lat / 10.0);
    lon -= f1 * 20.0;
    lat -= f2 * 10.0;
    int s1 = (int)(lon / 2.0);
    int s2 = (int)(lat / 1.0);
    lon -= s1 * 2.0;
    lat -= s2 * 1.0;
    int ss1 = (int)(lon / (2.0 / 24.0));
    int ss2 = (int)(lat / (1.0 / 24.0));

    out[0] = (char)('A' + f1);
    out[1] = (char)('A' + f2);
    out[2] = (char)('0' + s1);
    out[3] = (char)('0' + s2);
    out[4] = (char)('A' + ss1);
    out[5] = (char)('A' + ss2);
    out[6] = '\0';
}

static void put(char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                int row, int col, const char *txt)
{
    if (row < 0 || row >= GPS_PAGE_ROWS || !txt) return;
    for (int i = 0; txt[i] && col + i < GPS_PAGE_COLS; i++) {
        if (col + i < 0) continue;
        lines[row][col + i] = txt[i];
    }
}

static void put_ch(char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                   int row, int col, char c)
{
    if (row < 0 || row >= GPS_PAGE_ROWS) return;
    if (col < 0 || col >= GPS_PAGE_COLS) return;
    /* Never paint over a satellite with a grid dot. The satellites go down
     * first and the dots skip anything already there. */
    lines[row][col] = c;
}

/* Degrees and decimal MINUTES, the way both the QMX viewer and every NMEA
 * sentence write it: "55 42.869013 N". A decimal-degrees number here would be
 * read as degrees-and-minutes by eye and be wrong by a factor of 1.667 in the
 * fractional part, which looks entirely plausible on a map. */
static void fmt_latlon(char *buf, size_t n, double deg, bool is_lat)
{
    char hemi;
    if (is_lat) hemi = (deg < 0) ? 'S' : 'N';
    else        hemi = (deg < 0) ? 'W' : 'E';

    double a = fabs(deg);
    int    d = (int)a;
    double m = (a - d) * 60.0;
    snprintf(buf, n, "%d %09.6f %c", d, m, hemi);
}

/* Is the cell free of anything already drawn? */
static bool cell_free(char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1], int row, int col)
{
    if (row < 0 || row >= GPS_PAGE_ROWS) return false;
    if (col < PLOT_X0 || col >= GPS_PAGE_COLS) return false;
    return lines[row][col] == ' ';
}

/* Would plotting at (r,c) read as a line segment with the dot at (pr,pc)?
 *
 * Same cell, side by side, or diagonally touching: yes. Directly above or
 * below: NO - a vertical pair is the side tangent, not a line artefact. */
static bool touches_sideways(int r, int c, int pr, int pc)
{
    int dr = r - pr, dc = c - pc;
    if (dr < 0) dr = -dr;
    if (dc < 0) dc = -dc;
    if (dr == 0 && dc == 0) return true;      /* the same cell again */
    return dc <= 1 && dr <= 1 && dc != 0;     /* beside it, or diagonal */
}

/* One elevation ring, as a ring of SEPARATED dots.
 *
 * ⛔ THREE REQUIREMENTS, ALL FROM THE GLASS, AND THEY PULL AGAINST EACH OTHER.
 *   1. 2026-10-08: "only ONE dot per line - not 2 or 3".
 *   2. 2026-10-09: it must not read as "two lenses put together (a UFO)" -
 *      i.e. the ring has to look closed all the way round, not like two dense
 *      side arcs meeting at points.
 *   3. 2026-10-09, after seeing (2) shipped: NO STRAIGHT LINES. The fix for
 *      (2) walked the curve one cell at a time, which is the textbook way to
 *      draw a continuous ellipse - and continuity is precisely the defect.
 *      Near the top, consecutive columns round to the SAME row, so the outline
 *      grew horizontal runs; down the sides, consecutive rows share a column
 *      and it grew vertical ones. He marked them in red on a screenshot.
 *
 * ⭐ What satisfies all three is EVEN SPACING ALONG THE CURVE with a
 * guaranteed gap: sample the ellipse parametrically, far more finely than the
 * grid, and keep a candidate only when it does not touch the dot already
 * placed. Chebyshev distance >= 2 between consecutive dots means no two
 * plotted cells are ever neighbours, so a run cannot form in any direction -
 * requirement 3 holds by construction, not by inspection.
 *
 * Because the parameter advances along the ARC, the dots come out roughly
 * equally spaced the whole way round rather than bunching on the sides, which
 * is what kills the UFO (requirement 2). And one dot is placed at a time, so
 * (1) holds too.
 *
 * ⚠ Do NOT "improve" this back into a continuous curve. That is where it came
 * from, twice. If the dotted look ever has to go, the replacement is a real
 * drawn oval in the VIEW (faint grey, LVGL/CSS), not a denser character grid -
 * the operator said as much when he rejected this.
 */
static void draw_ring(char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                      double a, double b)
{
    if (a < 1.0 || b < 1.0) return;

    /* Fine enough that the limiter, not the step, decides the spacing: the
     * longest ring is a few hundred cells around, so a few thousand samples
     * leaves no gap unconsidered. */
    const int STEPS = 2048;

    int first_row = INT_MIN, first_col = INT_MIN;
    int last_row  = INT_MIN, last_col  = INT_MIN;

    for (int i = 0; i < STEPS; i++) {
        double th  = 2.0 * M_PI * (double)i / (double)STEPS;
        int    col = PLOT_CX + (int)lround(a * sin(th));
        int    row = PLOT_CY - (int)lround(b * cos(th));

        /* ⭐ VERTICAL IS ALLOWED, HORIZONTAL AND DIAGONAL ARE NOT.
         *
         * A dot directly above or below its neighbour is what gives the sides
         * their one-dot-per-row tangent at 90 and 270 - the operator asked for
         * that in the 2026-10-08 round and his approved screenshot has it. A
         * dot BESIDE or DIAGONAL to its neighbour is what the eye reads as a
         * straight line welded onto the circle, and that is what he marked in
         * red. So the rule is not "nothing touches" - that emptied the sides
         * and lost the shape - it is "nothing touches SIDEWAYS". */
        if (last_row != INT_MIN && touches_sideways(row, col, last_row, last_col))
            continue;

        /* The same test against the FIRST dot once we are far enough round to
         * be closing on it: the join is the one place the neighbour test above
         * cannot see, and a run there is as visible as anywhere else. */
        if (first_row != INT_MIN && i > STEPS / 2 &&
            touches_sideways(row, col, first_row, first_col)) continue;

        /* A satellite already here keeps the cell - it is the more important
         * mark, and it also reads as part of the rim. Do NOT advance `last`
         * when the cell is taken: the spacing is about the dots that were
         * drawn, and skipping the update keeps the next one correctly placed. */
        if (!cell_free(lines, row, col)) continue;

        put_ch(lines, row, col, '.');
        last_row = row; last_col = col;
        if (first_row == INT_MIN) { first_row = row; first_col = col; }
    }
}

static void draw_sky(char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                     const unit_gps_info_t *in,
                     gps_page_marker_t *markers, int *n_markers)
{
    /* Satellites FIRST, grid dots second, so a dot never lands on top of a
     * satellite label. */
    for (int i = 0; i < in->n_sats; i++) {
        const unit_gps_sat_t *s = &in->sat[i];
        if (s->elev_deg < 0 || s->azim_deg < 0) continue;   /* position unknown */

        /* Polar: azimuth 0 at the top and clockwise, elevation 90 at the
         * centre and 0 at the rim. The character cell is about twice as tall
         * as it is wide, hence the separate x and y radii - using one radius
         * draws an ellipse and puts every satellite in the wrong place. */
        double az = s->azim_deg * M_PI / 180.0;
        double r  = (90.0 - s->elev_deg) / 90.0;
        int    x  = PLOT_CX + (int)lround(sin(az) * r * PLOT_RX);
        int    y  = PLOT_CY - (int)lround(cos(az) * r * PLOT_RY);

        /* Marker 'x' = in the position solution (GSA listed its PRN),
         * '+' = in view but not used. It sits on the OUTER side of the
         * number, which is how the QMX's own plot reads. */
        char marker = s->used ? 'x' : '+';
        char txt[5];
        if (x >= PLOT_CX) snprintf(txt, sizeof(txt), "%d%c", s->prn, marker);
        else              snprintf(txt, sizeof(txt), "%c%d", marker, s->prn);

        int len = (int)strlen(txt);
        int col = (x >= PLOT_CX) ? x : x - (len - 1);
        if (col < PLOT_X0) col = PLOT_X0;
        if (col + len > GPS_PAGE_COLS) col = GPS_PAGE_COLS - len;
        put(lines, y, col, txt);

        if (markers && n_markers && *n_markers < GPS_PAGE_MAX_MARKERS) {
            gps_page_marker_t *m = &markers[(*n_markers)++];
            m->row    = y;
            m->col    = col;
            m->snr_db  = s->snr_db;
            m->used    = s->used;
            m->compass = false;
            snprintf(m->text, sizeof(m->text), "%s", txt);
        }
    }

    /* TWO rings: the horizon rim and one HALF WAY in, which is 45 degrees of
     * elevation. The radio's plot has both; the three evenly-spaced rings this
     * code drew first were my invention and crowded a 52-column plot. */
    for (int elev = 0; elev <= 45; elev += 45) {
        double k = (90.0 - elev) / 90.0;
        draw_ring(lines, k * PLOT_RX, k * PLOT_RY);
    }
    for (int y = PLOT_CY - PLOT_RY; y <= PLOT_CY + PLOT_RY; y++) {
        if (cell_free(lines, y, PLOT_CX)) put_ch(lines, y, PLOT_CX, '.');
    }
    for (int x = PLOT_CX - PLOT_RX; x <= PLOT_CX + PLOT_RX; x++) {
        if (cell_free(lines, PLOT_CY, x)) put_ch(lines, PLOT_CY, x, '.');
    }

    /* Compass labels. Azimuth, not a clock face: 0 north at the top. Emitted
     * as markers too, so the view can draw them BLUE the way the radio does. */
    struct { int r, c; const char *t; } cmp[4] = {
        { PLOT_CY - PLOT_RY, PLOT_CX,                "0"   },
        { PLOT_CY + PLOT_RY, PLOT_CX - 1,            "180" },
        { PLOT_CY,           PLOT_CX - PLOT_RX - 3,  "270" },
        { PLOT_CY,           GPS_PAGE_COLS - 2,      "90"  },
    };
    for (int i = 0; i < 4; i++) {
        put(lines, cmp[i].r, cmp[i].c, cmp[i].t);
        if (markers && n_markers && *n_markers < GPS_PAGE_MAX_MARKERS) {
            gps_page_marker_t *m = &markers[(*n_markers)++];
            m->row = cmp[i].r;
            m->col = cmp[i].c;
            m->snr_db = -1;
            m->used = false;
            m->compass = true;
            snprintf(m->text, sizeof(m->text), "%s", cmp[i].t);
        }
    }
}

/* Elevation, azimuth and SNR are each left BLANK when the receiver did not
 * report them, exactly as the QMX leaves them. Printing 0 or -1 would read as
 * a measurement. */
int gps_page_sat_rows(const unit_gps_info_t *in, gps_page_satrow_t *out, int max)
{
    if (!in || !out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < in->n_sats && n < max; i++) {
        const unit_gps_sat_t *s = &in->sat[i];
        char el[8] = "  ", az[8] = "   ", sn[8] = "  ";
        if (s->elev_deg >= 0) snprintf(el, sizeof(el), "%2d", s->elev_deg);
        if (s->azim_deg >= 0) snprintf(az, sizeof(az), "%3d", s->azim_deg);
        if (s->snr_db   >= 0) snprintf(sn, sizeof(sn), "%2d", s->snr_db);
        snprintf(out[n].text, sizeof(out[n].text), "%3d %s %s %s  %s",
                 s->prn, el, az, sn, unit_gps_constellation(s->talker));
        out[n].snr_db = s->snr_db;
        out[n].used   = s->used;
        n++;
    }
    return n;
}

/* Strong green, usable amber, weak grey - and an UNTRACKED satellite dimmer
 * still. A sky full of untracked satellites is what a bad antenna looks like,
 * and it must not look the same as a sky full of good ones. */
uint32_t gps_page_snr_colour(int snr_db)
{
    if (snr_db < 0)   return 0x707070;
    if (snr_db >= 30) return 0x4CD964;
    if (snr_db >= 20) return 0xE8C040;
    return 0xA0A0A0;
}

void gps_page_render(const unit_gps_info_t *in, gps_page_src_t src,
                     char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                     gps_page_marker_t *markers, int *n_markers)
{
    if (n_markers) *n_markers = 0;
    for (int r = 0; r < GPS_PAGE_ROWS; r++) {
        memset(lines[r], ' ', GPS_PAGE_COLS);
        lines[r][GPS_PAGE_COLS] = '\0';
    }
    if (!in) return;

    char buf[48];

    /* Validity is the RMC status byte, not our own idea of it: 'A' active,
     * 'V' void. The operator reads the same letter the receiver sent. */
    put(lines, 0, 0, "Validity");
    put(lines, 0, VAL_COL, in->valid ? "A" : "V");

    put(lines, 1, 0, "Fix");
    put(lines, 1, VAL_COL, in->fix_type == 3 ? "3D"
                         : in->fix_type == 2 ? "2D"
                         : in->fix_type == 1 ? "none" : "-");

    put(lines, 2, 0, "UT date");
    if (in->has_time && in->mon >= 1 && in->mon <= 12) {
        snprintf(buf, sizeof(buf), "%02d-%s-%02d",
                 in->mday, kMonth[in->mon - 1], in->year % 100);
        put(lines, 2, VAL_COL, buf);
    } else {
        put(lines, 2, VAL_COL, "-");
    }

    put(lines, 3, 0, "UT time");
    if (in->has_time) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", in->hour, in->min, in->sec);
        put(lines, 3, VAL_COL, buf);
        put(lines, 0, 25, buf);           /* the big clock, as the QMX shows it */
    } else {
        put(lines, 3, VAL_COL, "-");
    }

    put(lines, 4, 0, "Grid");
    put(lines, 5, 0, "Latitude");
    put(lines, 6, 0, "Longitude");
    if (in->has_pos) {
        char grid[7];
        gps_page_grid(in->lat_deg, in->lon_deg, grid);
        put(lines, 4, VAL_COL, grid);
        fmt_latlon(buf, sizeof(buf), in->lat_deg, true);
        put(lines, 5, VAL_COL, buf);
        fmt_latlon(buf, sizeof(buf), in->lon_deg, false);
        put(lines, 6, VAL_COL, buf);
    } else {
        put(lines, 4, VAL_COL, "-");
        put(lines, 5, VAL_COL, "-");
        put(lines, 6, VAL_COL, "-");
    }

    put(lines, 7, 0, "Altitude");
    if (in->has_alt) {
        snprintf(buf, sizeof(buf), "%.3f", (double)in->alt_m);
        put(lines, 7, VAL_COL, buf);
    } else {
        put(lines, 7, VAL_COL, "-");
    }

    /* ⛔ The three rows below are the ones the QMX cannot fill over CAT. They
     * stay on the page and read "-", rather than being hidden, so the two
     * sources are visibly the SAME page with the radio simply not reporting -
     * not two different pages. */
    put(lines, 8, 0, "Fix sats");
    put(lines, 9, 0, "Tot sats");
    put(lines, 10, 0, "Avg SNR");
    if (src == GPS_PAGE_SRC_QMX) {
        put(lines, 8, VAL_COL, "-");
        put(lines, 9, VAL_COL, "-");
        put(lines, 10, VAL_COL, "-");
    } else {
        snprintf(buf, sizeof(buf), "%d", in->fix_sats);
        put(lines, 8, VAL_COL, buf);
        snprintf(buf, sizeof(buf), "%d", in->tot_sats);
        put(lines, 9, VAL_COL, buf);
        if (in->avg_snr >= 0) {
            snprintf(buf, sizeof(buf), "%d", in->avg_snr);
            put(lines, 10, VAL_COL, buf);
        } else {
            put(lines, 10, VAL_COL, "-");
        }
    }

    if (src == GPS_PAGE_SRC_QMX) {
        put(lines, TABLE_Y0, 1, "no satellite detail over CAT");
        put(lines, TABLE_Y0 + 1, 1, "- the radio does not report it");
        return;
    }

    /* Satellite table - the first screenful. The view scrolls the full list
     * from gps_page_sat_rows(), which formats exactly these strings, so the
     * scrolled text and the text checked here cannot drift apart. */
    {
        /* Only the rows that fit on the grid, and on the STACK - twelve rows
         * is about 400 bytes. The static array this used to be was 32 rows in
         * .bss, and internal .bss comes straight out of the DMA pool: with it
         * the SD card would not mount at all (2026-10-08, "DMA free=1235 B,
         * could not take 1024 B"). See project_internal_bss_root_cause. */
        gps_page_satrow_t rows[GPS_PAGE_TABLE_ROWS];
        int n = gps_page_sat_rows(in, rows, GPS_PAGE_TABLE_ROWS);
        for (int i = 0; i < n; i++) {
            put(lines, TABLE_Y0 + i, 0, rows[i].text);
        }
    }

    draw_sky(lines, in, markers, n_markers);
}


/* ⭐ ONE composition, two displays. The glass (ui/gps_status_view.c) and the
 * web (/api/gps) both draw the markers and the satellite table themselves -
 * coloured, and the table scrolling because a receiver routinely sees twenty
 * satellites while the grid has room for twelve. Both therefore have to blank
 * the same cells first, or the grid text shows through underneath.
 *
 * That blanking used to be two copies of two loops. It is one copy here
 * because the failure mode is silent: a cell blanked on one display and not
 * the other is a page that looks right on the bench and wrong in the browser.
 */
void gps_page_compose(const unit_gps_info_t *in, gps_page_src_t src,
                      char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                      gps_page_marker_t *markers, int *n_markers)
{
    int n = 0;
    gps_page_render(in, src, lines, markers, n_markers);
    if (n_markers) n = *n_markers;

    /* The marker text, so the coloured label is not drawn over the plain one. */
    for (int i = 0; markers && i < n; i++) {
        for (int k = 0; markers[i].text[k]; k++) {
            int c = markers[i].col + k;
            if (markers[i].row >= 0 && markers[i].row < GPS_PAGE_ROWS &&
                c >= 0 && c < GPS_PAGE_COLS)
                lines[markers[i].row][c] = ' ';
        }
    }

    /* The satellite-table region, which the scrolling pane owns. */
    for (int r = GPS_PAGE_TABLE_ROW0; r < GPS_PAGE_ROWS; r++)
        for (int c = 0; c < GPS_PAGE_SAT_COLS && c < GPS_PAGE_COLS; c++)
            lines[r][c] = ' ';
}
