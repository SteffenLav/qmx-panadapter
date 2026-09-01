#pragma once

// CW page (Tier 1, v1.4 — docs/cw-page-design.md). Third carousel stop:
// unlike the FT8 page this one KEEPS the panadapter spectrum/waterfall live
// underneath (CW is a tuning mode) and overlays a left keyer pane + a decode
// ticker strip. TX goes through the QMX's own keyer (CAT KY), decode is the
// QMX's own CW decoder (CAT TB), speed is CAT KS — all plumbed in cat.c.

#include "lvgl.h"

// Width of the CW page's left keyer pane. Exported because ui.c has to know
// where the pane ENDS: the pane overlays the spectrum canvas rather than
// narrowing it, so the frequency-axis labels and the band-plan strip have to
// be kept clear of the covered pixels. One definition, so the page and the
// axis can never disagree about where the visible spectrum starts.
#define CW_PANE_W 320

void       cw_screen_view_init(lv_obj_t *parent);
void       cw_screen_view_show(void);
void       cw_screen_view_hide(void);
lv_obj_t  *cw_screen_view_get_container(void);

// Called by cat.c's TB; response parser with freshly decoded characters.
// Runs on the CDC-ACM RX callback context — must never touch LVGL, so it
// only appends to a spinlock-guarded ring; the page's own LVGL timer
// renders it. Safe to call while the page is hidden (chars still collect).
void cw_screen_feed_decoded(const char *s, int n);

// Clear the decode ticker, the copilot hint, the station card and the ESM
// ladder. Called on a simulation-mode toggle so a phantom QSO leaves nothing
// on screen. LVGL thread only.
void cw_screen_clear_decode(void);
