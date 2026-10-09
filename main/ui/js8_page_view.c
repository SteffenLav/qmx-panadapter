#include "js8_page_view.h"
#include "js8_page.h"
#include "js8_chat.h"
#include "ui.h"
#include "ui_theme.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>

#include "cat/cat.h"

static const char *TAG = "js8_view";

LV_FONT_DECLARE(qmx_mono_25);

/* Same geometry as qmx_term_view and the GPS page. CELL_W must match the
 * font's advance width exactly or every column drifts along its row - see the
 * note at the top of font_qmx_mono_25.c. */
#define CELL_W     15
#define ROW_H      27
#define GRID_W     (JS8_PAGE_COLS * CELL_W)   /* 1200 */
#define GRID_H     (JS8_PAGE_ROWS * ROW_H)    /* 648  */
#define HEADER_H   62
/* One slot. Nothing on this page changes faster than a decode, so a 1 Hz
 * repaint would be 14 wasted passes over the store per slot. */
#define REFRESH_MS 2000

static lv_obj_t   *s_overlay;
static lv_obj_t   *s_dial;                 /* the heading's right-hand half */
static lv_obj_t   *s_rows[JS8_PAGE_ROWS];
static lv_timer_t *s_timer;
static bool        s_open;

/* SCRATCH IN PSRAM, NOT .bss, AND ONLY WHILE THE PAGE IS OPEN.
 *
 * About 11 kB. As a static it would sit in internal .bss, and internal .bss
 * comes straight out of the DMA pool - that is what stopped the SD card
 * mounting on 2026-10-08 ("SDFAIL[mount] err=0x101"). It cannot go on the
 * stack either: repaint() runs on the LVGL task. See
 * project_internal_bss_root_cause. */
typedef struct {
    js8_chat_msg_t msgs[JS8_CHAT_MAX_MSGS];
    char           lines[JS8_PAGE_ROWS][JS8_PAGE_COLS + 1];
    char           dial[32];
} js8_view_scratch_t;

static js8_view_scratch_t *s_scratch;

bool js8_page_view_is_open(void) { return s_open; }

static void repaint(void)
{
    if (!s_open || !s_scratch) return;

    int n = js8_chat_count();
    if (n > JS8_CHAT_MAX_MSGS) n = JS8_CHAT_MAX_MSGS;
    int have = 0;
    for (int i = 0; i < n; i++)
        if (js8_chat_at(i, &s_scratch->msgs[have])) have++;

    /* The dial, read fresh each repaint: the operator can retune with the page
     * open, and a stale heading would say the traffic came from a band it did
     * not.
     *
     * ⛔ IT LIVES IN THE HEADER, NOT IN THE GRID. The grid used to draw its own
     * title row and the header drew the same words one line above it, so the
     * page named itself twice - seen on the glass 2026-10-09. */
    uint32_t hz = cat_get_frequency();
    if (hz) snprintf(s_scratch->dial, sizeof(s_scratch->dial),
                     "%u.%03u MHz", (unsigned)(hz / 1000000u),
                     (unsigned)((hz / 1000u) % 1000u));
    else    snprintf(s_scratch->dial, sizeof(s_scratch->dial), "no CAT");
    if (s_dial) lv_label_set_text(s_dial, s_scratch->dial);

    js8_page_render(s_scratch->msgs, have, s_scratch->lines);

    for (int r = 0; r < JS8_PAGE_ROWS; r++)
        if (s_rows[r]) lv_label_set_text(s_rows[r], s_scratch->lines[r]);
}

static void tick_cb(lv_timer_t *t) { (void)t; repaint(); }
static void close_cb(lv_event_t *e) { (void)e; js8_page_view_close(); }

void js8_page_view_open(void)
{
    if (s_open) return;

    s_scratch = heap_caps_calloc(1, sizeof(*s_scratch), MALLOC_CAP_SPIRAM);
    if (!s_scratch) {
        ESP_LOGE(TAG, "no PSRAM for the JS8 page scratch (%u B) - not opening",
                 (unsigned)sizeof(*s_scratch));
        return;
    }

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps below */

    lv_obj_t *hdr = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_HOR_RES, HEADER_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "JS8 conversation");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);

    /* Right of the title, left of Close: the band this page is listening to.
     * Montserrat like the title, not the grid font - it is furniture, not a
     * column of data. */
    s_dial = lv_label_create(hdr);
    lv_label_set_text(s_dial, "");
    lv_obj_set_style_text_font(s_dial, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_dial, lv_color_hex(0xA0A0A0), 0);
    lv_obj_align(s_dial, LV_ALIGN_RIGHT_MID, -124, 0);

    lv_obj_t *cb = lv_btn_create(hdr);
    lv_obj_set_size(cb, 92, 48);
    lv_obj_align(cb, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x3a2222), 0);
    lv_obj_set_style_border_color(cb, lv_color_hex(0xB05050), 0);
    lv_obj_set_style_border_width(cb, 2, 0);
    lv_obj_add_event_cb(cb, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cl);

    lv_obj_t *grid = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, GRID_W, GRID_H);
    lv_obj_set_pos(grid, (LV_HOR_RES - GRID_W) / 2, HEADER_H + 4);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int r = 0; r < JS8_PAGE_ROWS; r++) {
        s_rows[r] = lv_label_create(grid);
        lv_obj_set_style_text_font(s_rows[r], &qmx_mono_25, 0);
        lv_obj_set_style_text_color(s_rows[r], lv_color_hex(0xD8D8D8), 0);
        lv_label_set_long_mode(s_rows[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_rows[r], GRID_W);
        lv_obj_set_pos(s_rows[r], 0, r * ROW_H);
        lv_label_set_text(s_rows[r], "");
    }

    s_open = true;
    repaint();
    s_timer = lv_timer_create(tick_cb, REFRESH_MS, NULL);
    ui_help_overlay_changed();
    ESP_LOGI(TAG, "JS8 page opened (%d message(s) held)", js8_chat_count());
}

void js8_page_view_close(void)
{
    if (!s_open) return;
    s_open = false;
    if (s_timer)   { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    if (s_scratch) { heap_caps_free(s_scratch); s_scratch = NULL; }
    memset(s_rows, 0, sizeof(s_rows));
    s_dial = NULL;
    ui_help_overlay_changed();
    ESP_LOGI(TAG, "JS8 page closed");
}
