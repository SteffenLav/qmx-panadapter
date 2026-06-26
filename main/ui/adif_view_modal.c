// On-device ADIF log viewer - read-only, newest QSO first. Lets the operator
// check "did I already work this station" / verify a QSO actually logged
// without pulling qso.adi off the device. Field request (Ken KF0AYY): during
// a POTA activation with marginal copy, a QSO can complete on his end while
// his partner never copies the final RR73 and keeps re-sending a report -
// this gives him a quick way to confirm it logged before working them again.
//
// Structurally a copy of wifi_config.c's SSID-picker pattern: a centred,
// content-sized panel holding a title + a scrollable lv_list + a Close
// button. Read-only, so no per-row tap action.

#include "adif_view_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "adif/adif_log.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "adif_view_modal";

// Most recent QSOs shown - the log can grow unbounded over a long
// activation; capping keeps modal build time and memory bounded. Plenty for
// a "did I already work them" field check.
#define ADIF_VIEW_MAX_ROWS 200

static lv_obj_t *s_modal  = NULL;
static lv_obj_t *s_panel  = NULL;
static lv_obj_t *s_title  = NULL;
static lv_obj_t *s_list   = NULL;
static bool      s_open   = false;

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

// Build one display line from a raw ADIF record line, e.g.
// "KK4DUP  06-26 14:08  20M  S-08 R599"
static void format_row(const char *line, char *out, size_t out_sz)
{
    char call[20] = "?", date[9] = "", time_on[7] = "", band[8] = "",
         rst_sent[8] = "599", rst_rcvd[8] = "599";

    adif_log_extract_field(line, "CALL",     call,     sizeof(call));
    adif_log_extract_field(line, "QSO_DATE", date,     sizeof(date));
    adif_log_extract_field(line, "TIME_ON",  time_on,  sizeof(time_on));
    adif_log_extract_field(line, "BAND",     band,     sizeof(band));
    adif_log_extract_field(line, "RST_SENT", rst_sent, sizeof(rst_sent));
    adif_log_extract_field(line, "RST_RCVD", rst_rcvd, sizeof(rst_rcvd));

    // date is "YYYYMMDD", time_on is "HHMMSS" (or "HHMM") - slice down to
    // "MM-DD HH:MM" for a compact row; fall back to the raw text if short.
    char mmdd[6] = "--/--", hhmm[6] = "--:--";
    if (strlen(date) >= 8) snprintf(mmdd, sizeof(mmdd), "%.2s-%.2s", date + 4, date + 6);
    if (strlen(time_on) >= 4) snprintf(hhmm, sizeof(hhmm), "%.2s:%.2s", time_on, time_on + 2);

    snprintf(out, out_sz, "%-10s %s %s  %-4s S%s R%s",
             call, mmdd, hhmm, band[0] ? band : "--", rst_sent, rst_rcvd);
}

// Rebuild the list from the live ADIF log, newest record first.
static void list_render(void)
{
    int total = adif_log_count();
    if (s_title) {
        char t[48];
        snprintf(t, sizeof(t), "ADIF Log - %d QSO%s", total, total == 1 ? "" : "s");
        lv_label_set_text(s_title, t);
    }
    if (!s_list) return;
    lv_obj_clean(s_list);

    if (total == 0) {
        lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_CLOSE, "No QSOs logged yet");
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_24, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        return;
    }

    int shown = (total < ADIF_VIEW_MAX_ROWS) ? total : ADIF_VIEW_MAX_ROWS;
    char line[1024], row[64];
    for (int i = 0; i < shown; i++) {
        int idx = total - 1 - i;   // newest first
        if (!adif_log_get_record(idx, line, sizeof(line))) continue;
        format_row(line, row, sizeof(row));
        lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_OK, row);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_24, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);  // read-only - no tap action
    }
    ESP_LOGI(TAG, "ADIF viewer: showing %d of %d logged QSOs", shown, total);
}

static void modal_build(void)
{
    if (s_modal) return;

    lv_obj_t *scr = lv_screen_active();

    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    s_panel = lv_obj_create(s_modal);
    lv_obj_set_width(s_panel, 760);
    lv_obj_set_height(s_panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_panel, 660, 0);
    lv_obj_set_align(s_panel, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 16, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 12, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_panel);
    lv_label_set_text(s_title, "ADIF Log");
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);

    s_list = lv_list_create(s_panel);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_list, 480, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);

    lv_obj_t *close_btn = lv_btn_create(s_panel);
    lv_obj_set_size(close_btn, 240, 72);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(close_lbl);
    ui_kbd_set_buttons(NULL, close_btn);   // physical keyboard Esc -> Close
}

void adif_view_modal_init(void)
{
    modal_build();
}

void adif_view_modal_show(void)
{
    modal_build();
    list_render();
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
}
