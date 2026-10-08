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

/* Render into `lines`, each NUL-terminated and exactly GPS_PAGE_COLS wide
 * (trailing spaces included, as the grid has them). */
void gps_page_render(const unit_gps_info_t *in, gps_page_src_t src,
                     char lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1]);

/* "GPS", "GLONASS", "Galileo", "Beidou", "QZSS" - the QMX viewer spells the
 * constellation out in its last column. Lives here, not in the driver: it is
 * presentation, and the page harness links it on a host with no FreeRTOS. */
const char *unit_gps_constellation(const char talker[3]);

/* Maidenhead locator from signed degrees, 6 characters plus NUL. Exposed for
 * the harness: a grid square that is one square out looks entirely plausible
 * and is only caught by a known reference. */
void gps_page_grid(double lat_deg, double lon_deg, char out[7]);
