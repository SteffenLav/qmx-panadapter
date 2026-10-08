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

/* One line of the title, naming the source. Never "GPS" alone. */
const char *gps_page_title(gps_page_src_t src);

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
