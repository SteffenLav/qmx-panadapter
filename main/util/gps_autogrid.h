#pragma once

/* Should the station's Maidenhead grid be replaced by the one the GPS says?
 *
 * ⛔ THIS IS NOT PLUMBING, WHICH IS WHY IT IS ITS OWN PURE FUNCTION. `my_grid`
 * goes out on the air in every FT8 CQ and TX1, into every ADIF record and into
 * LoTW. A grid that flickers between two squares writes NVS every second and
 * puts a different locator in consecutive transmissions.
 *
 * So the decision has three guards, and test/gps_autogrid_harness.c holds them:
 *
 *   1. The GPS grid must AGREE WITH ITSELF for GPS_AUTOGRID_STABLE_S seconds
 *      before it is believed. A fresh fix wanders, and the sixth character is
 *      about 4 km across.
 *   2. It must actually DIFFER from what is stored. Re-writing the same string
 *      would wear the flash for nothing.
 *   3. ⭐ IT MUST NOT UNDO THE OPERATOR. "I want it filled in as soon as GPS
 *      data is available - operator can then alter it if needed" (operator,
 *      2026-10-09). So the state remembers the grid it last wrote. If the
 *      stored grid no longer matches that, the operator typed over it, and
 *      this square is never written again. A grid the operator corrected by
 *      hand used to be overwritten 60 s later, which made "can then alter it"
 *      mean nothing.
 *
 * ⚠ Guard 3 stands down for ONE square, not forever: when the GPS reaches a
 * DIFFERENT square the write happens again. That is the portable case this
 * feature exists for, and there the stored grid is the one that is wrong.
 *
 * ⛔ `written` MUST BE PERSISTED by the caller, or guard 3 fails on every
 * reboot - the operator's correction would be undone a minute after each boot.
 * status.c seeds it from settings_get_gps_grid_auto() and writes it back with
 * settings_set_gps_grid_auto() when this fires.
 */

#include <stdbool.h>
#include <stddef.h>

/* Seconds of agreement before a new grid is accepted. A minute: long enough
 * that a settling fix has stopped moving, short enough that arriving at a new
 * site is picked up before the first CQ. */
#define GPS_AUTOGRID_STABLE_S  60

typedef struct {
    char last[8];     /* the grid last seen from the GPS */
    char written[8];  /* the grid this module last told the caller to write */
    int  stable_s;    /* seconds it has read the same */
} gps_autogrid_t;

void gps_autogrid_init(gps_autogrid_t *st);

/* Seed `written` from persistent storage at startup. Pass NULL or "" when
 * nothing was ever written automatically. */
void gps_autogrid_set_written(gps_autogrid_t *st, const char *grid);

/* The grid this module last told the caller to write, "" if none. The caller
 * persists this after a fire. */
const char *gps_autogrid_written(const gps_autogrid_t *st);

/* Call once a second with the grid computed from the current fix, or NULL
 * when there is no usable fix, and the grid currently stored.
 *
 * Returns true when `gps_grid` should be written to settings. */
bool gps_autogrid_step(gps_autogrid_t *st, const char *gps_grid,
                       const char *stored_grid);
