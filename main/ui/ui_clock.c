#include "ui_clock.h"

#include <stdio.h>

void ui_clock_init(ui_clock_t *clk, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                    const lv_font_t *font, lv_color_t color, lv_coord_t cell_w)
{
    // The colon glyph is much narrower than a digit, so giving it a full
    // digit-width cell leaves a visible gap before the following digit.
    // Colon cells get a smaller width; digit cells keep cell_w (this is
    // what prevents jitter, since digits 0-9 vary slightly in advance width).
    const lv_coord_t colon_w = cell_w / 2;
    lv_coord_t pos = x;
    for (int i = 0; i < 8; i++) {
        bool is_colon = (i == 2 || i == 5);
        lv_coord_t w = is_colon ? colon_w : cell_w;
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, is_colon ? ":" : "0");
        lv_obj_set_style_text_color(l, color, 0);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_pos(l, pos, y);
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
        clk->cells[i] = l;
        pos += w;
    }
}

static void set_digit(lv_obj_t *cell, int digit)
{
    char buf[2] = { (char)('0' + digit), '\0' };
    lv_label_set_text(cell, buf);
}

void ui_clock_set_time(ui_clock_t *clk, int h, int m, int s)
{
    set_digit(clk->cells[0], (h / 10) % 10);
    set_digit(clk->cells[1], h % 10);
    set_digit(clk->cells[3], (m / 10) % 10);
    set_digit(clk->cells[4], m % 10);
    set_digit(clk->cells[6], (s / 10) % 10);
    set_digit(clk->cells[7], s % 10);
}

// --- WiFi signal-strength fan ------------------------------------------------

// Bow radii and the dot size, in px. The outer bow sets the footprint:
// UI_WIFI_FAN_W must stay >= 2 * BOW_R_OUTER.
#define DOT_D         7
#define BOW_R_INNER  11
#define BOW_R_OUTER  16
#define BOW_WIDTH     3
// The outer bow is lifted clear of the inner one rather than sitting a bare
// radius-step away: at this size the two arcs' ends nearly touched, which read
// as one thick bow instead of two countable elements.
#define BOW_OUTER_LIFT 2

// LVGL angles: 0 deg = 3 o'clock, increasing clockwise. A fan pointing
// straight up (12 o'clock = 270 deg) spanning 120 deg = 210..330.
#define BOW_START_DEG 210
#define BOW_END_DEG   330

#define FAN_OPA_LIT   LV_OPA_COVER
#define FAN_OPA_DIM   LV_OPA_30

static lv_obj_t *make_bow(lv_obj_t *parent, lv_coord_t cx, lv_coord_t cy,
                          lv_coord_t r, lv_color_t color)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, 2 * r, 2 * r);
    lv_obj_set_pos(a, cx - r, cy - r);
    lv_arc_set_bg_angles(a, BOW_START_DEG, BOW_END_DEG);
    // The widget's own box must contribute nothing - only the arc stroke is
    // wanted, and the theme's default arc styling includes a background.
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(a, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(a, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, color, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, BOW_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    // An arc is a slider by default: kill the indicator, the knob and the
    // input handling so it's a static decoration and can't eat a touch that
    // belongs to the bottom bar.
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    return a;
}

void ui_wifi_fan_init(ui_wifi_fan_t *f, lv_obj_t *parent,
                      lv_coord_t cx, lv_coord_t cy_dot, lv_color_t color)
{
    // Bows first, dot last, so the dot is drawn over the inner bow's ends.
    f->bow[1] = make_bow(parent, cx, cy_dot - BOW_OUTER_LIFT, BOW_R_OUTER, color);
    f->bow[0] = make_bow(parent, cx, cy_dot, BOW_R_INNER, color);

    f->dot = lv_obj_create(parent);
    lv_obj_set_size(f->dot, DOT_D, DOT_D);
    lv_obj_set_pos(f->dot, cx - DOT_D / 2, cy_dot - DOT_D / 2);
    lv_obj_set_style_radius(f->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(f->dot, color, 0);
    lv_obj_set_style_bg_opa(f->dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(f->dot, 0, 0);
    lv_obj_remove_flag(f->dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(f->dot, LV_OBJ_FLAG_SCROLLABLE);

    f->cy = cy_dot;
    ui_wifi_fan_set_level(f, 0);
}

void ui_wifi_fan_set_x(ui_wifi_fan_t *f, lv_coord_t cx)
{
    if (!f->dot) return;
    lv_obj_set_pos(f->bow[1], cx - BOW_R_OUTER, f->cy - BOW_R_OUTER - BOW_OUTER_LIFT);
    lv_obj_set_pos(f->bow[0], cx - BOW_R_INNER, f->cy - BOW_R_INNER);
    lv_obj_set_pos(f->dot,    cx - DOT_D / 2,   f->cy - DOT_D / 2);
}

void ui_wifi_fan_set_level(ui_wifi_fan_t *f, int level)
{
    if (!f->dot) return;
    lv_obj_set_style_bg_opa(f->dot, level >= 1 ? FAN_OPA_LIT : FAN_OPA_DIM, 0);
    for (int i = 0; i < 2; i++)
        lv_obj_set_style_arc_opa(f->bow[i],
            level >= i + 2 ? FAN_OPA_LIT : FAN_OPA_DIM, LV_PART_MAIN);
}

int ui_wifi_fan_level_for_dbm(int dbm)
{
    int pct = 2 * (dbm + 100);   // -100 dBm -> 0%, -50 dBm -> 100%
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (pct > 80) return 3;
    if (pct > 50) return 2;
    if (pct > 25) return 1;
    return 0;
}
