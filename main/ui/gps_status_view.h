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

void gps_status_view_open(void);
void gps_status_view_close(void);
bool gps_status_view_is_open(void);

/* Is there anything to show? True when the clock is disciplined by a GNSS
 * receiver - the Tab5's own or the QMX's. False on NTP/RTC/FT8/manual, where
 * the tap does nothing: a page of dashes is worse than no page. */
bool gps_status_view_available(void);
