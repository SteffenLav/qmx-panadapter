#pragma once
/* RTTY page - R006 (TODO-RTTY.md, docs/rtty-feasibility.md "R006 design").
 *
 * Same grid as the CW page: the panadapter spectrum and waterfall stay live
 * underneath, a 320 px pane on the left (CW_PANE_W - the spectrum mapping in
 * ui.c already narrows to it for any page with a left pane), and a 6-row
 * decoded-text area over the bottom of the waterfall, above the band-plan
 * strip. The pane carries the TX buttons (R010).
 *
 * Showing the page turns the decoder (rtty_rx) on; hiding it turns it off. */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

void      rtty_screen_view_init(lv_obj_t *parent);
void      rtty_screen_view_show(void);
void      rtty_screen_view_hide(void);
lv_obj_t *rtty_screen_view_get_container(void);

/* The waterfall area left visible by the pane and the text box (2 rects), for
 * ui.c's carpet shift; false while the page is not shown. See the CW page's
 * cw_screen_view_carpet_visible() for why. */
bool      rtty_screen_view_carpet_visible(lv_area_t a[2]);

/* A tap (Tab5) or click (web) on the spectrum/waterfall while this page is
 * up: the DIAL moves so the tapped frequency sits at RTTY_TAP_AUDIO_HZ, and
 * the decoder's tone centre goes on it. Any task (the web calls it). */
#define RTTY_TAP_AUDIO_HZ 1500
void      rtty_screen_view_tap_hz(uint32_t tapped_hz);
