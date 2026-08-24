#pragma once
#include "lvgl.h"

/* The WSPR page: a sibling screen to Panadapter and FT8, shown while
 * ui_mode == UI_MODE_WSPR. Design and its reasoning are in
 * docs/wspr-ui-design.md - in particular why this looks different from the FT8
 * screen rather than the same, which is the protocol's doing and not a
 * preference:
 *
 *   - the rhythm is TWO MINUTES, so the countdown orients rather than urges
 *   - it is a one-way beacon, so there is no QSO furniture at all
 *   - the list is a LOG grouped by cycle, not a live "who is on frequency now"
 *   - there is no MESSAGE column, because there is no message
 */

void      wspr_screen_view_init(lv_obj_t *parent);
void      wspr_screen_view_show(void);
void      wspr_screen_view_hide(void);
lv_obj_t *wspr_screen_view_get_container(void);

// Called from the 1 Hz UI tick while the page is up: refreshes the countdown,
// the status line and (only when it has changed) the spot list.
void      wspr_screen_view_tick(void);
