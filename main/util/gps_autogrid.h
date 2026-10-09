#pragma once

/* Should the station's Maidenhead grid be replaced by the one the GPS says?
 *
 * ⛔ THIS IS NOT PLUMBING, WHICH IS WHY IT IS ITS OWN PURE FUNCTION. `my_grid`
 * goes out on the air in every FT8 CQ and TX1, into every ADIF record and into
 * LoTW. A grid that flickers between two squares writes NVS every second and
 * puts a different locator in consecutive transmissions.
 *
 * So the decision has two guards, and test/gps_autogrid_harness.c holds them:
 *
 *   1. The GPS grid must AGREE WITH ITSELF for GPS_AUTOGRID_STABLE_S seconds
 *      before it is believed. A fresh fix wanders, and the sixth character is
 *      about 4 km across.
 *   2. It must actually DIFFER from what is stored. Re-writing the same string
 *      would wear the flash for nothing.
 *
 * ⚠ IT DOES OVERWRITE A HAND-ENTERED GRID. "Set the grid automatically when we
 * have the grid from GPS" (operator, 2026-10-09) means exactly that, and the
 * case it is for - operating portable - is the case where the stored grid is
 * the one that is wrong. The change is logged where it happens.
 */

#include <stdbool.h>

/* Seconds of agreement before a new grid is accepted. A minute: long enough
 * that a settling fix has stopped moving, short enough that arriving at a new
 * site is picked up before the first CQ. */
#define GPS_AUTOGRID_STABLE_S  60

typedef struct {
    char last[8];     /* the grid last seen from the GPS */
    int  stable_s;    /* seconds it has read the same */
} gps_autogrid_t;

void gps_autogrid_init(gps_autogrid_t *st);

/* Call once a second with the grid computed from the current fix, or NULL
 * when there is no usable fix, and the grid currently stored.
 *
 * Returns true when `gps_grid` should be written to settings. */
bool gps_autogrid_step(gps_autogrid_t *st, const char *gps_grid,
                       const char *stored_grid);
