// "What's wrong?" triage panel. Contract and rationale in help_triage.h.
//
// The list is rebuilt on every open, because its whole value is that it reflects
// the device RIGHT NOW: a row the firmware can see is actually happening (no CAT
// link, IQ never confirmed, an empty decode list) is marked and floated to the
// top. Ranking lives in help_triage_collect() (help_topics.c) so this file stays
// pure presentation.

#include "help_triage.h"
#include "help_topics.h"
#include "reader_view.h"
#include "ui_theme.h"
#include "ui.h"

#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "help_triage";

#define PANEL_W   940
#define PANEL_H   620
#define PAD       20
#define ROW_W     (PANEL_W - 2 * PAD)
#define ROW_H     68
#define ROW_GAP   8
#define ROWS_TOP  104

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_rows[HELP_TRIAGE_MAX] = {0};
static help_topic_t s_row_topic[HELP_TRIAGE_MAX] = {0};

bool help_triage_is_open(void)
{
    return s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void close_modal(void)
{
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    ui_help_overlay_changed();
}

static void close_cb(lv_event_t *e) { (void)e; close_modal(); }

static void row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= HELP_TRIAGE_MAX) return;
    help_topic_t t = s_row_topic[idx];
    const help_entry_t *entry = help_topic_get(t);
    ESP_LOGI(TAG, "triage pick: %s", entry ? entry->label : "?");
    close_modal();
    help_open(t);
}

static void manual_cb(lv_event_t *e)
{
    (void)e;
    close_modal();
    reader_view_show();   // plain manual; Contents is one tap away inside it
}

// Style one row for its current content, or hide it when unused. Rows are created
// once and restyled per open - cheaper than destroying and rebuilding widgets, and
// it keeps the panel's geometry fixed.
static void row_apply(int i, const help_triage_row_t *r)
{
    lv_obj_t *btn = s_rows[i];
    if (!btn) return;
    if (!r) { lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN); return; }

    lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
    s_row_topic[i] = r->topic;

    // A flagged row is one the device can see is happening now. It is highlighted,
    // NOT auto-opened: the operator still chooses. See help_topics.h.
    if (r->flagged) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a2a10), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xFFA040), 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x252b33), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x555555), 0);
    }

    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        if (r->flagged) {
            lv_label_set_text_fmt(lbl, LV_SYMBOL_WARNING "  %s", r->symptom);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFC864), 0);
        } else {
            lv_label_set_text(lbl, r->symptom);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xE0E0E0), 0);
        }
    }
}

static void build(void)
{
    if (s_modal) return;
    lv_obj_t *scr = lv_screen_active();

    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    // Tapping the scrim dismisses. Child clicks do not bubble by default, so the
    // panel itself is unaffected.
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal, close_cb, LV_EVENT_CLICKED, NULL);

    s_panel = lv_obj_create(s_modal);
    lv_obj_set_size(s_panel, PANEL_W, PANEL_H);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, PAD, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "What's wrong?");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(s_panel);
    lv_label_set_text(hint, "Pick what you see. Highlighted rows are happening right now.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x909090), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_22, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 64);

    for (int i = 0; i < HELP_TRIAGE_MAX; i++) {
        lv_obj_t *btn = lv_btn_create(s_panel);
        lv_obj_set_size(btn, ROW_W, ROW_H);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, ROWS_TOP + i * (ROW_H + ROW_GAP));
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_pad_left(btn, 18, 0);
        lv_obj_add_event_cb(btn, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        s_rows[i] = btn;
    }

    lv_obj_t *man_btn = lv_btn_create(s_panel);
    lv_obj_set_size(man_btn, 340, 72);
    lv_obj_align(man_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(man_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_radius(man_btn, 8, 0);
    lv_obj_add_event_cb(man_btn, manual_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *man_lbl = lv_label_create(man_btn);
    // Deliberately NOT "Show all topics": the A-Z index (Layer 4) does not exist
    // yet, and a button must not promise one. Rename it when the index lands.
    lv_label_set_text(man_lbl, "Open the manual");
    lv_obj_set_style_text_color(man_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(man_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(man_lbl);

    lv_obj_t *cl_btn = lv_btn_create(s_panel);
    lv_obj_set_size(cl_btn, 240, 72);
    lv_obj_align(cl_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(cl_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_border_color(cl_btn, lv_color_hex(0xc04040), 0);
    lv_obj_set_style_border_width(cl_btn, 2, 0);
    lv_obj_set_style_radius(cl_btn, 8, 0);
    lv_obj_add_event_cb(cl_btn, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl_lbl = lv_label_create(cl_btn);
    lv_label_set_text(cl_lbl, "Close");
    lv_obj_set_style_text_color(cl_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cl_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cl_lbl);

    ESP_LOGI(TAG, "triage panel built");
}

void help_triage_open(void)
{
    build();

    help_triage_row_t rows[HELP_TRIAGE_MAX];   // 5 * 12 B - safe on taskLVGL
    int n = help_triage_collect(rows, HELP_TRIAGE_MAX);
    for (int i = 0; i < HELP_TRIAGE_MAX; i++) row_apply(i, i < n ? &rows[i] : NULL);

    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    ui_help_overlay_changed();   // stand the top-bar hit zones and edge strips down
    ESP_LOGI(TAG, "opened with %d row(s), %d flagged", n, n ? (rows[0].flagged ? 1 : 0) : 0);
}
