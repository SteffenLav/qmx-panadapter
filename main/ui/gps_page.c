#include "gps_page.h"

#include <math.h>
#include <stdio.h>
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

const char *gps_page_title(gps_page_src_t src)
{
    /* Never just "GPS". Two receivers, two antennas, two failure modes, and
     * only one of them survives the radio being unplugged. */
    return (src == GPS_PAGE_SRC_QMX) ? "GPS  -  QMX internal receiver"
                                     : "GPS  -  Module v2.1 on the M-Bus";
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

static void draw_sky(char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                     const unit_gps_info_t *in)
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
    }

    /* Dotted rings at 0, 30 and 60 degrees elevation, then the two axes. The
     * step is in whole degrees of azimuth: fine enough that the rim reads as a
     * circle, coarse enough that it stays dotted rather than solid. */
    for (int elev = 0; elev <= 60; elev += 30) {
        double r = (90.0 - elev) / 90.0;
        for (int a = 0; a < 360; a += 5) {
            double rad = a * M_PI / 180.0;
            int x = PLOT_CX + (int)lround(sin(rad) * r * PLOT_RX);
            int y = PLOT_CY - (int)lround(cos(rad) * r * PLOT_RY);
            if (cell_free(lines, y, x)) put_ch(lines, y, x, '.');
        }
    }
    for (int y = PLOT_CY - PLOT_RY; y <= PLOT_CY + PLOT_RY; y++) {
        if (cell_free(lines, y, PLOT_CX)) put_ch(lines, y, PLOT_CX, '.');
    }
    for (int x = PLOT_CX - PLOT_RX; x <= PLOT_CX + PLOT_RX; x++) {
        if (cell_free(lines, PLOT_CY, x)) put_ch(lines, PLOT_CY, x, '.');
    }

    /* Compass labels. Azimuth, not a clock face: 0 north at the top. */
    put(lines, PLOT_CY - PLOT_RY, PLOT_CX, "0");
    put(lines, PLOT_CY + PLOT_RY, PLOT_CX - 1, "180");
    put(lines, PLOT_CY, PLOT_CX - PLOT_RX - 3, "270");
    put(lines, PLOT_CY, GPS_PAGE_COLS - 2, "90");
}

void gps_page_render(const unit_gps_info_t *in, gps_page_src_t src,
                     char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1])
{
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

    /* Satellite table. Elevation, azimuth and SNR are each blank when the
     * receiver did not report them, exactly as the QMX leaves them blank. */
    for (int i = 0; i < in->n_sats && TABLE_Y0 + i < GPS_PAGE_ROWS; i++) {
        const unit_gps_sat_t *s = &in->sat[i];
        char row[48];
        char el[8] = "  ", az[8] = "   ", sn[8] = "  ";
        if (s->elev_deg >= 0) snprintf(el, sizeof(el), "%2d", s->elev_deg);
        if (s->azim_deg >= 0) snprintf(az, sizeof(az), "%3d", s->azim_deg);
        if (s->snr_db   >= 0) snprintf(sn, sizeof(sn), "%2d", s->snr_db);
        snprintf(row, sizeof(row), "%3d %s %s %s  %s",
                 s->prn, el, az, sn, unit_gps_constellation(s->talker));
        put(lines, TABLE_Y0 + i, 0, row);
    }

    draw_sky(lines, in);
}
