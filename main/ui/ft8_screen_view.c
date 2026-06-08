#include "ft8_screen_view.h"
#include "ft8_screen.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cat/cat.h"
#include "storage/settings.h"
#include "util/maidenhead.h"
#include "util/dxcc.h"

// v0.12.0: Manual FT8 TX (Reply + Call CQ) - tap a heard-station row, or
// the "Call CQ" button below, to open the confirmation modal; a small
// state indicator (armed/active, tap to cancel/abort) lives in the left
// info pane alongside "ME: <call> <grid>".
#include "ft8_tx.h"
#include "ft8_tx_modal.h"
#include "identity_config.h"

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

// Column x-offsets / widths within the row.
// Layout: CALL | MESSAGE | COUNTRY | SNR | KM | BRG | HRD
#define COL_CALL_X      12
#define COL_TEXT_X      150
#define COL_COUNTRY_X   430
#define COL_SNR_X       580
#define COL_KM_X        670
#define COL_BRG_X       770
#define COL_HEARD_X     880
#define COL_RIGHT_EDGE  960
#define ROW_H           36

#define COL_CALL_W      (COL_TEXT_X    - COL_CALL_X    - 8)
#define COL_MSG_W       (COL_COUNTRY_X - COL_TEXT_X    - 8)
#define COL_COUNTRY_W   (COL_SNR_X     - COL_COUNTRY_X - 8)
#define COL_SNR_W       (COL_KM_X      - COL_SNR_X     - 8)
#define COL_KM_W        (COL_BRG_X     - COL_KM_X      - 8)
#define COL_BRG_W       (COL_HEARD_X   - COL_BRG_X     - 8)
#define COL_HEARD_W     (COL_RIGHT_EDGE - COL_HEARD_X  - 16)

// Pool size: pre-allocated row container/label objects.
// Combined with shared lv_style_t (below), per-row local styles drop
// from ~42 to 1 (SNR colour).
//
// Row count: 20. The LVGL static pool (CONFIG_LV_MEM_SIZE_KILOBYTES)
// was raised from 64 KB to 128 KB in sdkconfig to accommodate the
// cumulative LVGL allocation of pre-built modals + drawer + 20-row
// FT8 pool (~110 KB). Below 128 KB the allocator hit hard cliffs at
// ~60 KB of cumulative LVGL objects, manifesting as NULL store
// faults, lv_obj_create hangs, or lv_obj_allocate_spec_attr lockup.
// The cost is 64 KB more internal SRAM consumed at link time
// (the pool is a static .bss array in internal RAM).
#define MAX_ROWS        20

// Shared styles. These live in BSS, not on the heap, so the dozens
// of label objects can share them via lv_obj_add_style() without
// triggering per-object local-style allocations. This was the root
// of the burger-press crash at higher row counts.
static lv_style_t s_style_row;          // row container
static lv_style_t s_style_col_call;     // CALL column (amber, left)
static lv_style_t s_style_col_msg;      // MESSAGE column (white, left)
static lv_style_t s_style_col_country;  // COUNTRY (dim, left)
static lv_style_t s_style_col_snr;      // SNR base (font/pos/align), colour set per-row
static lv_style_t s_style_col_km;       // KM (dim, right)
static lv_style_t s_style_col_brg;      // BRG (dim, right)
static lv_style_t s_style_col_heard;    // HRD (dim, right)
static lv_style_t s_style_header;       // column header row
static lv_style_t s_style_header_label; // column header text
static bool s_styles_inited = false;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *l_call;
    lv_obj_t *l_msg;
    lv_obj_t *l_country;
    lv_obj_t *l_snr;
    lv_obj_t *l_km;
    lv_obj_t *l_brg;
    lv_obj_t *l_heard;
    // Dirty-tracking cache: skip lv_label_set_text when unchanged.
    char prev_call[16];
    char prev_msg[40];
    char prev_country[24];
    char prev_snr[12];
    char prev_km[12];
    char prev_brg[12];
    char prev_heard[12];
    int16_t prev_snr_db;
    int8_t  prev_color;     /* -1=unset 0=other 1=CQ/green 2=self/red */
} row_widgets_t;

static lv_obj_t *s_container   = NULL;
static lv_obj_t *s_left_pane   = NULL;
static lv_obj_t *s_right_pane  = NULL;

static lv_obj_t *s_lbl_mode     = NULL;
static lv_obj_t *s_lbl_freq     = NULL;
static lv_obj_t *s_lbl_utc      = NULL;
static lv_obj_t *s_lbl_count    = NULL;
static lv_obj_t *s_lbl_heard    = NULL;
static lv_obj_t *s_lbl_me       = NULL;
static lv_obj_t *s_btn_cq       = NULL;  // "Call CQ" - opens the TX confirmation modal
static lv_obj_t *s_lbl_tx       = NULL;  // TX state indicator: armed/active, tap to cancel/abort
// CQ TX parity preference: -1=any slot, 0=EVEN only, 1=ODD only.
// Shown as two small toggle buttons between the slot countdown and "Heard: N".
// Tap once to lock; tap the active button again to revert to "any".
static lv_obj_t *s_btn_tx_even  = NULL;
static lv_obj_t *s_btn_tx_odd   = NULL;
static int        s_cq_parity   = -1;

static lv_obj_t *s_list         = NULL;
static row_widgets_t s_rows[MAX_ROWS];

static lv_timer_t *s_t_refresh  = NULL;
static lv_timer_t *s_t_clock    = NULL;
static char         s_my_call[16] = {0};  /* operator callsign uppercased; refreshed by 1 Hz clock timer */

static volatile bool s_refresh_pending = false;

// Touch-and-drag row selection.
//
// A hold-time gate distinguishes "swipe to scroll" from "hold to select":
//   - Finger lifts before ROW_HOLD_SELECT_MS  → no action (was a swipe/tap)
//   - Finger held ≥ ROW_HOLD_SELECT_MS        → selection mode active:
//       row highlights; dragging shifts highlight; lifting opens the modal
//   - LV_EVENT_PRESS_LOST (LVGL scroll kick-in) → cancel, no action
//
// This prevents accidental station selection while scrolling the list.
#define ROW_HOLD_SELECT_MS  400

static int      s_row_hover         = -1;
static uint32_t s_press_start_ms    = 0;
static bool     s_in_selection_mode = false;  // true while scroll is locked for drag-select

static double s_user_lat = 0.0;
static double s_user_lon = 0.0;
static bool   s_user_loc_valid = false;

// ---------------- shared style init ----------------

static void styles_init(void)
{
    if (s_styles_inited) return;
    s_styles_inited = true;

    // Row container: black bg, thin bottom border, no radius/padding.
    lv_style_init(&s_style_row);
    lv_style_set_bg_color    (&s_style_row, lv_color_hex(0x000000));
    lv_style_set_bg_opa      (&s_style_row, LV_OPA_COVER);
    lv_style_set_radius      (&s_style_row, 0);
    lv_style_set_pad_all     (&s_style_row, 0);
    lv_style_set_border_color(&s_style_row, lv_color_hex(0x303030));
    lv_style_set_border_width(&s_style_row, 1);
    lv_style_set_border_side (&s_style_row, LV_BORDER_SIDE_BOTTOM);

    // Per-column styles. Each owns: font, text colour, text align,
    // width, x offset, y offset. Sharing these saves ~7 local style
    // entries per label.
    #define INIT_COL(s, x, w, align, font, color) \
        lv_style_init(&(s)); \
        lv_style_set_text_font (&(s), (font)); \
        lv_style_set_text_color(&(s), lv_color_hex(color)); \
        lv_style_set_text_align(&(s), (align)); \
        lv_style_set_width     (&(s), (w)); \
        lv_style_set_x         (&(s), (x)); \
        lv_style_set_y         (&(s), 6);

    INIT_COL(s_style_col_call,    COL_CALL_X,    COL_CALL_W,    LV_TEXT_ALIGN_LEFT,  &lv_font_montserrat_24, 0xFFD700);
    INIT_COL(s_style_col_msg,     COL_TEXT_X,    COL_MSG_W,     LV_TEXT_ALIGN_LEFT,  &lv_font_montserrat_24, 0xFFFFFF);
    INIT_COL(s_style_col_country, COL_COUNTRY_X, COL_COUNTRY_W, LV_TEXT_ALIGN_LEFT,  &lv_font_montserrat_24, 0xC0C0C0);
    // SNR base: no colour (per-row), font/pos/align/width are shared.
    lv_style_init(&s_style_col_snr);
    lv_style_set_text_font (&s_style_col_snr, &lv_font_montserrat_24);
    lv_style_set_text_align(&s_style_col_snr, LV_TEXT_ALIGN_RIGHT);
    lv_style_set_width     (&s_style_col_snr, COL_SNR_W);
    lv_style_set_x         (&s_style_col_snr, COL_SNR_X);
    lv_style_set_y         (&s_style_col_snr, 6);

    INIT_COL(s_style_col_km,      COL_KM_X,      COL_KM_W,      LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, 0xC0C0C0);
    INIT_COL(s_style_col_brg,     COL_BRG_X,     COL_BRG_W,     LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, 0xC0C0C0);
    INIT_COL(s_style_col_heard,   COL_HEARD_X,   COL_HEARD_W,   LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, 0xC0C0C0);
    #undef INIT_COL

    // Column header bar.
    lv_style_init(&s_style_header);
    lv_style_set_bg_color(&s_style_header, lv_color_hex(0x202028));
    lv_style_set_bg_opa  (&s_style_header, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_header, 0);
    lv_style_set_radius  (&s_style_header, 0);
    lv_style_set_pad_all (&s_style_header, 0);

    // Header label: smaller font, dim grey, top padding.
    lv_style_init(&s_style_header_label);
    lv_style_set_text_font (&s_style_header_label, &lv_font_montserrat_18);
    lv_style_set_text_color(&s_style_header_label, lv_color_hex(0x808080));
    lv_style_set_y         (&s_style_header_label, 5);
}

// ---------------- helpers ----------------

static int cmp_by_utc_desc(const void *a, const void *b)
{
    const ft8_call_t *ca = (const ft8_call_t *)a;
    const ft8_call_t *cb = (const ft8_call_t *)b;
    if (cb->last_utc < ca->last_utc) return -1;
    if (cb->last_utc > ca->last_utc) return  1;
    return 0;
}

// Set label text only if it differs from cache (avoid redundant invalidate).
static void set_text_if_changed(lv_obj_t *lbl, char *cache, size_t cap, const char *txt)
{
    if (!lbl || !cache || !txt) return;
    if (strncmp(cache, txt, cap) == 0) return;
    strncpy(cache, txt, cap - 1);
    cache[cap - 1] = '\0';
    lv_label_set_text(lbl, txt);
}

static lv_color_t snr_color(int snr)
{
    if (snr >=  0)  return lv_color_hex(0x80FF80);
    if (snr >= -5)  return lv_color_hex(0xFFFFFF);
    if (snr >= -15) return lv_color_hex(0xFFA040);
    return lv_color_hex(0x707070);
}

// Create one label, attach a shared style. No local styles needed.
// Width/position/colour/font/align are all carried by the style.
static lv_obj_t *make_label_styled(lv_obj_t *row, const lv_style_t *style)
{
    lv_obj_t *lbl = lv_label_create(row);
    if (!lbl) return NULL;
    lv_obj_add_style(lbl, (lv_style_t *)style, 0);
    lv_label_set_text(lbl, "");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    return lbl;
}

// Set / clear the hover highlight on a row.  Always clears the previous row
// first.  Silently ignores hidden rows (sets s_row_hover = -1 instead).
static void row_set_hover(int new_idx)
{
    if (new_idx == s_row_hover) return;
    // Clear previous highlight
    if (s_row_hover >= 0 && s_row_hover < MAX_ROWS && s_rows[s_row_hover].row)
        lv_obj_set_style_bg_opa(s_rows[s_row_hover].row, LV_OPA_0, 0);

    s_row_hover = new_idx;

    // Apply highlight to new row (only if it is actually visible)
    if (new_idx >= 0 && new_idx < MAX_ROWS
        && s_rows[new_idx].row
        && !lv_obj_has_flag(s_rows[new_idx].row, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_style_bg_color(s_rows[new_idx].row, lv_color_hex(0x1050A0), 0);
        lv_obj_set_style_bg_opa(s_rows[new_idx].row, LV_OPA_70, 0);
    } else {
        s_row_hover = -1; // can't highlight a hidden / out-of-range row
    }
}

// Map an absolute screen Y coordinate to a row index in s_list, accounting
// for s_list's screen position and its current vertical scroll offset.
// Returns -1 when the coordinate is outside the list content area.
static int screen_y_to_row(lv_coord_t abs_y)
{
    if (!s_list) return -1;
    lv_area_t a;
    lv_obj_get_coords(s_list, &a);
    int32_t scroll_y = lv_obj_get_scroll_y(s_list);
    int32_t content_y = (int32_t)abs_y - (int32_t)a.y1 + scroll_y;
    if (content_y < 0) return -1;
    int row = (int)(content_y / ROW_H);
    if (row < 0 || row >= MAX_ROWS) return -1;
    return row;
}

// v0.12.0: confirm a row selection - resolve the callsign against a fresh
// table snapshot and open the TX confirmation modal.
// Rows are repopulated every 500 ms by rebuild_list() and are NOT
// permanently bound to a callsign, so we re-resolve here from the live table.
static void row_activate(int idx)
{
    if (idx < 0 || idx >= MAX_ROWS) return;
    row_widgets_t *r = &s_rows[idx];
    if (!r->row || !r->l_call || lv_obj_has_flag(r->row, LV_OBJ_FLAG_HIDDEN)) return;

    const char *call = lv_label_get_text(r->l_call);
    if (!call || !call[0]) return;

    static ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    const ft8_call_t *match = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].call, call) == 0) { match = &snap[i]; break; }
    }
    if (!match) {
        ESP_LOGW(TAG, "row activate: '%s' no longer in heard table - ignoring", call);
        return;
    }
    ESP_LOGI(TAG, "row activate: reply to %s (freq=%d Hz, last_utc=%lld)",
             match->call, (int)match->last_freq, (long long)match->last_utc);

    ft8_tx_request_t req;
    char err[64];
    if (ft8_tx_build_request(FT8_TX_KIND_REPLY, match->call, match->last_freq,
                             match->last_utc, &req, err, sizeof(err))) {
        ft8_tx_modal_show(&req);
    } else {
        ESP_LOGW(TAG, "build_request(reply to %s) failed: %s", match->call, err);
        identity_config_modal_show();
    }
}

// Touch-and-drag row selection handler registered on every row for
// PRESSED / PRESSING / RELEASED / PRESS_LOST:
//
//   PRESSED     - finger touches down: record timestamp, no highlight yet.
//                 The hold gate (ROW_HOLD_SELECT_MS) prevents fast scroll
//                 swipes from ever entering selection mode.
//
//   PRESSING    - fires continuously while held. Selection mode only activates
//                 once the finger has been down for ≥ ROW_HOLD_SELECT_MS; at
//                 that point the row under the fingertip highlights and dragging
//                 moves the highlight.
//
//   RELEASED    - finger lifts. Only opens the modal if selection mode was
//                 active (held long enough AND a row is highlighted). A quick
//                 swipe-and-release that never crossed the time threshold does
//                 nothing — the list just scrolls naturally.
//
//   PRESS_LOST  - LVGL detected a scroll gesture and stole the touch. Always
//                 cancels selection silently, regardless of hold time.
static void row_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        // Start the hold clock; don't highlight yet.
        s_press_start_ms    = lv_tick_get();
        s_in_selection_mode = false;
        row_set_hover(-1);

    } else if (code == LV_EVENT_PRESSING) {
        // Only enter selection mode after ROW_HOLD_SELECT_MS of continuous hold.
        uint32_t held_ms = lv_tick_get() - s_press_start_ms;
        if (held_ms >= ROW_HOLD_SELECT_MS) {
            // Lock the list scroll the first time we cross the threshold so
            // that dragging the finger to a different row doesn't also scroll
            // the whole list.
            if (!s_in_selection_mode) {
                s_in_selection_mode = true;
                if (s_list) lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
            }
            lv_indev_t *indev = lv_indev_get_act();
            if (indev) {
                lv_point_t pt;
                lv_indev_get_point(indev, &pt);
                int hover = screen_y_to_row(pt.y);
                if (hover != s_row_hover)
                    row_set_hover(hover);
            }
        }

    } else if (code == LV_EVENT_RELEASED) {
        uint32_t held_ms = lv_tick_get() - s_press_start_ms;
        int confirm = s_row_hover;
        row_set_hover(-1);
        // Restore list scroll before anything else.
        if (s_in_selection_mode && s_list)
            lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
        s_in_selection_mode = false;
        // Only fire if the hold gate was crossed AND a row was highlighted.
        // A quick tap or swipe (held_ms < threshold) just lets the list scroll.
        if (held_ms >= ROW_HOLD_SELECT_MS && confirm >= 0)
            row_activate(confirm);

    } else if (code == LV_EVENT_PRESS_LOST) {
        row_set_hover(-1);
        // Restore scroll if we somehow get PRESS_LOST while in selection mode.
        if (s_in_selection_mode && s_list)
            lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
        s_in_selection_mode = false;
        s_press_start_ms    = 0;
    }
}

static void build_row(int i)
{
    row_widgets_t *r = &s_rows[i];

    r->row = lv_obj_create(s_list);
    if (!r->row) {
        ESP_LOGE(TAG, "build_row(%d): lv_obj_create returned NULL", i);
        return;
    }
    lv_obj_set_size(r->row, RIGHT_W, ROW_H);
    lv_obj_set_pos(r->row, 0, i * ROW_H);
    lv_obj_add_style(r->row, &s_style_row, 0);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);

    // v0.12.0: touch-and-drag row selection. Finger-down highlights the row
    // immediately; dragging shifts the highlight to whichever row is under
    // the fingertip; lifting confirms the selection. A scroll swipe triggers
    // PRESS_LOST, which cancels without opening a modal.
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(r->row, (void *)(intptr_t)i);
    lv_obj_add_event_cb(r->row, row_touch_cb, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(r->row, row_touch_cb, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(r->row, row_touch_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(r->row, row_touch_cb, LV_EVENT_PRESS_LOST, NULL);

    r->l_call    = make_label_styled(r->row, &s_style_col_call);
    r->l_msg     = make_label_styled(r->row, &s_style_col_msg);
    r->l_country = make_label_styled(r->row, &s_style_col_country);
    r->l_snr     = make_label_styled(r->row, &s_style_col_snr);
    r->l_km      = make_label_styled(r->row, &s_style_col_km);
    r->l_brg     = make_label_styled(r->row, &s_style_col_brg);
    r->l_heard   = make_label_styled(r->row, &s_style_col_heard);

    // SNR colour starts white; per-row local override on update.
    if (r->l_snr) {
        lv_obj_set_style_text_color(r->l_snr, lv_color_hex(0xFFFFFF), 0);
    }

    r->prev_call[0]    = '\0';
    r->prev_msg[0]     = '\0';
    r->prev_country[0] = '\0';
    r->prev_snr[0]     = '\0';
    r->prev_km[0]      = '\0';
    r->prev_brg[0]     = '\0';
    r->prev_heard[0]   = '\0';
    r->prev_snr_db     = -127;
    r->prev_color      = -1;
}

static void update_row(int i, const ft8_call_t *src)
{
    row_widgets_t *r = &s_rows[i];
    if (!r->row) return;

    const char *country = dxcc_lookup(src->call);
    if (!country) country = "--";

    int snr = (int)src->last_snr_db;
    char b_snr[12], b_km[12], b_brg[12], b_heard[12];
    snprintf(b_snr,   sizeof(b_snr),   "%+d dB", snr);
    snprintf(b_heard, sizeof(b_heard), "(%u)",  (unsigned)src->heard_count);

    if (s_user_loc_valid && src->last_grid[0]) {
        double rlat = 0.0, rlon = 0.0;
        if (maidenhead_to_latlon(src->last_grid, &rlat, &rlon)) {
            double km  = haversine_km(s_user_lat, s_user_lon, rlat, rlon);
            double brg = bearing_deg (s_user_lat, s_user_lon, rlat, rlon);
            snprintf(b_km,  sizeof(b_km),  "%d",   (int)(km + 0.5));
            snprintf(b_brg, sizeof(b_brg), "%d°", (int)(brg + 0.5));
        } else {
            snprintf(b_km,  sizeof(b_km),  "--");
            snprintf(b_brg, sizeof(b_brg), "--");
        }
    } else {
        snprintf(b_km,  sizeof(b_km),  "--");
        snprintf(b_brg, sizeof(b_brg), "--");
    }

    set_text_if_changed(r->l_call,    r->prev_call,    sizeof(r->prev_call),    src->call);
    set_text_if_changed(r->l_msg,     r->prev_msg,     sizeof(r->prev_msg),     src->last_text);
    set_text_if_changed(r->l_country, r->prev_country, sizeof(r->prev_country), country);
    set_text_if_changed(r->l_snr,     r->prev_snr,     sizeof(r->prev_snr),     b_snr);
    set_text_if_changed(r->l_km,      r->prev_km,      sizeof(r->prev_km),      b_km);
    set_text_if_changed(r->l_brg,     r->prev_brg,     sizeof(r->prev_brg),     b_brg);
    set_text_if_changed(r->l_heard,   r->prev_heard,   sizeof(r->prev_heard),   b_heard);

    /* Ken's colour scheme: RED=own call heard, GREEN=CQ, WHITE=other */
    {
        int8_t col = 0; /* other / white */
        if (s_my_call[0] && strstr(src->last_text, s_my_call)) {
            col = 2; /* own call in message -> red */
        } else if (strncmp(src->last_text, "CQ ", 3) == 0) {
            col = 1; /* CQ -> green */
        }
        if (col != r->prev_color) {
            r->prev_color = col;
            lv_color_t c = (col == 2) ? lv_palette_main(LV_PALETTE_RED)
                         : (col == 1) ? lv_palette_main(LV_PALETTE_GREEN)
                         :              lv_color_white();
            lv_obj_set_style_text_color(r->l_call, c, 0);
            lv_obj_set_style_text_color(r->l_msg,  c, 0);
        }
    }
    if (src->last_snr_db != r->prev_snr_db) {
        r->prev_snr_db = src->last_snr_db;
        lv_obj_set_style_text_color(r->l_snr, snr_color(snr), 0);
    }

    if (lv_obj_has_flag(r->row, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_row(int i)
{
    row_widgets_t *r = &s_rows[i];
    if (!r->row) return;
    if (!lv_obj_has_flag(r->row, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(r->row, LV_OPA_0, 0); // clear any hover highlight
    }
    if (s_row_hover == i) s_row_hover = -1;
}

static void rebuild_list(void)
{
    static ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    qsort(snap, n, sizeof(ft8_call_t), cmp_by_utc_desc);

    int shown = n < MAX_ROWS ? n : MAX_ROWS;
    for (int i = 0; i < shown; i++) {
        update_row(i, &snap[i]);
    }
    for (int i = shown; i < MAX_ROWS; i++) {
        hide_row(i);
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
        bool is_even = (((int64_t)now / 15) % 2) == 0;
        char b[16];
        snprintf(b, sizeof(b), "%s  %d s", is_even ? "EVEN" : "ODD", remain);
        lv_label_set_text(s_lbl_count, b);
        // Steel blue for EVEN, warm orange for ODD - neither conflicts with the
        // TX-armed amber (0xFFA040) or any other colour already in this view.
        lv_obj_set_style_text_color(s_lbl_count,
            is_even ? lv_color_hex(0x40A0E0) : lv_color_hex(0xE09040), 0);
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

    // v0.12.0: TX state indicator - hidden when idle; amber while a reply
    // or CQ call is armed and waiting for its slot (tap to cancel); red
    // while a burst is actually on-air (tap to abort). Reuses this view's
    // existing colour conventions (0xFFA040 amber already used for
    // mid-range SNR; LV_PALETTE_RED already used for "own call heard" rows).
    if (s_lbl_tx) {
        char text[32];
        int secs_until = 0;
        ft8_tx_state_t st = ft8_tx_get_status(text, sizeof(text), &secs_until);
        char b[96];
        switch (st) {
            case FT8_TX_ARMED: {
                // Compute the parity of the slot this burst will fire in.
                // now + secs_until approximates that slot's start second;
                // (x/15)%2 gives its even/odd index, same as slot_is_even().
                bool tx_even = (((int64_t)now + secs_until) / 15) % 2 == 0;
                snprintf(b, sizeof(b), "TX armed: %s\n-> %s slot, ~%ds (tap to cancel)",
                         text, tx_even ? "EVEN" : "ODD", secs_until);
                lv_label_set_text(s_lbl_tx, b);
                lv_obj_set_style_text_color(s_lbl_tx, lv_color_hex(0xFFA040), 0);
                lv_obj_clear_flag(s_lbl_tx, LV_OBJ_FLAG_HIDDEN);
                break;
            }
            case FT8_TX_ACTIVE:
                snprintf(b, sizeof(b), "TRANSMITTING: %s\n(tap to abort)", text);
                lv_label_set_text(s_lbl_tx, b);
                lv_obj_set_style_text_color(s_lbl_tx, lv_palette_main(LV_PALETTE_RED), 0);
                lv_obj_clear_flag(s_lbl_tx, LV_OBJ_FLAG_HIDDEN);
                break;
            default:
                lv_obj_add_flag(s_lbl_tx, LV_OBJ_FLAG_HIDDEN);
                break;
        }
    }
}

// Sync button appearance to s_cq_parity: the active choice glows in the
// slot colour; the inactive choice stays dim grey.
static void update_parity_btns(void)
{
    if (!s_btn_tx_even || !s_btn_tx_odd) return;
    lv_obj_set_style_bg_color(s_btn_tx_even,
        s_cq_parity == 0 ? lv_color_hex(0x40A0E0)   // steel blue = EVEN
                         : lv_color_hex(0x303044), 0);
    lv_obj_set_style_bg_color(s_btn_tx_odd,
        s_cq_parity == 1 ? lv_color_hex(0xE09040)   // warm orange = ODD
                         : lv_color_hex(0x303044), 0);
}

static void tx_even_btn_cb(lv_event_t *e)
{
    (void)e;
    s_cq_parity = (s_cq_parity == 0) ? -1 : 0;  // tap again to deselect
    update_parity_btns();
    ESP_LOGI(TAG, "CQ parity pref: %s",
             s_cq_parity < 0 ? "any" : s_cq_parity == 0 ? "EVEN only" : "ODD only");
}

static void tx_odd_btn_cb(lv_event_t *e)
{
    (void)e;
    s_cq_parity = (s_cq_parity == 1) ? -1 : 1;  // tap again to deselect
    update_parity_btns();
    ESP_LOGI(TAG, "CQ parity pref: %s",
             s_cq_parity < 0 ? "any" : s_cq_parity == 0 ? "EVEN only" : "ODD only");
}

// v0.12.0: "Call CQ" - auto-selects the nearest clear 50-Hz audio slot to
// 1500 Hz (scanning the current heard-station table via ft8_find_clear_tone_hz),
// then opens the same confirmation modal a reply uses. parity/last_utc are
// irrelevant for CQ (fires on the very next slot boundary).
static void cq_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Call CQ tapped");

    ft8_tx_request_t req;
    char err[64];
    // Auto-select the nearest clear 50-Hz slot to 1500 Hz; if the table is
    // empty (nothing decoded yet) ft8_find_clear_tone_hz() returns 1500 Hz.
    int cq_freq_hz = ft8_find_clear_tone_hz();
    if (ft8_tx_build_request(FT8_TX_KIND_CQ, NULL, cq_freq_hz,
                             0, &req, err, sizeof(err))) {
        // Apply TX parity preference if the user has locked one.
        // build_request sets use_parity=false for CQ; we override here so
        // ft8_tx_should_run_this_slot only fires on the preferred slot type.
        if (s_cq_parity >= 0) {
            req.use_parity     = true;
            req.want_even_slot = (s_cq_parity == 0);
        }
        ft8_tx_modal_show(&req);
    } else {
        ESP_LOGW(TAG, "build_request(CQ) failed: %s", err);
        identity_config_modal_show();
    }
}

// v0.12.0: tap the TX state indicator to back out - cancels an ARMED
// request before it fires, or asks an in-flight ACTIVE burst to wind down
// early (clean TA0;/RX; tail; radio never left keyed up). No-op if IDLE
// (the label is hidden then anyway, so this shouldn't normally fire).
static void tx_indicator_tap_cb(lv_event_t *e)
{
    (void)e;
    char text[32];
    ft8_tx_state_t st = ft8_tx_get_status(text, sizeof(text), NULL);
    switch (st) {
        case FT8_TX_ARMED:
            ESP_LOGI(TAG, "TX indicator tapped while ARMED ('%s') - disarming", text);
            ft8_tx_disarm();
            break;
        case FT8_TX_ACTIVE:
            ESP_LOGI(TAG, "TX indicator tapped while ACTIVE ('%s') - requesting abort", text);
            ft8_tx_request_abort();
            break;
        default:
            break;
    }
}

// ---------------- public API ----------------

void ft8_screen_view_init(lv_obj_t *parent)
{
    styles_init();

    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, MID_W, MID_H);
    lv_obj_set_pos(s_container, 0, MID_Y);
    lv_obj_set_style_bg_color(s_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_radius(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);

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

    // CQ TX parity preference: [TX: EVEN] [TX: ODD] toggle row.
    // Fits in the 60 px gap between the slot countdown (y=240, ~28 px tall)
    // and the heard count (y=304).  Dim grey when inactive; lights up in the
    // same slot colours as s_lbl_count (steel blue / warm orange) when active.
    s_btn_tx_even = lv_btn_create(s_left_pane);
    lv_obj_set_size(s_btn_tx_even, 136, 26);
    lv_obj_set_pos(s_btn_tx_even, 0, 272);
    lv_obj_set_style_bg_color(s_btn_tx_even, lv_color_hex(0x303044), 0);
    lv_obj_set_style_border_width(s_btn_tx_even, 0, 0);
    lv_obj_set_style_radius(s_btn_tx_even, 4, 0);
    lv_obj_set_style_pad_all(s_btn_tx_even, 0, 0);
    lv_obj_add_event_cb(s_btn_tx_even, tx_even_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *even_lbl = lv_label_create(s_btn_tx_even);
    lv_label_set_text(even_lbl, "TX: EVEN");
    lv_obj_set_style_text_color(even_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(even_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(even_lbl);

    s_btn_tx_odd = lv_btn_create(s_left_pane);
    lv_obj_set_size(s_btn_tx_odd, 136, 26);
    lv_obj_set_pos(s_btn_tx_odd, 148, 272);
    lv_obj_set_style_bg_color(s_btn_tx_odd, lv_color_hex(0x303044), 0);
    lv_obj_set_style_border_width(s_btn_tx_odd, 0, 0);
    lv_obj_set_style_radius(s_btn_tx_odd, 4, 0);
    lv_obj_set_style_pad_all(s_btn_tx_odd, 0, 0);
    lv_obj_add_event_cb(s_btn_tx_odd, tx_odd_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *odd_lbl = lv_label_create(s_btn_tx_odd);
    lv_label_set_text(odd_lbl, "TX: ODD");
    lv_obj_set_style_text_color(odd_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(odd_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(odd_lbl);

    update_parity_btns();  // sync colours to s_cq_parity (persists on FT8 re-entry)

    s_lbl_heard = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_heard, "Heard: 0");
    lv_obj_set_style_text_color(s_lbl_heard, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_lbl_heard, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_heard, 0, 304);

    s_lbl_me = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_me, "");
    lv_obj_set_style_text_color(s_lbl_me, lv_color_hex(0xA0FFA0), 0);
    lv_obj_set_style_text_font(s_lbl_me, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_me, 0, 360);
    lv_obj_add_flag(s_lbl_me, LV_OBJ_FLAG_HIDDEN);

    // v0.12.0: "Call CQ" - opens the TX confirmation modal pre-filled with
    // a CQ message at the conventional default audio tone. Coloured the
    // same green as the modal's "Transmit" / identity modal's "Save"
    // buttons - this app's established "primary action" colour - tying
    // the two steps of the flow together visually.
    s_btn_cq = lv_btn_create(s_left_pane);
    lv_obj_set_size(s_btn_cq, 288, 60);
    lv_obj_set_pos(s_btn_cq, 0, 410);
    lv_obj_set_style_bg_color(s_btn_cq, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_border_color(s_btn_cq, lv_color_hex(0x4caf50), 0);
    lv_obj_set_style_border_width(s_btn_cq, 2, 0);
    lv_obj_set_style_radius(s_btn_cq, 8, 0);
    lv_obj_add_event_cb(s_btn_cq, cq_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cq_lbl = lv_label_create(s_btn_cq);
    lv_label_set_text(cq_lbl, "Call CQ");
    lv_obj_set_style_text_color(cq_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cq_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cq_lbl);

    // TX state indicator - hidden while idle; amber/armed or red/active,
    // tap to cancel/abort. See t_clock_cb (1 Hz refresh: state, colour,
    // countdown text) and tx_indicator_tap_cb (the tap action itself).
    s_lbl_tx = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_tx, "");
    lv_obj_set_style_text_font(s_lbl_tx, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(s_lbl_tx, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_tx, 288);
    lv_obj_set_pos(s_lbl_tx, 0, 482);
    lv_obj_add_flag(s_lbl_tx, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lbl_tx, tx_indicator_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_lbl_tx, LV_OBJ_FLAG_HIDDEN);

    // Right pane
    s_right_pane = lv_obj_create(s_container);
    lv_obj_set_size(s_right_pane, RIGHT_W, MID_H);
    lv_obj_set_pos(s_right_pane, LEFT_W, 0);
    lv_obj_set_style_bg_color(s_right_pane, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_right_pane, 0, 0);
    lv_obj_set_style_radius(s_right_pane, 0, 0);
    lv_obj_set_style_pad_all(s_right_pane, 0, 0);
    lv_obj_clear_flag(s_right_pane, LV_OBJ_FLAG_SCROLLABLE);

    // Column header (shared styles)
    lv_obj_t *hdr = lv_obj_create(s_right_pane);
    lv_obj_set_size(hdr, RIGHT_W, 30);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_add_style(hdr, &s_style_header, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    struct { const char *t; int x; int w; lv_text_align_t a; } cols[7] = {
        { "CALL",    COL_CALL_X,    COL_CALL_W,    LV_TEXT_ALIGN_LEFT  },
        { "MESSAGE", COL_TEXT_X,    COL_MSG_W,     LV_TEXT_ALIGN_LEFT  },
        { "COUNTRY", COL_COUNTRY_X, COL_COUNTRY_W, LV_TEXT_ALIGN_LEFT  },
        { "SNR",     COL_SNR_X,     COL_SNR_W,     LV_TEXT_ALIGN_RIGHT },
        { "KM",      COL_KM_X,      COL_KM_W,      LV_TEXT_ALIGN_RIGHT },
        { "BRG",     COL_BRG_X,     COL_BRG_W,     LV_TEXT_ALIGN_RIGHT },
        { "HRD",     COL_HEARD_X,   COL_HEARD_W,   LV_TEXT_ALIGN_RIGHT },
    };
    for (int i = 0; i < 7; i++) {
        lv_obj_t *lbl = lv_label_create(hdr);
        lv_obj_add_style(lbl, &s_style_header_label, 0);
        lv_label_set_text(lbl, cols[i].t);
        lv_obj_set_width(lbl, cols[i].w);
        lv_obj_set_x(lbl, cols[i].x);
        lv_obj_set_style_text_align(lbl, cols[i].a, 0);
    }

    // Scrolling list container
    s_list = lv_obj_create(s_right_pane);
    lv_obj_set_size(s_list, RIGHT_W, MID_H - 30);
    lv_obj_set_pos(s_list, 0, 30);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    // Pre-allocate the row pool at boot, when ~199 KB internal heap is
    // free. See MAX_ROWS comment block for the beta3 rationale.
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    memset(s_rows, 0, sizeof(s_rows));
    for (int i = 0; i < MAX_ROWS; i++) {
        build_row(i);
    }
    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "row pool built (%d rows, heap_i %u -> %u, delta %d B)",
             MAX_ROWS, (unsigned)heap_before, (unsigned)heap_after,
             (int)heap_after - (int)heap_before);

    s_t_refresh = lv_timer_create(t_refresh_cb, 500, NULL);
    s_t_clock   = lv_timer_create(t_clock_cb,  1000, NULL);

    ESP_LOGI(TAG, "FT8 view built (container %dx%d at y=%d, hidden)",
             MID_W, MID_H, MID_Y);
}

void ft8_screen_view_show(void)
{
    if (!s_container) return;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_refresh_pending = true;

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
            /* refresh callsign cache for row colour scheme */
            {
                size_t ci;
                for (ci = 0; ci < sizeof(s_my_call) - 1 && s.my_callsign[ci]; ci++)
                    s_my_call[ci] = (s.my_callsign[ci] >= 'a' && s.my_callsign[ci] <= 'z')
                                   ? (char)(s.my_callsign[ci] - 32) : s.my_callsign[ci];
                s_my_call[ci] = 0;
            }
            lv_label_set_text(s_lbl_me, buf);
            lv_obj_clear_flag(s_lbl_me, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_me, LV_OBJ_FLAG_HIDDEN);
        }
        s_user_loc_valid = false;
        if (s.my_grid[0]) {
            s_user_loc_valid = maidenhead_to_latlon(s.my_grid,
                                                    &s_user_lat,
                                                    &s_user_lon);
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
