#pragma once

/* Full-screen GPS status page, opened by tapping the UTC field in the bottom
 * bar. Deliberately the same shape as qmx_term_view's radio-menu overlay -
 * same 80x24 grid, same qmx_mono_25 font, same header - because the page it
 * shows is a copy of the radio's own "Hardware tests | GPS viewer" and the
 * operator asked for the two to look alike.
 *
 * ⛔ The TITLE is what tells them apart, and it is never just "GPS". Two
 * receivers, two antennas, two failure modes, and only one of them survives
 * the radio being unplugged.
 */

#include <stdbool.h>

#include "gps_page.h"

void gps_status_view_open(void);
void gps_status_view_close(void);
bool gps_status_view_is_open(void);

/* Which receiver is disciplining the clock, or neither.
 *
 * ⛔ NTP, RTC, FT8 and manual all return false. The tap then does nothing, and
 * /api/gps says so: a page of dashes is worse than no page, because it reads
 * as a receiver that has failed rather than as no receiver at all.
 *
 * Exposed so the WEB page decides the same way the glass does - it reads
 * time_sync only, no LVGL and no display lock, so the HTTPD task may call it.
 * Duplicating this rule in webserver.c is how the two would come to disagree. */
bool gps_status_source(gps_page_src_t *out);

/* Is there anything to show? gps_status_source() without the answer. */
bool gps_status_view_available(void);
