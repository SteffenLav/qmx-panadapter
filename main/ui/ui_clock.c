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

void ui_rssi_init(ui_rssi_t *r, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                   const lv_font_t *font, lv_color_t color, lv_coord_t cell_w)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, (i == 0) ? "-" : "0");
        lv_obj_set_style_text_color(l, color, 0);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_pos(l, x + i * cell_w, y);
        lv_obj_set_width(l, cell_w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
        r->cells[i] = l;
    }
}

void ui_rssi_set(ui_rssi_t *r, int dbm)
{
    if (dbm > 0) dbm = 0;
    if (dbm < -99) dbm = -99;
    int mag = -dbm;
    set_digit(r->cells[1], (mag / 10) % 10);
    set_digit(r->cells[2], mag % 10);
}
