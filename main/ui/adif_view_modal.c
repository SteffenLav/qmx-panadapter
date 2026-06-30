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
#include "util/dxcc.h"

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

// 8 columns (Call, Country, Mode, Band, Date, Time, Sent, Rcvd), each a
// weighted flex-grow share of the row's width rather than a fixed px value -
// proportional fonts mean padding a single string with spaces (the old
// approach) never actually lines columns up. Weights (see COL_GROW_* below)
// give Call/Country more room than the narrow fixed-format columns.
#define COL_GAP  10

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

// One flex-row container per table row (header or data) - children are
// individually-width-set labels, which is what actually makes columns line
// up with a proportional font (string padding with spaces does not).
static lv_obj_t *make_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, COL_GAP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

// flex_grow + width=0 splits the row's width by weight instead of a fixed
// px value - still resizes cleanly if the panel width ever changes again,
// but lets wide-content columns (Call, Country) claim more of it than
// narrow ones (Mode/Band/Date/Time/Sent/Rcvd, all <=5 chars of content).
// Weights mirror typical content length, not character-count exactly.
#define COL_GROW_CALL  2
#define COL_GROW_CTRY  3
#define COL_GROW_NARROW 1

static void add_col(lv_obj_t *row, const char *text, int grow,
                     const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);   // ellipsize, never wrap a column
    lv_obj_set_width(lbl, 0);
    lv_obj_set_flex_grow(lbl, grow);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
}

static void add_header_row(lv_obj_t *parent)
{
    lv_obj_t *row = make_row(parent);
    lv_obj_set_style_pad_bottom(row, 6, 0);
    add_col(row, "Call",    COL_GROW_CALL,   &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Country", COL_GROW_CTRY,   &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Mode",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Band",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Date",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Time",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Sent",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Rcvd",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
}

// Build one QSO's row from a raw ADIF record line - callsign, DXCC country
// (looked up from the callsign, not stored in the ADIF file), mode (FT8/FT4),
// band, date, time, and the sent/received signal reports as two separate
// columns. even_row alternates the row background (zebra striping) so long
// lists stay easy to read across.
static void build_qso_row(lv_obj_t *parent, const char *line, bool even_row)
{
    char call[20] = "?", date[9] = "", time_on[7] = "", band[8] = "",
         mode[8] = "FT8", rst_sent[8] = "599", rst_rcvd[8] = "599";

    adif_log_extract_field(line, "CALL",     call,     sizeof(call));
    adif_log_extract_field(line, "QSO_DATE", date,     sizeof(date));
    adif_log_extract_field(line, "TIME_ON",  time_on,  sizeof(time_on));
    adif_log_extract_field(line, "BAND",     band,     sizeof(band));
    adif_log_extract_field(line, "MODE",     mode,     sizeof(mode));
    adif_log_extract_field(line, "RST_SENT", rst_sent, sizeof(rst_sent));
    adif_log_extract_field(line, "RST_RCVD", rst_rcvd, sizeof(rst_rcvd));

    // date is "YYYYMMDD", time_on is "HHMMSS" (or "HHMM") - slice down to
    // "MM-DD" / "HH:MM" for a compact column.
    char mmdd[6] = "--/--", hhmm[6] = "--:--";
    if (strlen(date) >= 8) snprintf(mmdd, sizeof(mmdd), "%.2s-%.2s", date + 4, date + 6);
    if (strlen(time_on) >= 4) snprintf(hhmm, sizeof(hhmm), "%.2s:%.2s", time_on, time_on + 2);

    const char *country = dxcc_lookup(call);

    lv_obj_t *row = make_row(parent);
    if (even_row) {
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
        lv_obj_set_style_pad_top(row, 4, 0);
        lv_obj_set_style_pad_bottom(row, 4, 0);
    }
    add_col(row, call,                    COL_GROW_CALL,   &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, country ? country : "-", COL_GROW_CTRY,   &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, mode,                    COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, band[0] ? band : "--",   COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, mmdd,                    COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, hhmm,                    COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, rst_sent,                COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, rst_rcvd,                COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
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
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "No QSOs logged yet");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        return;
    }

    int shown = (total < ADIF_VIEW_MAX_ROWS) ? total : ADIF_VIEW_MAX_ROWS;
    char line[1024];
    for (int i = 0; i < shown; i++) {
        int idx = total - 1 - i;   // newest first
        if (!adif_log_get_record(idx, line, sizeof(line))) continue;
        build_qso_row(s_list, line, (i % 2) == 1);
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
    lv_obj_set_style_bg_opa(s_modal, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    s_panel = lv_obj_create(s_modal);
    lv_obj_set_width(s_panel, 900);
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

    // Header row sits outside the scrollable list (a sibling, not a child) so
    // column titles stay pinned in place while the QSO rows scroll under it.
    add_header_row(s_panel);

    // Plain flex-column container, not lv_list - lv_list_add_button() forces
    // a single icon+label per row, which is exactly what items #1/#2 of this
    // change removed (no per-row layout control, and a checkmark icon that
    // implied an action when this view is read-only).
    s_list = lv_obj_create(s_panel);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_list, 480, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

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
