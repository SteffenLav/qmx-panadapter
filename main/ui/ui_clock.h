#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Jitter-free "HH:MM:SS" clock widget.
//
// A plain lv_label with lv_label_set_text("HH:MM:SS") visibly bounces:
// proportional fonts (e.g. Montserrat) give digits slightly different
// advance widths, so as the digits cycle the whole string (and anything
// after it, plus the centered/left-anchored block itself) shifts by a
// pixel or two every second.
//
// Fix: render each of the 8 "HH:MM:SS" characters in its own fixed-width
// cell, left-aligned within the cell. The glyph's left edge — and
// therefore every cell boundary — never moves, regardless of which
// digit is showing.
typedef struct {
    lv_obj_t *cells[8];  // H H : M M : S S
} ui_clock_t;

// Create the 8 fixed-width cells starting at (x, y) within parent.
// cell_w should be >= the widest digit/colon glyph advance for `font`
// (a few px of slack on the right of each cell is fine and invisible).
void ui_clock_init(ui_clock_t *clk, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                    const lv_font_t *font, lv_color_t color, lv_coord_t cell_w);

// Update the displayed time. h/m/s in 0..99 (only 0..23/0..59 expected,
// but not range-checked here).
void ui_clock_set_time(ui_clock_t *clk, int h, int m, int s);

// Same jitter problem applies to any label with a changing number embedded
// in otherwise-static text (e.g. "WiFi  MySSID  -67dBm  192.168.1.5" — the
// RSSI digits' varying glyph widths shift the WiFi icon/SSID left of it).
//
// Fixed-width "-NN" RSSI widget: sign cell + 2 digit cells, left-aligned
// per cell, so nothing to its left ever moves.
typedef struct {
    lv_obj_t *cells[3];  // - N N
} ui_rssi_t;

void ui_rssi_init(ui_rssi_t *r, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                   const lv_font_t *font, lv_color_t color, lv_coord_t cell_w);

// dbm expected in -99..0 (clamped). Renders as "-NN".
void ui_rssi_set(ui_rssi_t *r, int dbm);

#ifdef __cplusplus
}
#endif
