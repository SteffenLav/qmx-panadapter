#include "ft8_screen_view.h"
#include "ui_theme.h"
#include "ft8_screen.h"
#include "ft8_cq_modal.h"
#include "ui_clock.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cat/cat.h"
#include "display/display.h"
#include "storage/settings.h"
#include "util/maidenhead.h"
#include "util/dxcc.h"

// v0.12.0: Manual FT8 TX (Reply + Call CQ) - tap a heard-station row, or
// the "Call CQ" button below, to open the confirmation modal; a small
// state indicator (armed/active, tap to cancel/abort) lives in the left
// info pane alongside "ME: <call> <grid>".
#include "ft8_tx.h"
#include "ft8_qso.h"
#include "ft8_status.h"
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
// Layout: SL | CALL | MESSAGE | COUNTRY | SNR | KM | BRG | HRD
// SL = slot parity (E blue / O amber). CALL…KM shifted +10 px right;
// KM absorbs the 10 px taken from its right edge (BRG/HEARD unchanged).
// SNR/KM/BRG are shifted a further +15 px right with their widths held
// fixed (COL_SNR_W/COL_KM_W/COL_BRG_W are constants, not derived from the
// next column's X) so they move without growing or shrinking.
#define COL_SLOT_X      6
#define COL_SLOT_W      22
#define COL_CALL_X      46
#define COL_TEXT_X      184
#define COL_COUNTRY_X   479
#define COL_SNR_X       629
#define COL_KM_X        719
#define COL_BRG_X       785
#define COL_HEARD_X     880
#define COL_RIGHT_EDGE  960
#define ROW_H           36

#define COL_CALL_W      (COL_TEXT_X    - COL_CALL_X    - 8)
#define COL_MSG_W       272
#define COL_COUNTRY_W   157
#define COL_SNR_W       82
#define COL_KM_W        71
#define COL_BRG_W       102
#define COL_HEARD_W     (COL_RIGHT_EDGE - COL_HEARD_X  - 16)

// Pool size: pre-allocated row container/label objects.
// Combined with shared lv_style_t (below), per-row local styles drop
// from ~42 to 1 (SNR colour).
//
// Row count: 40 (raised from 20 in v0.16.0 — the dual-buffer ping-pong
// decode now produces a result every slot, and busy bands can yield
// ~50 decodes/slot, so 20 visible rows lost too many signals).
// The LVGL static pool (CONFIG_LV_MEM_SIZE_KILOBYTES) was raised from
// 128 KB to 256 KB in sdkconfig to accommodate the larger cumulative
// LVGL allocation of pre-built modals + drawer + 40-row FT8 pool
// (~110 KB for 20 rows, so roughly ~220 KB for 40). Below 128 KB the
// allocator hit hard cliffs at ~60 KB of cumulative LVGL objects,
// manifesting as NULL store faults, lv_obj_create hangs, or
// lv_obj_allocate_spec_attr lockup — verify via the boot-time
// "row pool built (... heap_i -> ..., delta ...)" log and the
// "Free PSRAM/free internal" log after this change.
// The cost is additional internal SRAM consumed at link time
// (the pool is a static .bss array in internal RAM).
#define MAX_ROWS        40

// Shared styles. These live in BSS, not on the heap, so the dozens
// of label objects can share them via lv_obj_add_style() without
// triggering per-object local-style allocations. This was the root
// of the burger-press crash at higher row counts.
static lv_style_t s_style_row;          // row container
static lv_style_t s_style_col_slot;     // SL column (font/pos/size; colour set per-row)
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
    lv_obj_t *l_slot;
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
    int8_t  prev_color;          /* -1=unset 0=other 1=CQ/green 2=self/red */
    int8_t  prev_slot_parity;    /* -1=unset 0=odd 1=even */
} row_widgets_t;

static lv_obj_t *s_container   = NULL;
static lv_obj_t *s_left_pane   = NULL;
static lv_obj_t *s_right_pane  = NULL;

static lv_obj_t *s_lbl_mode     = NULL;
static lv_obj_t *s_lbl_freq     = NULL;
static lv_obj_t *s_ft8_freq_hit = NULL;  // enlarged touch target over s_lbl_freq
static ui_clock_t s_clk_utc;
static lv_obj_t *s_lbl_utc_suffix = NULL;
static lv_obj_t *s_lbl_count    = NULL;
static lv_obj_t *s_bar_slot     = NULL;  // tiny countdown bar beside s_lbl_count
static lv_obj_t *s_lbl_heard    = NULL;
static lv_obj_t *s_lbl_me       = NULL;
static lv_obj_t *s_btn_cq       = NULL;  // "Call CQ" - short tap TX, long-press edits presets
static lv_obj_t *s_cq_lbl       = NULL;  // label inside s_btn_cq (shows the active CQ message)
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
static lv_timer_t *s_t_slotbar  = NULL;  // fast tick for smooth countdown bar
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
#define ROW_HOLD_SELECT_MS  700

static int      s_row_hover         = -1;
static uint32_t s_press_start_ms    = 0;
static bool     s_in_selection_mode = false;  // true while scroll is locked for drag-select
static lv_point_t s_press_start_pt  = {0, 0};
static bool     s_scroll_detected   = false;  // true if finger moved enough to be a scroll, ever
#define ROW_SCROLL_CANCEL_PX 12  // finger movement beyond this before the hold gate cancels selection

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

    // Slot parity (E/O): no colour (set per-row), font/pos/align shared.
    lv_style_init(&s_style_col_slot);
    lv_style_set_text_font (&s_style_col_slot, &lv_font_montserrat_24);
    lv_style_set_text_align(&s_style_col_slot, LV_TEXT_ALIGN_LEFT);
    lv_style_set_width     (&s_style_col_slot, COL_SLOT_W);
    lv_style_set_x         (&s_style_col_slot, COL_SLOT_X);
    lv_style_set_y         (&s_style_col_slot, 6);

    INIT_COL(s_style_col_call,    COL_CALL_X,    COL_CALL_W,    LV_TEXT_ALIGN_LEFT,  &lv_font_montserrat_24, UI_COLOR_ACCENT_GOLD);
    INIT_COL(s_style_col_msg,     COL_TEXT_X,    COL_MSG_W,     LV_TEXT_ALIGN_LEFT,  &lv_font_montserrat_24, 0xFFFFFF);
    INIT_COL(s_style_col_country, COL_COUNTRY_X, COL_COUNTRY_W, LV_TEXT_ALIGN_LEFT,  &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY);
    // SNR base: no colour (per-row), font/pos/align/width are shared.
    lv_style_init(&s_style_col_snr);
    lv_style_set_text_font (&s_style_col_snr, &lv_font_montserrat_24);
    lv_style_set_text_align(&s_style_col_snr, LV_TEXT_ALIGN_RIGHT);
    lv_style_set_width     (&s_style_col_snr, COL_SNR_W);
    lv_style_set_x         (&s_style_col_snr, COL_SNR_X);
    lv_style_set_y         (&s_style_col_snr, 6);

    INIT_COL(s_style_col_km,      COL_KM_X,      COL_KM_W,      LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY);
    INIT_COL(s_style_col_brg,     COL_BRG_X,     COL_BRG_W,     LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY);
    INIT_COL(s_style_col_heard,   COL_HEARD_X,   COL_HEARD_W,   LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY);
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
    lv_style_set_text_color(&s_style_header_label, lv_color_hex(UI_COLOR_TEXT_MUTED));
    lv_style_set_y         (&s_style_header_label, 5);
}

// ---------------- helpers ----------------

static int cmp_cq_then_snr(const void *a, const void *b)
{
    const ft8_call_t *ca = (const ft8_call_t *)a;
    const ft8_call_t *cb = (const ft8_call_t *)b;
    // Messages directed at us (red rows) always sort first, regardless of CQ.
    bool a_me = s_my_call[0] && strstr(ca->last_text, s_my_call);
    bool b_me = s_my_call[0] && strstr(cb->last_text, s_my_call);
    if (a_me != b_me) return b_me ? 1 : -1;
    bool a_cq = (strncmp(ca->last_text, "CQ ", 3) == 0);
    bool b_cq = (strncmp(cb->last_text, "CQ ", 3) == 0);
    if (a_cq != b_cq) return b_cq ? 1 : -1;  // CQ rows first
    // Within same category: strongest SNR first
    if (cb->last_snr_db > ca->last_snr_db) return  1;
    if (cb->last_snr_db < ca->last_snr_db) return -1;
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
    return lv_color_hex(UI_COLOR_TEXT_MUTED);
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
        lv_obj_set_style_bg_color(s_rows[new_idx].row, lv_color_hex(UI_COLOR_PRIMARY), 0);
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
                             match->last_utc, NULL, &req, err, sizeof(err))) {
        ft8_tx_modal_show(&req);
    } else {
        ESP_LOGW(TAG, "build_request(reply to %s) failed: %s", match->call, err);
        identity_config_modal_show();
    }
}

// Touch-and-drag row selection handler registered on every row for
// PRESSED / PRESSING / RELEASED / PRESS_LOST:
//
//   PRESSED     - finger touches down: record timestamp + start point, no
//                 highlight yet. The hold gate (ROW_HOLD_SELECT_MS) prevents
//                 fast scroll swipes from ever entering selection mode.
//
//   PRESSING    - fires continuously while held. If the finger ever moves more
//                 than ROW_SCROLL_CANCEL_PX from its start point, this touch is
//                 a scroll/swipe and is permanently barred from entering
//                 selection mode (even if held past the threshold afterwards).
//                 Otherwise, once held ≥ ROW_HOLD_SELECT_MS, selection mode
//                 activates: the row under the fingertip highlights, list
//                 scroll locks, and dragging moves the highlight.
//
//   RELEASED    - finger lifts. Only opens the modal if selection mode was
//                 active (held long enough, no scroll motion, AND a row is
//                 highlighted). A swipe, or a quick tap that never crossed the
//                 time threshold, does nothing — the list just scrolls naturally.
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
        s_scroll_detected   = false;
        row_set_hover(-1);
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) lv_indev_get_point(indev, &s_press_start_pt);

    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t pt = s_press_start_pt;
        if (indev) lv_indev_get_point(indev, &pt);

        if (!s_in_selection_mode && !s_scroll_detected) {
            // If the finger has moved more than a few px before the hold
            // gate fires, this is a scroll/swipe — never enter selection
            // mode for this touch, even if held longer afterwards.
            int32_t dx = pt.x - s_press_start_pt.x;
            int32_t dy = pt.y - s_press_start_pt.y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > ROW_SCROLL_CANCEL_PX || dy > ROW_SCROLL_CANCEL_PX) {
                s_scroll_detected = true;
            }
        }

        // Only enter selection mode after ROW_HOLD_SELECT_MS of continuous
        // hold AND no scroll motion was ever detected during this touch.
        uint32_t held_ms = lv_tick_get() - s_press_start_ms;
        if (held_ms >= ROW_HOLD_SELECT_MS && !s_scroll_detected) {
            // Lock the list scroll the first time we cross the threshold so
            // that dragging the finger to a different row doesn't also scroll
            // the whole list.
            if (!s_in_selection_mode) {
                s_in_selection_mode = true;
                if (s_list) lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
            }
            int hover = screen_y_to_row(pt.y);
            if (hover != s_row_hover)
                row_set_hover(hover);
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

    r->l_slot    = make_label_styled(r->row, &s_style_col_slot);
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
    r->prev_snr_db       = -127;
    r->prev_color        = -1;
    r->prev_slot_parity  = -1;
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
    snprintf(b_heard, sizeof(b_heard), "%u",  (unsigned)src->heard_count);

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

    /* E (blue) / O (amber) slot parity indicator */
    {
        int8_t parity = ((src->last_utc / 15) % 2) == 0 ? 1 : 0; /* 1=even 0=odd */
        if (parity != r->prev_slot_parity) {
            r->prev_slot_parity = parity;
            lv_label_set_text(r->l_slot, parity ? "E" : "O");
            lv_obj_set_style_text_color(r->l_slot,
                parity ? lv_color_hex(UI_COLOR_PRIMARY_BORDER)   /* steel blue = EVEN */
                       : lv_color_hex(0xE09040),  /* warm orange = ODD */
                0);
        }
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
    qsort(snap, n, sizeof(ft8_call_t), cmp_cq_then_snr);

    // While we're running our own CQ, hide other stations' CQ rows so replies
    // addressed to us aren't buried under unrelated CQ traffic.
    bool hide_cq = ft8_qso_cq_filter_active();

    int row = 0;
    for (int i = 0; i < n && row < MAX_ROWS; i++) {
        if (hide_cq && strncmp(snap[i].last_text, "CQ ", 3) == 0) continue;
        update_row(row++, &snap[i]);
    }
    for (int i = row; i < MAX_ROWS; i++) {
        hide_row(i);
    }

    if (s_lbl_heard) {
        char b[32];
        snprintf(b, sizeof(b), "Active: %d", n);
        lv_label_set_text(s_lbl_heard, b);
    }
}

static void t_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_refresh_pending) return;
    // Don't reorder/reposition rows while the user is drag-selecting one —
    // the row under their finger must stay put until they lift.
    if (s_in_selection_mode) return;
    s_refresh_pending = false;
    rebuild_list();
}

// Smoothly drive the 15 s slot countdown bar. Runs fast (~50 ms) and uses
// sub-second time so the bar glides to 0 instead of snapping per second.
// Bar range is 0..15000 (ms remaining). Also owns the bar colour: red while
// a TX burst is ACTIVE, otherwise the EVEN/ODD slot colour.
static void t_slotbar_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_bar_slot) return;
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    // Position within the current 15 s slot, in milliseconds (0..15000).
    int slot_ms = (int)(((int64_t)tv.tv_sec % 15) * 1000 + tv.tv_usec / 1000);
    int remain_ms = 15000 - slot_ms;
    if (remain_ms < 0) remain_ms = 0;
    lv_bar_set_value(s_bar_slot, remain_ms, LV_ANIM_OFF);

    lv_color_t col;
    if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_ACTIVE) {
        col = lv_palette_main(LV_PALETTE_RED);
    } else {
        bool is_even = (((int64_t)tv.tv_sec / 15) % 2) == 0;
        col = is_even ? lv_color_hex(UI_COLOR_PRIMARY_BORDER) : lv_color_hex(0xE09040);
    }
    lv_obj_set_style_bg_color(s_bar_slot, col, LV_PART_INDICATOR);
}

static void t_clock_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;

    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);

    ui_clock_set_time(&s_clk_utc, utc.tm_hour, utc.tm_min, utc.tm_sec);
    if (s_lbl_count) {
        int sec_in_slot = (int)(now % 15);
        int remain = 15 - sec_in_slot;
        bool is_even = (((int64_t)now / 15) % 2) == 0;
        char b[16];
        snprintf(b, sizeof(b), "%s  %d s", is_even ? "EVEN" : "ODD", remain);
        lv_label_set_text(s_lbl_count, b);
        // Steel blue for EVEN, warm orange for ODD - neither conflicts with the
        // TX-armed amber (0xFFA040) or any other colour already in this view.
        lv_color_t slot_color = is_even ? lv_color_hex(UI_COLOR_PRIMARY_BORDER) : lv_color_hex(0xE09040);
        lv_obj_set_style_text_color(s_lbl_count, slot_color, 0);
        // The bar's value AND colour are owned by t_slotbar_cb (fast tick) so
        // it glides smoothly and can show TX-red without this 1 Hz tick fighting it.
    }
    if (s_lbl_freq) {
        uint32_t hz = cat_get_frequency();
        char b[40];
        uint32_t mhz = hz / 1000000;
        uint32_t khz_frac = (hz / 1000) % 1000;
        snprintf(b, sizeof(b), "Preset: %lu.%03lu MHz",
                 (unsigned long)mhz, (unsigned long)khz_frac);
        lv_label_set_text(s_lbl_freq, b);
    }

    // Status / TX / QSO indicator — always visible.
    // Priority: ACTIVE (red) > ARMED (amber) > QSO state (cyan) > ft8_status (dim white).
    if (s_lbl_tx) {
        char tx_text[32];
        int  secs_until = 0;
        ft8_tx_state_t tx_st = ft8_tx_get_status(tx_text, sizeof(tx_text), &secs_until);
        ft8_qso_state_t qso_st = ft8_qso_get_state();
        char b[96];

        if (tx_st == FT8_TX_ACTIVE) {
            // Red: transmitting right now (tap to abort)
            snprintf(b, sizeof(b), "TRANSMITTING: %s\n(tap to abort)", tx_text);
            lv_label_set_text(s_lbl_tx, b);
            lv_obj_set_style_text_color(s_lbl_tx, lv_palette_main(LV_PALETTE_RED), 0);

        } else if (tx_st == FT8_TX_ARMED) {
            // Amber: burst scheduled (tap to cancel)
            bool tx_even = (((int64_t)now + secs_until) / 15) % 2 == 0;
            snprintf(b, sizeof(b), "TX armed: %s\n-> %s slot, ~%ds (tap to cancel)",
                     tx_text, tx_even ? "EVEN" : "ODD", secs_until);
            lv_label_set_text(s_lbl_tx, b);
            lv_obj_set_style_text_color(s_lbl_tx, lv_color_hex(0xFFA040), 0);

        } else if (qso_st == FT8_QSO_DONE) {
            // Bright green: QSO complete
            char target[FT8_CALL_MAX_LEN];
            ft8_qso_get_target(target, sizeof(target));
            snprintf(b, sizeof(b), "QSO %s: complete!", target);
            lv_label_set_text(s_lbl_tx, b);
            lv_obj_set_style_text_color(s_lbl_tx, lv_palette_main(LV_PALETTE_GREEN), 0);

        } else if (qso_st == FT8_QSO_TIMEOUT) {
            // Orange-red: QSO timed out (tap to clear)
            char target[FT8_CALL_MAX_LEN];
            ft8_qso_get_target(target, sizeof(target));
            snprintf(b, sizeof(b), "QSO %s: timeout\n(tap to clear)", target);
            lv_label_set_text(s_lbl_tx, b);
            lv_obj_set_style_text_color(s_lbl_tx, lv_color_hex(0xFF6020), 0);

        } else {
            // Dim white: ft8_status passthrough (RX state, decode count, etc.)
            char status[96];
            ft8_status_get(status, sizeof(status));
            lv_label_set_text(s_lbl_tx, status[0] ? status : "Idle");
            lv_obj_set_style_text_color(s_lbl_tx, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        }
        lv_obj_clear_flag(s_lbl_tx, LV_OBJ_FLAG_HIDDEN);
    }

    // Rebuild the decode list every second so stations that have gone stale
    // (not re-decoded within FT8_ROW_STALE_SEC) leave the view even when the
    // band is quiet and no fresh decode is triggering a refresh on its own.
    s_refresh_pending = true;
}

// Sync button appearance to s_cq_parity: the active choice glows in the
// slot colour; the inactive choice stays dim grey.
static void update_parity_btns(void)
{
    if (!s_btn_tx_even || !s_btn_tx_odd) return;
    lv_obj_set_style_bg_color(s_btn_tx_even,
        s_cq_parity == 0 ? lv_color_hex(UI_COLOR_PRIMARY_BORDER)   // steel blue = EVEN
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

    // Transmit the user's selected CQ preset (defaults to "CQ <call> <grid>").
    char cq_text[28];
    ft8_cq_get_active_text(cq_text, sizeof(cq_text));
    if (ft8_tx_build_request_text(cq_text, cq_freq_hz, &req, err, sizeof(err))) {
        if (s_cq_parity >= 0) {
            req.use_parity     = true;
            req.want_even_slot = (s_cq_parity == 0);
        }
        char qso_err[64];
        if (!ft8_qso_start_cq(&req, qso_err, sizeof(qso_err))) {
            ESP_LOGW(TAG, "CQ start failed: %s", qso_err);
            if (strstr(qso_err, "callsign") || strstr(qso_err, "Set your")) {
                identity_config_modal_show();
            }
        }
    } else {
        ESP_LOGW(TAG, "build_request(CQ '%s') failed: %s", cq_text, err);
        identity_config_modal_show();
    }
}

// Long-press "Call CQ" -> open the CQ preset editor.
static void cq_btn_long_press_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Call CQ long-pressed -> CQ preset editor");
    ft8_cq_modal_show();
}

// Update the Call CQ button label to show the currently-selected CQ message.
// Public so the preset modal can call it after a save.
void ft8_screen_view_refresh_cq_label(void)
{
    if (!s_cq_lbl) return;
    char txt[28];
    ft8_cq_get_active_text(txt, sizeof(txt));
    lv_label_set_text(s_cq_lbl, txt);
}

// v0.12.0: tap the TX state indicator to back out - cancels an ARMED
// request before it fires, or asks an in-flight ACTIVE burst to wind down
// early (clean TA0;/RX; tail; radio never left keyed up). No-op if IDLE
// (the label is hidden then anyway, so this shouldn't normally fire).
static void tx_indicator_tap_cb(lv_event_t *e)
{
    (void)e;
    char text[32];
    ft8_tx_state_t  tx_st  = ft8_tx_get_status(text, sizeof(text), NULL);
    ft8_qso_state_t qso_st = ft8_qso_get_state();

    if (tx_st == FT8_TX_ACTIVE) {
        ESP_LOGI(TAG, "TX indicator tapped ACTIVE — requesting abort");
        ft8_qso_abort();          // also aborts the auto-pounce QSO if one is running
        ft8_tx_request_abort();
    } else if (tx_st == FT8_TX_ARMED) {
        ESP_LOGI(TAG, "TX indicator tapped ARMED — disarming");
        ft8_qso_abort();
        ft8_tx_disarm();
    } else if (qso_st == FT8_QSO_TIMEOUT) {
        ESP_LOGI(TAG, "QSO timeout indicator tapped — clearing");
        ft8_qso_abort();          // resets to IDLE
    }
}

// ---- FT8 frequency preset popup -----------------------------------------
// Tapping the dial-frequency label opens a dropdown of conventional FT8 dial
// frequencies for each band the QMX supports (from cat_get_band_list()).
typedef struct {
    const char *band;     // matches cat_band_entry_t.name, e.g. "40"
    uint32_t    freq_hz;  // conventional FT8 dial frequency
} ft8_band_freq_t;

static const ft8_band_freq_t FT8_BAND_FREQS[] = {
    { "160", 1840000  },
    { "80",  3573000  },
    { "60",  5357000  },
    { "40",  7074000  },
    { "30",  10136000 },
    { "20",  14074000 },
    { "17",  18100000 },
    { "15",  21074000 },
    { "12",  24915000 },
    { "10",  28074000 },
    { "6",   50313000 },
};
#define N_FT8_BAND_FREQS (sizeof(FT8_BAND_FREQS) / sizeof(FT8_BAND_FREQS[0]))

static lv_obj_t *s_ft8_freq_popup = NULL;

static void ft8_freq_popup_close(void)
{
    if (s_ft8_freq_popup) { lv_obj_delete(s_ft8_freq_popup); s_ft8_freq_popup = NULL; }
}

static void ft8_freq_preset_cb(lv_event_t *e)
{
    uint32_t freq_hz = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    ft8_freq_popup_close();
    cat_set_frequency(freq_hz);
}

static void ft8_freq_overlay_cb(lv_event_t *e)
{
    (void)e;
    ft8_freq_popup_close();
}

static void ft8_freq_label_clicked_cb(lv_event_t *e);

static void ft8_freq_popup_open(void)
{
    if (s_ft8_freq_popup) { ft8_freq_popup_close(); return; }

    int band_count = 0;
    const cat_band_entry_t *bands = cat_get_band_list(&band_count);
    if (band_count == 0) {
        ESP_LOGW(TAG, "FT8 freq dropdown: no bands available (band_count=0)");
        return;
    }

    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, ft8_freq_overlay_cb, LV_EVENT_CLICKED, NULL);
    s_ft8_freq_popup = ov;

    int btn_h = 56;
    int panel_w = 220;
    int panel_h = band_count * btn_h + 32;
    int panel_x = 8;
    int panel_y = MID_Y + 80;
    int max_h = DISPLAY_V_RES - panel_y - 4;
    bool needs_scroll = panel_h > max_h;
    if (needs_scroll) panel_h = max_h;

    lv_obj_t *panel = lv_obj_create(ov);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_pos(panel, panel_x, panel_y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_min_height(panel, 0, 0);
    lv_obj_set_style_min_width(panel, 0, 0);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, 0, 0);
    lv_obj_set_style_pad_column(panel, 0, 0);
    if (needs_scroll) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    } else {
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    uint32_t cur_hz = cat_get_frequency();
    for (int i = 0; i < band_count; i++) {
        // Find the FT8 dial frequency for this band, if we know one.
        uint32_t ft8_hz = 0;
        for (size_t j = 0; j < N_FT8_BAND_FREQS; j++) {
            if (strcmp(bands[i].name, FT8_BAND_FREQS[j].band) == 0) {
                ft8_hz = FT8_BAND_FREQS[j].freq_hz;
                break;
            }
        }
        if (ft8_hz == 0) continue;  // no known FT8 freq for this band

        bool active = (cur_hz >= bands[i].center_hz - 1000000 &&
                       cur_hz <= bands[i].center_hz + 1000000);
        lv_obj_t *btn = lv_obj_create(panel);
        lv_obj_set_size(btn, panel_w, btn_h);
        lv_obj_set_style_min_height(btn, 0, 0);
        lv_obj_set_style_min_width(btn, 0, 0);
        lv_obj_set_style_max_height(btn, btn_h, 0);
        lv_obj_set_style_bg_color(btn, active ? lv_color_hex(0x2A2A00) : lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, ft8_freq_preset_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)ft8_hz);

        char bstr[24];
        snprintf(bstr, sizeof(bstr), "%sm  %lu.%03lu",
                 bands[i].name,
                 (unsigned long)(ft8_hz / 1000000),
                 (unsigned long)((ft8_hz / 1000) % 1000));
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, bstr);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, active ? lv_color_hex(UI_COLOR_ACCENT_GOLD) : lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_center(lbl);
    }
}

static void ft8_freq_label_clicked_cb(lv_event_t *e)
{
    (void)e;
    ft8_freq_popup_open();
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
    lv_obj_set_style_text_color(s_lbl_mode, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_text_font(s_lbl_mode, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(s_lbl_mode, 0, 0);

    s_lbl_freq = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_freq, "Preset: --.--- MHz");
    lv_obj_set_style_text_color(s_lbl_freq, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_freq, &lv_font_montserrat_32, 0);
    lv_obj_set_pos(s_lbl_freq, 0, 80);

    // Tap to open a dropdown of conventional FT8 dial frequencies for the
    // bands this QMX supports. A separate transparent overlay (rather than
    // ext_click_area on the label itself) gives a generous touch target
    // spanning the full frequency text and extending well below it, since
    // ext_click_area only pads uniformly and the label's own width is just
    // the text's natural width.
    {
        // Created on `parent` (the screen), not s_left_pane, and reaching up
        // to screen y=0: ui.c's top-bar hit zones (Band/Mode/etc, 200px tall
        // at y=0) are also screen-level siblings and sit on top, so a touch
        // starting at the true top edge of the screen - e.g. "swipe down
        // from the top edge over Preset" - landed on the Band/Mode zones
        // instead of ever reaching a hit area confined to the label's own
        // y=80..170. Covering y=0..(MID_Y+170) here and re-foregrounding in
        // ft8_screen_view_show() lets this hit area win against those zones.
        lv_obj_t *hit = lv_obj_create(parent);
        lv_obj_set_size(hit, 345, MID_Y + 170);
        lv_obj_set_pos(hit, 0, 0);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hit, 0, 0);
        lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_HIDDEN);  // shown only while FT8 is active
        // PRESSED (not CLICKED) so a swipe/drag starting on this button
        // still opens the dropdown - a swipe-down gesture starting here
        // would otherwise be claimed as a drag/scroll and never fire
        // CLICKED, leaving the touch area "owned" by the button but
        // doing nothing.
        lv_obj_add_event_cb(hit, ft8_freq_label_clicked_cb, LV_EVENT_PRESSED, NULL);
        s_ft8_freq_hit = hit;
    }

    {
        const lv_font_t *font = &lv_font_montserrat_32;
        const lv_coord_t cell_w = 22;
        ui_clock_init(&s_clk_utc, s_left_pane, 0, 160, font, lv_color_hex(0xA0FFA0), cell_w);
        s_lbl_utc_suffix = lv_label_create(s_left_pane);
        lv_label_set_text(s_lbl_utc_suffix, " UTC");
        lv_obj_set_style_text_color(s_lbl_utc_suffix, lv_color_hex(0xA0FFA0), 0);
        lv_obj_set_style_text_font(s_lbl_utc_suffix, font, 0);
        lv_obj_set_pos(s_lbl_utc_suffix, 7 * cell_w, 160);
    }

    s_lbl_count = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_count, "Slot: -- s");
    lv_obj_set_style_text_color(s_lbl_count, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_lbl_count, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_count, 0, 240);

    // Tiny countdown bar to the right of "EVEN/ODD  N s", counting down
    // from full (start of slot) to empty (end of slot). Range is in ms so the
    // fast t_slotbar_cb tick can glide it smoothly; colour set in t_clock_cb.
    s_bar_slot = lv_bar_create(s_left_pane);
    lv_obj_set_size(s_bar_slot, 140, 8);
    lv_obj_set_pos(s_bar_slot, 140, 251);
    lv_obj_set_style_radius(s_bar_slot, 2, 0);
    lv_obj_set_style_bg_color(s_bar_slot, lv_color_hex(0x303044), 0);
    lv_obj_set_style_border_width(s_bar_slot, 0, 0);
    lv_bar_set_range(s_bar_slot, 0, 15000);
    lv_bar_set_value(s_bar_slot, 15000, LV_ANIM_OFF);

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
    lv_label_set_text(s_lbl_heard, "Active: 0");
    lv_obj_set_style_text_color(s_lbl_heard, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
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
    // Short tap = transmit the selected preset; long-press = edit presets.
    // SHORT_CLICKED (not CLICKED) so a long-press doesn't also fire a TX.
    lv_obj_add_event_cb(s_btn_cq, cq_btn_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btn_cq, cq_btn_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    s_cq_lbl = lv_label_create(s_btn_cq);
    lv_label_set_long_mode(s_cq_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_cq_lbl, 270);
    lv_obj_set_style_text_align(s_cq_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_cq_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_cq_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_cq_lbl);
    ft8_screen_view_refresh_cq_label();  // show the active CQ message

    // TX state indicator - hidden while idle; amber/armed or red/active,
    // tap to cancel/abort. See t_clock_cb (1 Hz refresh: state, colour,
    // countdown text) and tx_indicator_tap_cb (the tap action itself).
    s_lbl_tx = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_tx, "");
    lv_obj_set_style_text_font(s_lbl_tx, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_tx, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_label_set_long_mode(s_lbl_tx, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_tx, 288);
    lv_obj_set_pos(s_lbl_tx, 0, 482);
    lv_obj_add_flag(s_lbl_tx, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lbl_tx, tx_indicator_tap_cb, LV_EVENT_CLICKED, NULL);

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

    struct { const char *t; int x; int w; lv_text_align_t a; } cols[8] = {
        { "SL",      COL_SLOT_X,    COL_SLOT_W,    LV_TEXT_ALIGN_LEFT  },
        { "CALL",    COL_CALL_X,    COL_CALL_W,    LV_TEXT_ALIGN_LEFT  },
        { "MESSAGE", COL_TEXT_X,    COL_MSG_W,     LV_TEXT_ALIGN_LEFT  },
        { "COUNTRY", COL_COUNTRY_X, COL_COUNTRY_W, LV_TEXT_ALIGN_LEFT  },
        { "SNR",     COL_SNR_X,     COL_SNR_W,     LV_TEXT_ALIGN_RIGHT },
        { "KM",      COL_KM_X,      COL_KM_W,      LV_TEXT_ALIGN_RIGHT },
        { "BRG",     COL_BRG_X,     COL_BRG_W,     LV_TEXT_ALIGN_RIGHT },
        { "HRD",     COL_HEARD_X,   COL_HEARD_W,   LV_TEXT_ALIGN_RIGHT },
    };
    for (int i = 0; i < 8; i++) {
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
    s_t_slotbar = lv_timer_create(t_slotbar_cb,  50, NULL);

    // Ensure the freq touch overlay sits above later-added siblings
    // (UTC clock, slot bar, etc.) that partially overlap its hit area.
    if (s_ft8_freq_hit) lv_obj_move_foreground(s_ft8_freq_hit);

    ESP_LOGI(TAG, "FT8 view built (container %dx%d at y=%d, hidden)",
             MID_W, MID_H, MID_Y);
}

void ft8_screen_view_show(void)
{
    if (!s_container) return;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_refresh_pending = true;

    // ui.c creates transparent top-bar hit zones (Band/Mode/BW/Freq/Zoom,
    // each 200px tall at y=0) directly on the screen layer, as a sibling of
    // s_ft8_freq_hit (now also created on the screen, see
    // ft8_screen_view_init). Re-foreground it here so a touch starting at
    // the true top edge of the screen over "Preset: xx.xxx MHz" - previously
    // claimed by the Band/Mode hit zones - reaches the FT8 dropdown instead.
    // NOTE: do NOT foreground s_container itself - it's a near-full-screen
    // opaque pane (1280x644 at y=60) and would cover the left/right
    // edge-swipe grip handles (both vertically centered at y=360, i.e.
    // inside that span), which sit on scr as siblings created during
    // ui_init. This was the cause of both grips vanishing when booting
    // straight into FT8 mode (sticky mode, v0.16.0).
    if (s_ft8_freq_hit) {
        lv_obj_clear_flag(s_ft8_freq_hit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ft8_freq_hit);
    }

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
    if (s_ft8_freq_hit) lv_obj_add_flag(s_ft8_freq_hit, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "hide");
}

lv_obj_t *ft8_screen_view_get_container(void)
{
    return s_container;
}

void ft8_screen_view_request_refresh(void)
{
    s_refresh_pending = true;
}
