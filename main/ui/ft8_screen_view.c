#include "ft8_screen_view.h"
#include "ft8_screen.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>

#include "esp_log.h"
#include "cat/cat.h"
#include "storage/settings.h"

static const char *TAG = "ft8_view";

// Geometry. Display is 1280x720 landscape (sw_rotate). Top bar 60,
// bottom bar 36; middle band = 624 px.
#define TOP_BAR_H        60
#define BOTTOM_BAR_H     36
#define MID_Y            TOP_BAR_H
#define MID_H            (720 - TOP_BAR_H - BOTTOM_BAR_H)
#define MID_W            1280
#define LEFT_W           320
#define RIGHT_W          (MID_W - LEFT_W)

static lv_obj_t *s_container   = NULL;
static lv_obj_t *s_left_pane   = NULL;
static lv_obj_t *s_right_pane  = NULL;

static lv_obj_t *s_lbl_mode     = NULL;
static lv_obj_t *s_lbl_freq     = NULL;
static lv_obj_t *s_lbl_utc      = NULL;
static lv_obj_t *s_lbl_count    = NULL;   // slot countdown
static lv_obj_t *s_lbl_heard    = NULL;   // unique callsigns
static lv_obj_t *s_lbl_me       = NULL;   // "ME: CALL GRID" (if set)

static lv_obj_t *s_list         = NULL;

static lv_timer_t *s_t_refresh  = NULL;
static lv_timer_t *s_t_clock    = NULL;

static volatile bool s_refresh_pending = false;

// ---------------- helpers ----------------

static int cmp_by_utc_desc(const void *a, const void *b)
{
    const ft8_call_t *ca = (const ft8_call_t *)a;
    const ft8_call_t *cb = (const ft8_call_t *)b;
    if (cb->last_utc < ca->last_utc) return -1;
    if (cb->last_utc > ca->last_utc) return  1;
    return 0;
}

// Column x-offsets within the row (right pane is 960 px wide).
#define COL_CALL_X      12
#define COL_TEXT_X      160
#define COL_SCORE_X     520
#define COL_FREQ_X      640
#define COL_HEARD_X     770
#define COL_RIGHT_EDGE  960
#define ROW_H           36

static void add_row_label(lv_obj_t *row, const char *txt, int x, int w,
                          lv_text_align_t align, lv_color_t color,
                          const lv_font_t *font)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_width(lbl, w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_obj_set_pos(lbl, x, 6);
}

static void rebuild_list(void)
{
    static ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    qsort(snap, n, sizeof(ft8_call_t), cmp_by_utc_desc);

    lv_obj_clean(s_list);

    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, RIGHT_W, ROW_H);
        lv_obj_set_pos(row, 0, i * ROW_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x303030), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char b_score[12], b_freq[12], b_heard[12];
        snprintf(b_score, sizeof(b_score), "%d",  (int)snap[i].last_score);
        snprintf(b_freq,  sizeof(b_freq),  "%+d", (int)snap[i].last_freq);
        snprintf(b_heard, sizeof(b_heard), "(%u)", (unsigned)snap[i].heard_count);

        const lv_color_t white  = lv_color_hex(0xFFFFFF);
        const lv_color_t amber  = lv_color_hex(0xFFD700);
        const lv_color_t dim    = lv_color_hex(0xC0C0C0);

        add_row_label(row, snap[i].call,        COL_CALL_X,
                      COL_TEXT_X - COL_CALL_X - 8,
                      LV_TEXT_ALIGN_LEFT,  amber, &lv_font_montserrat_24);
        add_row_label(row, snap[i].last_text,   COL_TEXT_X,
                      COL_SCORE_X - COL_TEXT_X - 8,
                      LV_TEXT_ALIGN_LEFT,  white, &lv_font_montserrat_24);
        add_row_label(row, b_score,             COL_SCORE_X,
                      COL_FREQ_X - COL_SCORE_X - 8,
                      LV_TEXT_ALIGN_RIGHT, dim,   &lv_font_montserrat_24);
        add_row_label(row, b_freq,              COL_FREQ_X,
                      COL_HEARD_X - COL_FREQ_X - 8,
                      LV_TEXT_ALIGN_RIGHT, dim,   &lv_font_montserrat_24);
        add_row_label(row, b_heard,             COL_HEARD_X,
                      COL_RIGHT_EDGE - COL_HEARD_X - 16,
                      LV_TEXT_ALIGN_RIGHT, dim,   &lv_font_montserrat_24);
    }

    if (s_lbl_heard) {
        char b[32];
        snprintf(b, sizeof(b), "Heard: %d", n);
        lv_label_set_text(s_lbl_heard, b);
    }
}

static void t_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_refresh_pending) return;
    s_refresh_pending = false;
    rebuild_list();
}

static void t_clock_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;

    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);

    if (s_lbl_utc) {
        char b[16];
        snprintf(b, sizeof(b), "%02d:%02d:%02d UTC",
                 utc.tm_hour, utc.tm_min, utc.tm_sec);
        lv_label_set_text(s_lbl_utc, b);
    }
    if (s_lbl_count) {
        int sec_in_slot = (int)(now % 15);
        int remain = 15 - sec_in_slot;
        char b[16];
        snprintf(b, sizeof(b), "Slot: %2d s", remain);
        lv_label_set_text(s_lbl_count, b);
    }
    if (s_lbl_freq) {
        uint32_t hz = cat_get_frequency();
        char b[32];
        uint32_t mhz = hz / 1000000;
        uint32_t khz = (hz / 1000) % 1000;
        uint32_t rem = hz % 1000;
        snprintf(b, sizeof(b), "%lu.%03lu.%03lu",
                 (unsigned long)mhz, (unsigned long)khz, (unsigned long)rem);
        lv_label_set_text(s_lbl_freq, b);
    }
}

// ---------------- public API ----------------

void ft8_screen_view_init(lv_obj_t *parent)
{
    // Middle-band container
    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, MID_W, MID_H);
    lv_obj_set_pos(s_container, 0, MID_Y);
    lv_obj_set_style_bg_color(s_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_radius(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);   // start hidden

    // Left info pane
    s_left_pane = lv_obj_create(s_container);
    lv_obj_set_size(s_left_pane, LEFT_W, MID_H);
    lv_obj_set_pos(s_left_pane, 0, 0);
    lv_obj_set_style_bg_color(s_left_pane, lv_color_hex(0x101018), 0);
    lv_obj_set_style_border_width(s_left_pane, 0, 0);
    lv_obj_set_style_radius(s_left_pane, 0, 0);
    lv_obj_set_style_pad_all(s_left_pane, 16, 0);
    lv_obj_clear_flag(s_left_pane, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_mode = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_mode, "MODE: FT8");
    lv_obj_set_style_text_color(s_lbl_mode, lv_color_hex(0xFFD700), 0);
    lv_obj_set_style_text_font(s_lbl_mode, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(s_lbl_mode, 0, 0);

    s_lbl_freq = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_freq, "--.---.---");
    lv_obj_set_style_text_color(s_lbl_freq, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_freq, &lv_font_montserrat_32, 0);
    lv_obj_set_pos(s_lbl_freq, 0, 80);

    s_lbl_utc = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_utc, "--:--:-- UTC");
    lv_obj_set_style_text_color(s_lbl_utc, lv_color_hex(0xA0FFA0), 0);
    lv_obj_set_style_text_font(s_lbl_utc, &lv_font_montserrat_32, 0);
    lv_obj_set_pos(s_lbl_utc, 0, 160);

    s_lbl_count = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_count, "Slot: -- s");
    lv_obj_set_style_text_color(s_lbl_count, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_lbl_count, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_count, 0, 240);

    s_lbl_heard = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_heard, "Heard: 0");
    lv_obj_set_style_text_color(s_lbl_heard, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_lbl_heard, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_heard, 0, 300);

    // ME: callsign + grid (hidden until set via Identity modal)
    s_lbl_me = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_me, "");
    lv_obj_set_style_text_color(s_lbl_me, lv_color_hex(0xA0FFA0), 0);
    lv_obj_set_style_text_font(s_lbl_me, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_me, 0, 360);
    lv_obj_add_flag(s_lbl_me, LV_OBJ_FLAG_HIDDEN);

    // Right decode list
    s_right_pane = lv_obj_create(s_container);
    lv_obj_set_size(s_right_pane, RIGHT_W, MID_H);
    lv_obj_set_pos(s_right_pane, LEFT_W, 0);
    lv_obj_set_style_bg_color(s_right_pane, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_right_pane, 0, 0);
    lv_obj_set_style_radius(s_right_pane, 0, 0);
    lv_obj_set_style_pad_all(s_right_pane, 0, 0);
    lv_obj_clear_flag(s_right_pane, LV_OBJ_FLAG_SCROLLABLE);

    // Column header row (above the scrolling list)
    lv_obj_t *hdr = lv_obj_create(s_right_pane);
    lv_obj_set_size(hdr, RIGHT_W, 30);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x202028), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    struct { const char *t; int x; int w; lv_text_align_t a; } cols[5] = {
        { "CALL",    COL_CALL_X,  COL_TEXT_X - COL_CALL_X - 8,         LV_TEXT_ALIGN_LEFT  },
        { "MESSAGE", COL_TEXT_X,  COL_SCORE_X - COL_TEXT_X - 8,        LV_TEXT_ALIGN_LEFT  },
        { "SCORE",   COL_SCORE_X, COL_FREQ_X - COL_SCORE_X - 8,        LV_TEXT_ALIGN_RIGHT },
        { "FREQ",    COL_FREQ_X,  COL_HEARD_X - COL_FREQ_X - 8,        LV_TEXT_ALIGN_RIGHT },
        { "HEARD",   COL_HEARD_X, COL_RIGHT_EDGE - COL_HEARD_X - 16,   LV_TEXT_ALIGN_RIGHT },
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *lbl = lv_label_create(hdr);
        lv_label_set_text(lbl, cols[i].t);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x808080), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_width(lbl, cols[i].w);
        lv_obj_set_style_text_align(lbl, cols[i].a, 0);
        lv_obj_set_pos(lbl, cols[i].x, 5);
    }

    // Scrolling list container (plain obj, vertical flex column)
    s_list = lv_obj_create(s_right_pane);
    lv_obj_set_size(s_list, RIGHT_W, MID_H - 30);
    lv_obj_set_pos(s_list, 0, 30);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    // Absolute-position rows (no flex). Scroll vertically when overflow.
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    // Timers (always running; cheap when container hidden)
    s_t_refresh = lv_timer_create(t_refresh_cb, 500, NULL);
    s_t_clock   = lv_timer_create(t_clock_cb,  1000, NULL);

    ESP_LOGI(TAG, "FT8 view built (container %dx%d at y=%d, hidden)",
             MID_W, MID_H, MID_Y);
}

void ft8_screen_view_show(void)
{
    if (!s_container) return;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_refresh_pending = true;   // force immediate first paint
    // Refresh the ME label from NVS (call/grid may have been edited
    // since last show).
    if (s_lbl_me) {
        qmx_settings_t s;
        settings_load_all(&s);
        if (s.my_callsign[0]) {
            char buf[40];
            if (s.my_grid[0]) {
                snprintf(buf, sizeof(buf), "ME: %s %s", s.my_callsign, s.my_grid);
            } else {
                snprintf(buf, sizeof(buf), "ME: %s", s.my_callsign);
            }
            lv_label_set_text(s_lbl_me, buf);
            lv_obj_clear_flag(s_lbl_me, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_me, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ESP_LOGI(TAG, "show");
}

void ft8_screen_view_hide(void)
{
    if (!s_container) return;
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "hide");
}

void ft8_screen_view_request_refresh(void)
{
    s_refresh_pending = true;
}
