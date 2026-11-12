#pragma once
/* RTTY page - R006 (TODO-RTTY.md, docs/rtty-feasibility.md "R006 design").
 *
 * Same grid as the CW page: the panadapter spectrum and waterfall stay live
 * underneath, a 320 px pane on the left (CW_PANE_W - the spectrum mapping in
 * ui.c already narrows to it for any page with a left pane), and an 8-row
 * decoded-text area over the bottom of the waterfall, above the band-plan
 * strip. RX only: the pane keeps a reserved, empty slot for TX (R010).
 *
 * Showing the page turns the decoder (rtty_rx) on; hiding it turns it off. */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

void      rtty_screen_view_init(lv_obj_t *parent);
void      rtty_screen_view_show(void);
void      rtty_screen_view_hide(void);
lv_obj_t *rtty_screen_view_get_container(void);

/* A tap on the spectrum/waterfall while this page is up: put the decoder's
 * tone centre on the tapped frequency. The dial does not move. LVGL thread. */
void      rtty_screen_view_tap_hz(uint32_t tapped_hz, uint32_t dial_hz);
