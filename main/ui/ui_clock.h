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

// WiFi signal-strength "fan" icon: the dot plus two bows, drawn as three
// separate objects so the count of lit elements can carry the link quality —
// the way every phone/laptop status bar does it. Replaces the numeric
// "-NN dBm" readout that used to sit in the bottom bar (a number very few
// people read, occupying space an SSID needs far more).
//
// LV_SYMBOL_WIFI can't do this: it's one glyph, always the full fan.
//
// All three elements are always drawn — unlit ones at a low opacity — so the
// icon never changes width and the lit count is read against a visible whole.
// Width of the drawn ink (the widest bow plus its stroke), which is what
// layout gaps should be measured against - the arc OBJECTS are a couple of px
// wider and extend below the dot, but that area is empty.
#define UI_WIFI_FAN_W  32
typedef struct {
    lv_obj_t *dot;
    lv_obj_t *bow[2];       // bow[0] = inner, bow[1] = outer
    lv_coord_t cy;          // dot centre y, kept for ui_wifi_fan_set_x()
} ui_wifi_fan_t;

// (cx, cy_dot) is the centre of the dot, i.e. the fan's apex — the bows are
// concentric around it and extend upward only, so cy_dot should sit near the
// text baseline and there must be ~UI_WIFI_FAN_W/2 of room above it.
void ui_wifi_fan_init(ui_wifi_fan_t *f, lv_obj_t *parent,
                      lv_coord_t cx, lv_coord_t cy_dot, lv_color_t color);

// Move the whole fan horizontally (dot centre x). For a bar where the icon
// tracks a variable-length text next to it.
void ui_wifi_fan_set_x(ui_wifi_fan_t *f, lv_coord_t cx);

// level: 0 = dot+bows all dim (connected but very weak, or disconnected),
// 1 = dot, 2 = dot + inner bow, 3 = dot + both bows.
void ui_wifi_fan_set_level(ui_wifi_fan_t *f, int level);

// Map RSSI in dBm to a 0..3 fan level using the usual linear quality
// percentage (0% at -100 dBm, 100% at -50 dBm) against the operator's
// thresholds: >25% dot, >50% + inner bow, >80% + outer bow.
int ui_wifi_fan_level_for_dbm(int dbm);

#ifdef __cplusplus
}
#endif
