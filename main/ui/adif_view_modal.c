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
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "adif_view_modal";

// Most recent QSOs shown - the log can grow unbounded over a long
// activation; capping keeps modal build time and memory bounded. Plenty for
// a "did I already work them" field check.
#define ADIF_VIEW_MAX_ROWS 200

// A POTA activation needs 10 QSOs to count - the whole reason for the Today
// filter (Ken KF0AYY field request: "so I know how close to 10 I am"). Once
// today's count reaches this, the title flips green: park is open.
#define POTA_ACTIVATION_QSOS 10
#define COLOR_ACTIVATION_OK  0x4caf50

static lv_obj_t *s_modal      = NULL;
static lv_obj_t *s_panel      = NULL;
static lv_obj_t *s_title      = NULL;
static lv_obj_t *s_list       = NULL;
static lv_obj_t *s_lbl_filter = NULL;   // label inside the Today/All toggle button
static bool      s_open       = false;

// Show only today's (UTC) QSOs - the POTA-activation view. Defaults to Today
// on open; show() falls back to All when nothing was logged today (reviewing
// an old log at home shouldn't open onto an empty list).
static bool s_today_only = true;

// Counts from the most recent list_render() pass, for show()'s fallback.
static int s_last_total = 0;
static int s_last_today = 0;

// Today's UTC date in ADIF QSO_DATE format ("YYYYMMDD"). QSOs are logged
// with UTC dates, so the comparison must be UTC too - not local time.
static void today_utc(char out[9])
{
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    // strftime, not snprintf("%04d...") - GCC's -Werror=format-truncation
    // can't prove tm_year+1900 fits 4 digits and rejects the latter.
    if (strftime(out, 9, "%Y%m%d", &tm_utc) == 0) out[0] = '\0';
}

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

// Rebuild the list from the live ADIF log, newest record first, honouring
// the Today/All filter. Also refreshes the title counts and the toggle
// button's label so all three always agree with what's on screen.
static void list_render(void)
{
    int64_t t_start = esp_timer_get_time();

    int total = adif_log_count();
    char today[9];
    today_utc(today);

    if (s_lbl_filter) lv_label_set_text(s_lbl_filter, s_today_only ? "Today" : "All");
    if (!s_list) return;
    lv_obj_clean(s_list);

    if (total == 0) {
        s_last_total = 0;
        s_last_today = 0;
        if (s_title) {
            lv_label_set_text(s_title, "ADIF Log - 0 QSOs");
            lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COLOR_TEXT), 0);
        }
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "No QSOs logged yet");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        return;
    }

    // Ring buffer holding the newest ring_cap filter-matching records - the
    // filter means we can't know which file offsets we'll display until the
    // whole file is scanned, so keep overwriting the oldest slot and the last
    // ring_cap matches survive. Still one fopen/fgets pass (the single-pass
    // O(N) shape that replaced the old O(N^2) per-row re-scan - keep it that
    // way). PSRAM: 200 rows * 1024 B = 200 KB max, trivial.
    int ring_cap = (total < ADIF_VIEW_MAX_ROWS) ? total : ADIF_VIEW_MAX_ROWS;
    char *lines = heap_caps_malloc((size_t)ring_cap * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lines) {
        ESP_LOGE(TAG, "OOM allocating %d-row ADIF buffer", ring_cap);
        return;
    }

    int64_t t_read_start = esp_timer_get_time();
    FILE *f = fopen(adif_log_file_path(), "r");
    int matched = 0;      // records passing the current filter (all, when All)
    int today_count = 0;  // records dated today UTC, counted regardless of filter
    if (f) {
        char raw[1024];
        bool header_skipped = false;
        while (fgets(raw, sizeof(raw), f)) {
            if (!header_skipped) { header_skipped = true; continue; }   // <EOH> line
            char date[9] = "";
            adif_log_extract_field(raw, "QSO_DATE", date, sizeof(date));
            bool is_today = (strcmp(date, today) == 0);
            if (is_today) today_count++;
            if (s_today_only && !is_today) continue;
            char *slot = lines + (size_t)(matched % ring_cap) * 1024;
            strncpy(slot, raw, 1023);
            slot[1023] = '\0';
            matched++;
        }
        fclose(f);
    } else {
        ESP_LOGW(TAG, "open %s failed", adif_log_file_path());
    }
    int64_t t_read_done = esp_timer_get_time();

    s_last_total = total;
    s_last_today = today_count;

    if (s_title) {
        char t[64];
        if (s_today_only) snprintf(t, sizeof(t), "ADIF Log - Today: %d  (%d total)", today_count, total);
        else              snprintf(t, sizeof(t), "ADIF Log - %d QSOs  (%d today)", total, today_count);
        lv_label_set_text(s_title, t);
        // Park is open: 10+ QSOs today = a valid POTA activation.
        lv_obj_set_style_text_color(s_title,
            lv_color_hex(today_count >= POTA_ACTIVATION_QSOS ? COLOR_ACTIVATION_OK : UI_COLOR_TEXT), 0);
    }

    if (matched == 0) {
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "No QSOs today yet");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        heap_caps_free(lines);
        return;
    }

    // Newest-first display: the newest match sits at (matched-1) % ring_cap,
    // walk backward from there.
    int shown = (matched < ring_cap) ? matched : ring_cap;
    for (int k = 0; k < shown; k++) {
        int idx = (matched - 1 - k) % ring_cap;
        bool even_row = (k % 2) == 1;
        build_qso_row(s_list, lines + (size_t)idx * 1024, even_row);
    }
    heap_caps_free(lines);

    int64_t t_done = esp_timer_get_time();
    ESP_LOGI(TAG, "ADIF viewer: showing %d of %d logged QSOs (filter=%s today=%d, read=%lld ms, rows=%lld ms, total=%lld ms)",
             shown, total, s_today_only ? "today" : "all", today_count,
             (long long)((t_read_done - t_read_start) / 1000),
             (long long)((t_done - t_read_done) / 1000),
             (long long)((t_done - t_start) / 1000));
}

static void filter_btn_cb(lv_event_t *e)
{
    (void)e;
    s_today_only = !s_today_only;
    list_render();
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

    // Title + Today/All toggle share one horizontal header strip so the
    // toggle doesn't cost the list any vertical space.
    lv_obj_t *hdr = lv_obj_create(s_panel);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(hdr);
    lv_label_set_text(s_title, "ADIF Log");
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);

    // Today/All filter toggle - label shows the view currently on screen.
    lv_obj_t *btn_filter = lv_btn_create(hdr);
    lv_obj_set_size(btn_filter, 160, 56);
    lv_obj_set_style_bg_color(btn_filter, lv_color_hex(0x2a2f37), 0);
    lv_obj_set_style_border_color(btn_filter, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(btn_filter, 2, 0);
    lv_obj_set_style_radius(btn_filter, 8, 0);
    lv_obj_add_event_cb(btn_filter, filter_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_filter = lv_label_create(btn_filter);
    lv_label_set_text(s_lbl_filter, "Today");
    lv_obj_set_style_text_color(s_lbl_filter, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_filter, &lv_font_montserrat_24, 0);
    lv_obj_center(s_lbl_filter);

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
    // Open on the Today view (the POTA-activation use case) - but fall back
    // to All when today is empty and older QSOs exist, so reviewing the log
    // at home doesn't open onto a blank list. An unsynced clock degrades the
    // same way: bogus "today" matches nothing -> All. A manual toggle back
    // to an empty Today is respected (this fallback runs only on open).
    s_today_only = true;
    list_render();
    if (s_last_today == 0 && s_last_total > 0) {
        s_today_only = false;
        list_render();
    }
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
}
