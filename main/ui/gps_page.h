#pragma once

/* The GPS page, composed as TEXT.
 *
 * The QMX draws its own "Hardware tests | GPS viewer" into an 80x24 character
 * grid, and the Tab5 already renders that grid with qmx_mono_25. Composing
 * our own receiver's page the same way is what makes the two look like the
 * same page rather than two designs of the same information - which is what
 * the operator asked for (2026-10-08).
 *
 * It is a PURE function of a snapshot: no LVGL, no driver, no clock. That is
 * deliberate - the layout is the part that can be wrong without the hardware
 * noticing (a column off by one, a latitude printed as a decimal degree, a
 * satellite drawn in the wrong quadrant), and test/gps_page_harness.c pins it
 * down on a PC instead of on the glass.
 */

#include <stdint.h>

#include "unit_gps/unit_gps.h"

#define GPS_PAGE_COLS 80
#define GPS_PAGE_ROWS 24

/* Which receiver the page is showing. The two sources carry different amounts
 * of data and the page must never let them be confused: the QMX's CAT link
 * gives position and time only, so its satellite table and sky plot would be
 * empty. */
typedef enum {
    GPS_PAGE_SRC_MODULE = 0,  /* Module GPS v2.1 / Unit GPS on the Tab5 */
    GPS_PAGE_SRC_QMX,         /* the radio's own receiver, over CAT */
} gps_page_src_t;

/* One line of the title, naming the source. Never "GPS" alone.
 *
 * `rx_gpio` is the pin the receiver is bound to (unit_gps_rx_gpio()), and it
 * names the PORT - PORT.A or the M-Bus. Ignored for GPS_PAGE_SRC_QMX.
 *
 * ⛔ It does NOT name a hardware version, deliberately. This used to read
 * "Module v2.1 on the M-Bus" for every Tab5-side receiver, including a Unit
 * GPS v1.1 on PORT.A, and nothing checked. The port is known; the version is
 * not, and the baud rate does not reveal it. */
const char *gps_page_title(gps_page_src_t src, int rx_gpio);

#define GPS_PAGE_MAX_MARKERS UNIT_GPS_MAX_SATS

/* Where a satellite was drawn in the sky plot, so the view can colour it.
 *
 * The marker is ALSO written into `lines`, not instead of it. The view blanks
 * those cells and redraws them coloured - the same thing qmx_term_view does
 * with the radio's colour runs. Reporting the position instead of drawing it
 * would mean the harness tested a placement the device never uses. */
typedef struct {
    int  row, col;
    char text[6];   /* "13x" or "+28" */
    int  snr_db;    /* -1 = in view, not tracked */
    bool used;      /* in the position solution */
    bool compass;   /* a 0/90/180/270 label, not a satellite - drawn blue */
} gps_page_marker_t;

/* Render into `lines`, each NUL-terminated and exactly GPS_PAGE_COLS wide
 * (trailing spaces included, as the grid has them). `markers`/`n_markers` may
 * be NULL when the caller does not colour them. */
void gps_page_render(const unit_gps_info_t *in, gps_page_src_t src,
                     char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                     gps_page_marker_t *markers, int *n_markers);

/* ⭐ THE ELEVATION RINGS ARE NOT CHARACTERS ANY MORE - THE VIEW DRAWS THEM.
 *
 * They were dots in the grid through three rounds of complaints and the
 * character cell lost every one of them. A 15x27 px cell cannot hold a smooth
 * ellipse: drawn continuously it grows horizontal and diagonal RUNS (the eye
 * reads line segments welded to a circle), and spaced out to break those runs
 * it becomes evenly spaced ". . ." - MEASURED 2026-10-09 as 11 dotted
 * horizontal runs of three or more. Both were rejected on the glass. The grid
 * is the wrong instrument for a curve.
 *
 * So this reports the GEOMETRY and the view draws a real thin faint-grey oval
 * with it - LVGL on the Tab5, SVG in the browser. Reported in CELL units
 * because that is the only coordinate system both views share; each converts
 * with its own cell size.
 *
 * ⛔ The two AXES are still characters, deliberately. They are straight lines
 * and the grid draws those perfectly, and they are in the screenshot the
 * operator approved.
 */
typedef struct {
    double cx, cy;    /* centre, in cells (cell CENTRES, so 0.5 offsets) */
    double rx, ry;    /* radii, in cells */
    int    elev_deg;  /* the elevation this ring marks: 0 = horizon */
} gps_page_ring_t;

#define GPS_PAGE_MAX_RINGS 4

/* Fill `out` with the rings the plot wants drawn. Returns how many. Pure: it
 * takes no snapshot, because the rings do not depend on the fix. */
int gps_page_rings(gps_page_ring_t *out, int max);

/* The N-S and E-W axes, same story and same units: the view draws them as
 * thin grey lines too (operator, 2026-10-09). They were characters while the
 * rings were, and a dotted cross beside two smooth ovals looked half-finished.
 *
 * ⛔ DRAWN FIRST, UNDER THE TEXT. A line through the middle of the plot
 * crosses satellites, and the satellite and its number are what the operator
 * reads - his words. Both views put these in the background. */
typedef struct {
    double x0, y0, x1, y1;   /* endpoints, in cells (cell CENTRES) */
} gps_page_axis_t;

#define GPS_PAGE_MAX_AXES 2

int gps_page_axes(gps_page_axis_t *out, int max);

/* gps_page_render(), then blank the cells the CALLER draws itself: the marker
 * text and the satellite-table region. Both displays need exactly this, so it
 * is one function rather than two copies of two loops - see the note at the
 * definition. Use this, not gps_page_render(), unless you really do want the
 * raw grid (the harness does). */
void gps_page_compose(const unit_gps_info_t *in, gps_page_src_t src,
                      char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1],
                      gps_page_marker_t *markers, int *n_markers);

/* The satellite table, as its own list.
 *
 * The page grid has room for twelve rows and a receiver routinely sees twenty,
 * so the view scrolls this list rather than truncating it. gps_page_render()
 * writes the first GPS_PAGE_TABLE_ROWS of exactly these strings into `lines`,
 * so what scrolls and what the harness checks are the same text. */
#define GPS_PAGE_TABLE_ROW0  12
#define GPS_PAGE_TABLE_ROWS  (GPS_PAGE_ROWS - GPS_PAGE_TABLE_ROW0)   /* 12 */
#define GPS_PAGE_SAT_COLS    26

typedef struct {
    char text[GPS_PAGE_SAT_COLS + 1];
    int  snr_db;   /* -1 = in view, not tracked */
    bool used;     /* in the position solution */
} gps_page_satrow_t;

/* Format every satellite. Returns how many rows were written. */
int gps_page_sat_rows(const unit_gps_info_t *in, gps_page_satrow_t *out, int max);

/* SNR colour, shared by the table and the sky plot so one satellite cannot be
 * green in one place and amber in the other. */
uint32_t gps_page_snr_colour(int snr_db);

/* "GPS", "GLONASS", "Galileo", "Beidou", "QZSS" - the QMX viewer spells the
 * constellation out in its last column. Lives here, not in the driver: it is
 * presentation, and the page harness links it on a host with no FreeRTOS. */
const char *unit_gps_constellation(const char talker[3]);

/* Maidenhead locator from signed degrees, 6 characters plus NUL. Exposed for
 * the harness: a grid square that is one square out looks entirely plausible
 * and is only caught by a known reference. */
void gps_page_grid(double lat_deg, double lon_deg, char out[7]);
