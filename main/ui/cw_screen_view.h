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

// Web-interface request to send CW text - a macro (with its {MYCALL}/{PARK}/
// {RST}/{SERIAL} placeholders still in it, expanded on the LVGL thread same as
// a Tab5 tap) or free text. Only records the request - same deferral pattern
// as ft8_screen_view_request_freetext(): this runs on the HTTP task, and
// send_cw_text() (expansion, the sim interlock, the CAT hand-off) belongs to
// the LVGL thread. Drained by the page's own 500 ms timer, so it only takes
// effect while the Tab5 is actually showing the CW page (T159, Ralph DL1HR).
void cw_screen_view_request_send(const char *raw_text);

// The last text actually sent (expanded, real or sim), for the web status
// poll to echo back - same string the Tab5's own "Keying:" line shows.
// Empty until the first send this session.
const char *cw_screen_view_last_sent(void);
