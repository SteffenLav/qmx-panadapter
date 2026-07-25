#include "ft8_screen_view.h"
#include "ui_theme.h"
#include "ft8_screen.h"
#include "ft8_cq_modal.h"
#include "ft8_filter_modal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cat/cat.h"
#include "ui.h"
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
#include "ft8_pileup.h"
#include "ft8_pileup_modal.h"
#include "ft8_status.h"
#include "ft8_test.h"   // ft8_op_mode_set() - FT8/FT4 sub-mode flag
#include "ft8_greylist.h"
#include "ft8_tx_modal.h"
#include "identity_config.h"
#include "adif/adif_log.h"
#include "adif_view_modal.h"
#include "net/webserver_ws.h"  // webserver_ws_set_paused() - pause spectrum stream off Panadapter

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
// Layout: SL | CALL | MESSAGE | CTY | SNR | DT | HZ | KM | BRG | HRD
// SL = slot parity (E blue / O amber). SL/CALL/MESSAGE are unchanged from
// the pre-v1.3.1 layout; the country column shrank from full entity names
// (157 px) to 3-letter codes (dxcc_lookup_alpha3), which - together with a
// tighter SNR (no " dB" suffix) - freed the width for two new columns
// (Roy KI0ER request): DT (slot-timing offset, seconds, band-consensus-
// relative so on-time stations read ~0.0) and HZ (the station's audio tone).
#define COL_SLOT_X      6
#define COL_SLOT_W      22
#define COL_CALL_X      46
#define COL_TEXT_X      184
#define COL_COUNTRY_X   479
#define COL_SNR_X       537
#define COL_DT_X        599
#define COL_HZ_X        669
#define COL_KM_X        739
#define COL_BRG_X       817
#define COL_HEARD_X     885
#define COL_RIGHT_EDGE  960
#define ROW_H           36

#define COL_CALL_W      (COL_TEXT_X    - COL_CALL_X    - 8)
#define COL_MSG_W       272
#define COL_COUNTRY_W   52
#define COL_SNR_W       56
#define COL_DT_W        64
#define COL_HZ_W        64
#define COL_KM_W        72
#define COL_BRG_W       62
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
static lv_style_t s_style_col_dt;       // DT (dim, right)
static lv_style_t s_style_col_hz;       // HZ (dim, right)
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
    lv_obj_t *l_dt;
    lv_obj_t *l_hz;
    lv_obj_t *l_km;
    lv_obj_t *l_brg;
    lv_obj_t *l_heard;
    // Dirty-tracking cache: skip lv_label_set_text when unchanged.
    char prev_call[16];
    char prev_msg[40];
    char prev_country[24];
    char prev_snr[12];
    char prev_dt[12];
    char prev_hz[12];
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
static lv_obj_t *s_btn_freq     = NULL;  // "Preset: XX.XXX MHz" button (visual only)
static lv_obj_t *s_btn_freq_hit = NULL;  // screen-level click target, see ft8_screen_view_init
static lv_obj_t *s_lbl_count    = NULL;
static lv_obj_t *s_bar_slot     = NULL;  // tiny countdown bar beside s_lbl_count
static lv_obj_t *s_lbl_heard    = NULL;
static lv_obj_t *s_btn_cq       = NULL;  // "Call CQ" - short tap TX, long-press edits presets
static lv_obj_t *s_cq_lbl       = NULL;  // label inside s_btn_cq (shows the active CQ message)
static lv_obj_t *s_lbl_tx       = NULL;  // TX state indicator: armed/active, tap to cancel/abort
static lv_obj_t *s_lbl_tx_pswr  = NULL;  // cyan live PWR/SWR line, shown only while ACTIVE (LVGL v9 has no in-label recolor)
static lv_obj_t *s_btn_override_resend = NULL;  // manual QSO override: re-send current msg
static lv_obj_t *s_lbl_resend          = NULL;  // label inside s_btn_override_resend (updated per-state)
static lv_obj_t *s_btn_override_rr73   = NULL;  // manual QSO override: force RR73
static lv_obj_t *s_btn_override_73     = NULL;  // manual QSO override: force 73
// CQ TX parity preference: -1=any slot, 0=EVEN only, 1=ODD only.
// Shown as two small toggle buttons between the slot countdown and "Heard: N".
// Tap once to lock; tap the active button again to revert to "any".
static lv_obj_t *s_btn_tx_even  = NULL;
static lv_obj_t *s_btn_tx_odd   = NULL;
static lv_obj_t *s_btn_filter   = NULL;  // "Filter" button — opens exclude-prefix modal
static lv_obj_t *s_btn_adif     = NULL;  // "ADIF-log" button - swaps to "Pileup" when ft8_pileup_count() > 0
static int        s_cq_parity   = -1;

static lv_obj_t *s_list         = NULL;
static row_widgets_t s_rows[MAX_ROWS];

static lv_timer_t *s_t_refresh  = NULL;
static lv_timer_t *s_t_clock    = NULL;
static lv_timer_t *s_t_slotbar  = NULL;  // fast tick for smooth countdown bar
static char         s_my_call[16] = {0};  /* operator callsign uppercased; refreshed by 1 Hz clock timer */
// Station currently mid-exchange in a CQ-run session (empty when none) -
// refreshed once per rebuild_list() and consumed by update_row() so the
// operator can tell, at a glance, who's actively being worked vs. who else
// answered the CQ and is still waiting their turn in the list below.
static char         s_qso_active_target[16] = {0};
static bool         s_greylist_en = false;   // cached once per rebuild (drives row colour)
static bool         s_distance_in_miles = false;  /* FT8 distance display unit; updated on settings change */
static lv_obj_t    *s_lbl_hdr_km = NULL;  /* "KM"/"MI" column header, re-labelled when the unit setting changes */

static volatile bool s_refresh_pending = false;
static volatile bool s_active = false;  // true while the FT8 screen is showing (vs. Panadapter)

// Touch-and-drag row selection.
//
// Movement, not hold time, decides scroll vs. select - a deliberate tap
// fires immediately on release, but a touch that strays into "this is a
// scroll" territory (ROW_SCROLL_CANCEL_PX) is barred from ever selecting:
//   - Finger moves > ROW_SCROLL_CANCEL_PX before the hold gate fires →
//       this touch is a scroll/swipe, permanently barred from selecting
//       for its whole lifetime (even if held past the gate afterwards) →
//       list scrolls, no modal on release
//   - Finger held ≥ ROW_PREVIEW_MS  → dim preview highlight shows which
//       row is targeted (no scroll lock yet — list still scrollable)
//   - Finger held ≥ ROW_HOLD_SELECT_MS → selection mode active: full
//       highlight, scroll locked, dragging shifts the highlight to other
//       rows; whatever's highlighted on release fires
//   - Finger released WITHOUT having scrolled and WITHOUT having crossed
//       ROW_HOLD_SELECT_MS (i.e. a quick tap, however brief) → fires
//       immediately on the row under the release point. This is what lets
//       a one-touch tap work at all - a 250ms-minimum hold before *every*
//       tap fired (even ones that never moved) used to leave a row dimly
//       highlighted with nothing happening on release.
//   - LV_EVENT_PRESS_LOST (LVGL scroll kick-in) → cancel, no action
//
// ROW_SCROLL_CANCEL_PX is generous (20 px) so slight hand-jitter in field
// conditions doesn't get misread as a scroll.
#define ROW_HOLD_SELECT_MS   250   // hold this long to enter selection mode
#define ROW_PREVIEW_MS        80   // show dim highlight this many ms into the hold
#define ROW_SCROLL_CANCEL_PX  20   // finger movement beyond this cancels selection

static int      s_row_hover         = -1;
static int      s_row_preview       = -1;  // dim pre-activation highlight
static uint32_t s_press_start_ms    = 0;
static bool     s_in_selection_mode = false;  // true while scroll is locked for drag-select
static lv_point_t s_press_start_pt  = {0, 0};
static bool     s_scroll_detected   = false;  // true if finger moved enough to be a scroll, ever

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

    INIT_COL(s_style_col_dt,      COL_DT_X,      COL_DT_W,      LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY);
    // HZ/KM/BRG values sit +10 px right of their (unchanged) header labels so
    // the numbers center under the right-aligned headings (operator request).
    INIT_COL(s_style_col_hz,      COL_HZ_X  + 10, COL_HZ_W,     LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY);
    INIT_COL(s_style_col_km,      COL_KM_X  + 10, COL_KM_W,     LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY);
    INIT_COL(s_style_col_brg,     COL_BRG_X + 10, COL_BRG_W,    LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY);
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

// Dim "about to select" highlight — shown between ROW_PREVIEW_MS and
// ROW_HOLD_SELECT_MS so the user sees which row is targeted before the
// hold gate fires.  List is still scrollable while preview is active.
static void row_clear_preview(void)
{
    if (s_row_preview >= 0 && s_row_preview < MAX_ROWS && s_rows[s_row_preview].row)
        lv_obj_set_style_bg_opa(s_rows[s_row_preview].row, LV_OPA_0, 0);
    s_row_preview = -1;
}

static void row_set_preview(int idx)
{
    if (idx == s_row_preview) return;
    row_clear_preview();
    if (idx >= 0 && idx < MAX_ROWS
        && s_rows[idx].row
        && !lv_obj_has_flag(s_rows[idx].row, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_style_bg_color(s_rows[idx].row, lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_set_style_bg_opa(s_rows[idx].row, LV_OPA_30, 0);
        s_row_preview = idx;
    }
}

// Set / clear the full selection highlight on a row.  Always clears the
// previous row and any preview highlight first.
static void row_set_hover(int new_idx)
{
    if (new_idx == s_row_hover) return;
    row_clear_preview();
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
        s_row_hover = -1;
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
// Index of the row last passed to row_activate() - lets the TX confirm
// modal's up/down nudge buttons (ft8_screen_view_nudge_confirm()) know what
// to move relative to. -1 until the first activation.
static int s_confirmed_row_idx = -1;

// Grey-list clear dialog: tapping a grey-listed row offers to clear the
// station instead of opening the TX modal (the auto pickers skip it anyway;
// clearing it is the one action that makes sense on such a row).
static lv_obj_t *s_grey_modal = NULL;
static char      s_grey_call[12];

static void grey_modal_close(void)
{
    if (s_grey_modal) { lv_obj_delete(s_grey_modal); s_grey_modal = NULL; }
}

static void grey_clear_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_grey_call[0]) ft8_greylist_clear(s_grey_call);
    grey_modal_close();
    s_refresh_pending = true;   // recolour the row on the next refresh
}

static void grey_cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    grey_modal_close();
}

static void grey_modal_show(const char *call)
{
    grey_modal_close();
    snprintf(s_grey_call, sizeof(s_grey_call), "%s", call);

    s_grey_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_grey_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_grey_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_grey_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_grey_modal, 0, 0);
    lv_obj_clear_flag(s_grey_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(s_grey_modal);
    lv_obj_set_size(panel, 640, 240);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text_fmt(lbl, "%s is grey-listed\n(no response to repeated calls)", call);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 8);

    struct { const char *t; uint32_t col; lv_event_cb_t cb; } btns[2] = {
        { "Clear from grey-list", 0x2e8b3a, grey_clear_btn_cb  },
        { "Cancel",               0x962020, grey_cancel_btn_cb },
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_btn_create(panel);
        lv_obj_set_size(b, 300, 64);
        lv_obj_align(b, i == 0 ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0, -4);
        lv_obj_set_style_bg_color(b, lv_color_hex(btns[i].col), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, btns[i].t);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_center(l);
    }
}

static void row_activate(int idx)
{
    if (idx < 0 || idx >= MAX_ROWS) return;
    row_widgets_t *r = &s_rows[idx];
    if (!r->row || !r->l_call || lv_obj_has_flag(r->row, LV_OBJ_FLAG_HIDDEN)) return;

    const char *call = lv_label_get_text(r->l_call);
    if (!call || !call[0]) return;

    // A grey-listed station's row offers "Clear from grey-list" instead of
    // the TX modal (the auto pickers skip it; a manual pounce needs the
    // clear first, which is one tap away here).
    {
        qmx_settings_t gq;
        settings_load_all(&gq);
        if (gq.greylist_en && ft8_greylist_contains(call)) {
            grey_modal_show(call);
            return;
        }
    }

    s_confirmed_row_idx = idx;

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
    // Pick a clear audio slot for our reply — not the CQ station's own tone
    // (that's theirs). ft8_find_clear_tone_hz() scans the heard-station table
    // and returns the nearest unoccupied 50 Hz slot to 1500 Hz.
    int reply_freq_hz = ft8_find_clear_tone_hz();

    ESP_LOGI(TAG, "row activate: reply to %s (their_freq=%d Hz -> our_freq=%d Hz, last_utc=%lld)",
             match->call, (int)match->last_freq, reply_freq_hz, (long long)match->last_utc);

    // Intelligent Transmit: build the correct NEXT message for this row (grid /
    // report / R-report / RR73 / 73) from what the station last sent us, so the
    // Transmit button steps the QSO forward instead of always resending our
    // grid. Auto Pounce (the full auto-sequencer) is still offered for a fresh
    // CQ; the modal derives that from the request (see ft8_tx_modal_show).
    ft8_tx_request_t req;
    char err[64];
    if (ft8_qso_build_manual_reply(match, reply_freq_hz, &req, NULL, err, sizeof(err))) {
        ft8_tx_modal_show(&req);
    } else {
        ESP_LOGW(TAG, "build manual reply to %s failed: %s", match->call, err);
        identity_config_modal_show();
    }
}

void ft8_screen_view_nudge_confirm(int delta)
{
    if (s_confirmed_row_idx < 0) return;
    int idx = s_confirmed_row_idx + delta;
    if (idx < 0 || idx >= MAX_ROWS || !s_rows[idx].row
        || lv_obj_has_flag(s_rows[idx].row, LV_OBJ_FLAG_HIDDEN)) {
        ui_toast(delta > 0 ? "No row below" : "No row above");
        return;
    }
    row_activate(idx);
}

// Touch-and-drag row selection handler registered on every row for
// PRESSED / PRESSING / RELEASED / PRESS_LOST:
//
//   PRESSED     - finger touches down: record timestamp + start point, no
//                 highlight yet.
//
//   PRESSING    - fires continuously while held. If the finger ever moves more
//                 than ROW_SCROLL_CANCEL_PX from its start point (before
//                 selection mode locked scroll), this touch is a scroll/swipe
//                 and is permanently barred from selecting, full stop.
//                 Otherwise, once held ≥ ROW_HOLD_SELECT_MS, selection mode
//                 activates: the row under the fingertip highlights, list
//                 scroll locks, and dragging moves the highlight to other rows.
//
//   RELEASED    - finger lifts. A scroll/swipe does nothing (list just
//                 scrolled). A held-and-dragged touch (selection mode was
//                 active) fires whatever row is currently highlighted. A
//                 plain tap - never scrolled, never held long enough to lock
//                 scroll - fires immediately on the row under the release
//                 point, so a quick single tap reliably opens the confirm
//                 modal instead of silently doing nothing.
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
            // If the finger has moved more than ROW_SCROLL_CANCEL_PX before
            // the hold gate fires, this is a scroll/swipe — cancel any preview
            // and permanently bar selection for this touch.
            int32_t dx = pt.x - s_press_start_pt.x;
            int32_t dy = pt.y - s_press_start_pt.y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > ROW_SCROLL_CANCEL_PX || dy > ROW_SCROLL_CANCEL_PX) {
                s_scroll_detected = true;
                row_clear_preview();
            }
        }

        uint32_t held_ms = lv_tick_get() - s_press_start_ms;
        if (!s_scroll_detected) {
            int cur_row = screen_y_to_row(pt.y);
            if (held_ms >= ROW_HOLD_SELECT_MS) {
                // Full selection mode: lock scroll so dragging moves the highlight.
                if (!s_in_selection_mode) {
                    s_in_selection_mode = true;
                    if (s_list) lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
                }
                if (cur_row != s_row_hover)
                    row_set_hover(cur_row);
            } else if (held_ms >= ROW_PREVIEW_MS) {
                // Dim preview: user sees which row they're targeting before
                // the hold gate fires.  List is still scrollable at this point.
                row_set_preview(cur_row);
            }
        }

    } else if (code == LV_EVENT_RELEASED) {
        int confirm = s_row_hover;
        row_clear_preview();
        row_set_hover(-1);
        // Restore list scroll before anything else.
        if (s_in_selection_mode && s_list)
            lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
        bool was_selection_mode = s_in_selection_mode;
        s_in_selection_mode = false;

        if (s_scroll_detected) {
            // Moved enough to be a scroll/swipe - let the list scroll, no
            // selection (and no longer a dimly-highlighted row left behind:
            // row_clear_preview() above used to be skipped on this path,
            // which is what made a brief touch look "stuck lit" with
            // nothing happening on release).
        } else if (was_selection_mode) {
            // Held long enough to lock scroll, optionally dragged across
            // rows - whatever's currently highlighted fires.
            if (confirm >= 0) row_activate(confirm);
        } else {
            // A tap that never crossed the hold-to-drag gate, but also
            // never moved far enough to be a scroll - fire immediately on
            // release instead of silently doing nothing. Previously a
            // deliberate single tap needed a 250ms hold to do anything;
            // now any release on a row that wasn't a scroll opens the
            // confirm modal right away, while a held-and-dragged touch
            // still gets to reselect a different row before releasing.
            lv_indev_t *indev = lv_indev_get_act();
            lv_point_t pt = s_press_start_pt;
            if (indev) lv_indev_get_point(indev, &pt);
            int row = screen_y_to_row(pt.y);
            if (row >= 0) row_activate(row);
        }

    } else if (code == LV_EVENT_PRESS_LOST) {
        row_set_hover(-1);
        row_clear_preview();
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
    r->l_dt      = make_label_styled(r->row, &s_style_col_dt);
    r->l_hz      = make_label_styled(r->row, &s_style_col_hz);
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

    const char *country = dxcc_lookup_alpha3(src->call);
    if (!country) country = "--";

    int snr = (int)src->last_snr_db;
    char b_snr[12], b_dt[12], b_hz[12], b_km[12], b_brg[12], b_heard[12];
    snprintf(b_snr,   sizeof(b_snr),   "%+d", snr);
    snprintf(b_heard, sizeof(b_heard), "%u",  (unsigned)src->heard_count);

    // DT: the station's slot-timing offset in seconds, relative to the band
    // consensus - the consensus carries our common RX audio latency, so
    // subtracting it makes an on-time station read ~0.0 (the WSJT-X-style
    // number Roy KI0ER asked for) and an off-time one show its true offset.
    // Falls back to the raw value while no consensus exists yet (first slot).
    {
        int consensus = 0;
        (void)ft8_get_last_timing_ms(&consensus);
        snprintf(b_dt, sizeof(b_dt), "%+.1f",
                 ((int)src->last_dt_ms - consensus) / 1000.0f);
    }
    snprintf(b_hz, sizeof(b_hz), "%d", (int)src->last_freq);

    if (s_user_loc_valid && src->last_grid[0]) {
        double rlat = 0.0, rlon = 0.0;
        if (maidenhead_to_latlon(src->last_grid, &rlat, &rlon)) {
            double km  = haversine_km(s_user_lat, s_user_lon, rlat, rlon);
            double brg = bearing_deg (s_user_lat, s_user_lon, rlat, rlon);
            if (s_distance_in_miles) {
                double miles = km * 0.621371;
                snprintf(b_km,  sizeof(b_km),  "%d",   (int)(miles + 0.5));
            } else {
                snprintf(b_km,  sizeof(b_km),  "%d",   (int)(km + 0.5));
            }
            snprintf(b_brg, sizeof(b_brg), "%d°", (int)(brg + 0.5));
        } else {
            snprintf(b_km,  sizeof(b_km),  "--");
            snprintf(b_brg, sizeof(b_brg), "--");
        }
    } else {
        snprintf(b_km,  sizeof(b_km),  "--");
        snprintf(b_brg, sizeof(b_brg), "--");
    }

    /* E (blue) / O (amber) slot parity indicator, on the ACTIVE protocol's
     * grid (7.5 s FT4 / 15 s FT8). last_utc is whole seconds (boundary_ms/1000),
     * so an odd FT4 slot's .5 s got truncated - round to the nearest slot index
     * (+period/2 before the divide) to recover it. Hardcoding /15 flipped parity
     * only every OTHER FT4 slot (the "E E O O" bug) and disagreed with the TX
     * engine, which already uses ft8_op_mode_slot_ms(). */
    {
        int per_ms = ft8_op_mode_slot_ms();
        int64_t sidx = ((int64_t)src->last_utc * 1000 + per_ms / 2) / per_ms;
        int8_t parity = (sidx % 2) == 0 ? 1 : 0; /* 1=even 0=odd */
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
    set_text_if_changed(r->l_dt,      r->prev_dt,      sizeof(r->prev_dt),      b_dt);
    set_text_if_changed(r->l_hz,      r->prev_hz,      sizeof(r->prev_hz),      b_hz);
    set_text_if_changed(r->l_km,      r->prev_km,      sizeof(r->prev_km),      b_km);
    set_text_if_changed(r->l_brg,     r->prev_brg,     sizeof(r->prev_brg),     b_brg);
    set_text_if_changed(r->l_heard,   r->prev_heard,   sizeof(r->prev_heard),   b_heard);

    /* Colour scheme: AMBER=actively working now, RED=own call heard,
       GREY=worked before, GREEN=CQ, WHITE=other */
    {
        int8_t col = 0; /* other / white */
        if (s_qso_active_target[0] && strcmp(src->call, s_qso_active_target) == 0) {
            col = 4; /* the one station we're mid-exchange with -> amber (highest priority) */
        } else if (s_my_call[0] && strstr(src->last_text, s_my_call)) {
            col = 2; /* own call in message -> red */
        } else if (s_greylist_en && ft8_greylist_contains(src->call)) {
            col = 5; /* grey-listed (repeated failed pounces) -> violet */
        } else if (adif_log_contains_call_on_band(src->call, cat_get_frequency())) {
            col = 3; /* worked before on THIS band -> dim grey */
        } else if (strncmp(src->last_text, "CQ ", 3) == 0) {
            col = 1; /* CQ -> green */
        }
        if (col != r->prev_color) {
            r->prev_color = col;
            // Own-call and active-exchange rows are inverted (fill + white text)
            // instead of plain colour-on-black: field feedback (Ken KF0AYY,
            // comparing against DXFT8) found plain red text on the dark
            // background hard to read at a glance. Every other colour stays
            // plain text on the row's normal black background.
            bool inverted = (col == 2 || col == 4);
            lv_color_t fill = (col == 4) ? lv_color_hex(0xFFA040)   /* working now: amber */
                                          : lv_palette_main(LV_PALETTE_RED);
            lv_color_t c = (col == 4) ? lv_color_black()            /* amber fill needs dark text */
                         : (col == 2) ? lv_color_white()            /* red fill needs light text */
                         : (col == 3) ? lv_color_hex(0x808080)      /* worked: dim grey */
                         : (col == 5) ? lv_color_hex(0x9070C8)      /* grey-listed: violet */
                         : (col == 1) ? lv_palette_main(LV_PALETTE_GREEN)
                         :              lv_color_white();
            lv_obj_set_style_text_color(r->l_call, c, 0);
            lv_obj_set_style_text_color(r->l_msg,  c, 0);
            lv_obj_set_style_bg_color(r->l_call, fill, 0);
            lv_obj_set_style_bg_color(r->l_msg,  fill, 0);
            lv_obj_set_style_bg_opa(r->l_call, inverted ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_opa(r->l_msg,  inverted ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
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

// ft8_filter_match() is the shared filter matcher declared in ft8_qso.h
// (matches a decoded message against the CQ-run include/exclude terms). It was
// promoted out of this file when the robot landed so ft8_qso.c, ft8_robot.c and
// this view all share one implementation - use that one here.

static void rebuild_list(void)
{
    static ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    qsort(snap, n, sizeof(ft8_call_t), cmp_cq_then_snr);

    qmx_settings_t qs;
    settings_load_all(&qs);
    if (s_distance_in_miles != qs.distance_in_miles && s_lbl_hdr_km) {
        lv_label_set_text(s_lbl_hdr_km, qs.distance_in_miles ? "MI" : "KM");
    }
    s_distance_in_miles = qs.distance_in_miles;

    // Keep the own-call cache (used for the red own-call row highlight) and
    // grid-derived location live, not just refreshed on FT8 screen entry
    // (ft8_screen_view_show()) - saving the callsign/grid via the CQ modal
    // while already sitting on the FT8 screen never re-triggers show(), so
    // the highlight would otherwise stay dead until a mode bounce.
    if (qs.my_callsign[0]) {
        size_t ci;
        for (ci = 0; ci < sizeof(s_my_call) - 1 && qs.my_callsign[ci]; ci++)
            s_my_call[ci] = (qs.my_callsign[ci] >= 'a' && qs.my_callsign[ci] <= 'z')
                           ? (char)(qs.my_callsign[ci] - 32) : qs.my_callsign[ci];
        s_my_call[ci] = 0;
    }
    s_user_loc_valid = false;
    if (qs.my_grid[0]) {
        s_user_loc_valid = maidenhead_to_latlon(qs.my_grid, &s_user_lat, &s_user_lon);
    }

    // While we're running our own CQ, hide other stations' CQ rows so replies
    // addressed to us aren't buried under unrelated CQ traffic. The "exclude
    // plain CQ" filter applies the same hide unconditionally.
    bool hide_cq = ft8_qso_cq_filter_active() || qs.ft8_filters.excl_plain_cq;

    // Also hide stations whose last decode landed on OUR TX parity while the
    // CQ run is active: we transmit over every slot we could hear them in, so
    // they can't be worked right now - and since the parity-aging pause
    // (06c8b9f) keeps their rows alive for the whole run (120 s window),
    // they'd otherwise sit frozen in the list for minutes. Display-only: the
    // table keeps them (the clash scan still sees their tones) and they
    // reappear the moment the run ends. Operator request 2026-07-16.
    bool cq_tx_even = false;
    bool cq_hide_our_parity = ft8_qso_cq_filter_active() &&
                              ft8_tx_get_parity_lock(&cq_tx_even);
    int  cq_per_ms = ft8_op_mode_slot_ms();

    // Who (if anyone) we're actively mid-exchange with right now, for the
    // "currently working" row highlight - distinct from the broader own-call
    // red highlight, which covers every station that's answered, not just
    // the one we're presently giving a report / waiting on a roger from.
    // Covers both a machine QSO's target and a manually-stepped partner
    // (ft8_qso_note_manual_target), so hand-run exchanges look the same.
    ft8_qso_get_working_target(s_qso_active_target, sizeof(s_qso_active_target));

    {
        qmx_settings_t gq;
        settings_load_all(&gq);
        s_greylist_en = gq.greylist_en;
    }

    int row = 0;
    for (int i = 0; i < n && row < MAX_ROWS; i++) {
        if (hide_cq && strncmp(snap[i].last_text, "CQ ", 3) == 0) continue;
        if (cq_hide_our_parity) {
            // Same nearest-slot parity rounding as the per-row E/O indicator.
            int64_t sidx = ((int64_t)snap[i].last_utc * 1000 + cq_per_ms / 2) / cq_per_ms;
            if (((sidx % 2) == 0) == cq_tx_even) continue;
        }
        if (qs.ft8_filters.incl_cq_only && strncmp(snap[i].last_text, "CQ ", 3) != 0) continue;
        if (!ft8_filter_match(snap[i].last_text, &qs.ft8_filters)) continue;
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

// Smoothly drive the slot countdown bar. Runs fast (~50 ms) and uses
// sub-second time so the bar glides to 0 instead of snapping per second.
// Period is read each tick from ft8_op_mode_slot_ms() (15000 FT8 / 7500 FT4)
// so this never drifts out of sync with the slot engine's actual period -
// the bar's range (lv_bar_set_range) is kept in step alongside it below.
// Also owns the bar colour: red while a TX burst is ACTIVE, otherwise the
// EVEN/ODD slot colour, computed on the ACTIVE protocol's grid (7.5 s FT4 /
// 15 s FT8) so it matches the slot engine and TX parity.
static void t_slotbar_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_bar_slot) return;
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int period_ms = ft8_op_mode_slot_ms();
    lv_bar_set_range(s_bar_slot, 0, period_ms);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int slot_ms = (int)(now_ms % period_ms);
    int remain_ms = period_ms - slot_ms;
    if (remain_ms < 0) remain_ms = 0;
    lv_bar_set_value(s_bar_slot, remain_ms, LV_ANIM_OFF);

    lv_color_t col;
    if (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_ACTIVE) {
        col = lv_palette_main(LV_PALETTE_RED);
    } else {
        // Slot parity (EVEN/ODD) on the ACTIVE protocol's grid (7.5 s FT4 /
        // 15 s FT8) - must match the slot engine and TX parity (ft8_tx also
        // uses ft8_op_mode_slot_ms), or the colour flips every OTHER FT4 slot
        // (the "E E O O" bug the countdown showed).
        bool is_even = ((now_ms / period_ms) % 2) == 0;
        col = is_even ? lv_color_hex(UI_COLOR_PRIMARY_BORDER) : lv_color_hex(0xE09040);
    }
    lv_obj_set_style_bg_color(s_bar_slot, col, LV_PART_INDICATOR);
}

static void t_clock_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;

    // Respawn watchdog: the FT8 view is visible (we're past the guard above),
    // but a lingering ft8_task from a fast Panadapter<->FT8 toggle can exit
    // on its own well after the toggle handler already decided "one's alive,
    // don't spawn a replacement" (see ft8_self_test()'s comment). Nothing else
    // ever notices that exit, so the view is left showing a stale status
    // forever with no task behind it. Checking here, once a second, closes
    // that gap regardless of the exact timing that caused it.
    if (!ft8_task_is_alive()) {
        ESP_LOGW(TAG, "t_clock_cb: FT8 view visible but no ft8_task alive - respawning");
        ft8_self_test();
    }

    if (s_lbl_count) {
        // FT4's 7.5 s period isn't a whole number of seconds, so derive the
        // countdown from the millisecond remainder (same math as the fast bar
        // in t_slotbar_cb) rather than naively truncating period_ms/1000 -
        // that would lose the half-second and drift the displayed number off
        // the true grid over time. Rounded up to the nearest second for
        // display. Parity is on the active protocol's grid (see t_slotbar_cb).
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        int period_ms = ft8_op_mode_slot_ms();
        int64_t now_ms = (int64_t)tv_now.tv_sec * 1000 + tv_now.tv_usec / 1000;
        int remain_ms = period_ms - (int)(now_ms % period_ms);
        int remain = (remain_ms + 999) / 1000;
        if (remain < 0) remain = 0;          // clamp: bounds the snprintf width below
        if (remain > 15) remain = 15;
        bool is_even = ((now_ms / period_ms) % 2) == 0;
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
    if (s_btn_freq) {
        uint32_t hz = cat_get_frequency();
        char b[40];
        uint32_t mhz = hz / 1000000;
        uint32_t khz_frac = (hz / 1000) % 1000;
        snprintf(b, sizeof(b), "Preset: %lu.%03lu MHz",
                 (unsigned long)mhz, (unsigned long)khz_frac);
        lv_obj_t *lbl = lv_obj_get_child(s_btn_freq, 0);
        if (lbl) lv_label_set_text(lbl, b);
    }

    // ADIF-log/Pileup dual-purpose button: swap label + colour to make an
    // active pileup impossible to miss. adif_or_pileup_btn_cb() re-checks the
    // count itself at tap time, so this is purely cosmetic - it can never
    // cause a tap to open the wrong modal.
    if (s_btn_adif) {
        static bool s_was_pileup = false;
        bool has_pileup = ft8_pileup_count() > 0;
        lv_obj_t *lbl = lv_obj_get_child(s_btn_adif, 0);
        if (lbl) lv_label_set_text(lbl, has_pileup ? "Pileup" : "ADIF-log");
        lv_obj_set_style_bg_color(s_btn_adif,
            lv_color_hex(has_pileup ? UI_COLOR_PRIMARY : 0x163d5e), 0);
        // When the button first flips to "Pileup", teach the (otherwise
        // hidden) hold-for-log gesture once, so the ADIF log never feels lost.
        if (has_pileup && !s_was_pileup)
            ui_toast("Pileup active - hold this button for the ADIF log");
        s_was_pileup = has_pileup;
    }

    // Status / TX / QSO indicator — always visible.
    // Priority: ACTIVE (red) > ARMED (amber) > QSO state (cyan) > ft8_status (dim white).
    if (s_lbl_tx) {
        char tx_text[32];
        int  secs_until = 0;
        ft8_tx_state_t tx_st = ft8_tx_get_status(tx_text, sizeof(tx_text), &secs_until);
        ft8_qso_state_t qso_st = ft8_qso_get_state();
        char b[128];

        // Live PWR/SWR cyan line is shown ONLY while ACTIVE; hide by default so
        // it vanishes the instant TX ends (QSO finish or cancel), by request.
        if (s_lbl_tx_pswr) lv_obj_add_flag(s_lbl_tx_pswr, LV_OBJ_FLAG_HIDDEN);

        bool clash = (tx_st != FT8_TX_IDLE) && ft8_tx_is_clashing();

        if (tx_st == FT8_TX_ACTIVE) {
            // Red: transmitting right now (tap to abort). Each logical chunk
            // gets its own explicit line - "TAP TO ABORT" is always the last
            // line on its own, never sharing a line (and so never an
            // auto-wrap break) with the message text above it. The message
            // itself is still free to wrap across multiple lines if it's too
            // long for the label width - only the boundary BETWEEN chunks is
            // fixed, not the wrapping within a chunk.
            if (clash)
                snprintf(b, sizeof(b), "Transmitting:\n%s\nTAP TO ABORT\n⚠ FREQ BUSY", tx_text);
            else
                snprintf(b, sizeof(b), "Transmitting:\n%s\nTAP TO ABORT", tx_text);
            lv_label_set_text(s_lbl_tx, b);
            lv_obj_set_style_text_color(s_lbl_tx, lv_palette_main(LV_PALETTE_RED), 0);

            // Live PWR/SWR on a separate cyan label below. Populated by
            // ft8_tx.c's mid-burst async query (~2 s into the burst); before
            // that it carries the previous burst's reading, or nothing on the
            // very first burst. Aligned under s_lbl_tx each tick so it follows
            // the (variable-height) TRANSMITTING text.
            float pw = -1.0f, sw = -1.0f;
            (void)ft8_tx_get_last_power_swr(&pw, &sw);
            if (s_lbl_tx_pswr && pw >= 0.0f && sw >= 0.0f) {
                char pswr[40];
                snprintf(pswr, sizeof(pswr), "PWR %.1fW  SWR %.2f", (double)pw, (double)sw);
                lv_label_set_text(s_lbl_tx_pswr, pswr);
                lv_obj_align_to(s_lbl_tx_pswr, s_lbl_tx, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
                lv_obj_clear_flag(s_lbl_tx_pswr, LV_OBJ_FLAG_HIDDEN);
            }

        } else if (tx_st == FT8_TX_ARMED) {
            // Amber (no clash) or red-orange (clash): burst scheduled (tap to cancel)
            // Parity of the slot we're about to fire on, for display only.
            // Computed with the same ms-precision/period-aware math as the
            // engine's own firing decision (ft8_tx.c) - the old whole-second
            // "/15" here had the same FT4 truncation bug as the parity check
            // that used to make CQ fire in TX-TX-RX-RX pairs instead of
            // alternating every slot, just for this cosmetic word instead of
            // the actual TX decision.
            struct timeval tv_armed;
            gettimeofday(&tv_armed, NULL);
            int64_t fire_ms = (int64_t)tv_armed.tv_sec * 1000 + tv_armed.tv_usec / 1000
                             + (int64_t)secs_until * 1000;
            bool tx_even = ((fire_ms / ft8_op_mode_slot_ms()) % 2) == 0;
            // Same one-chunk-per-line rule as the ACTIVE branch above -
            // "TAP TO CANCEL" is always its own trailing line.
            if (clash)
                snprintf(b, sizeof(b), "TX armed:\n%s\n-> %s slot, ~%ds\nTAP TO CANCEL\n⚠ FREQ BUSY",
                         tx_text, tx_even ? "EVEN" : "ODD", secs_until);
            else
                snprintf(b, sizeof(b), "TX armed:\n%s\n-> %s slot, ~%ds\nTAP TO CANCEL",
                         tx_text, tx_even ? "EVEN" : "ODD", secs_until);
            lv_label_set_text(s_lbl_tx, b);
            lv_obj_set_style_text_color(s_lbl_tx,
                clash ? lv_color_hex(0xFF4010) : lv_color_hex(0xFFA040), 0);

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
            snprintf(b, sizeof(b), "QSO %s: timeout\nTAP TO CLEAR", target);
            lv_label_set_text(s_lbl_tx, b);
            lv_obj_set_style_text_color(s_lbl_tx, lv_color_hex(0xFF6020), 0);

        } else {
            // Not transmitting and no QSO state to show: plain ft8_status
            // passthrough (RX state, decode count, etc.). The TX power/SWR is
            // shown ONLY on the live TRANSMITTING line above and is gone the
            // moment TX ends (QSO finish or cancel) - no lingering "Last TX"
            // readout, by request.
            char status[96];
            ft8_status_get(status, sizeof(status));
            lv_label_set_text(s_lbl_tx, status[0] ? status : "Idle");
            lv_obj_set_style_text_color(s_lbl_tx, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        }
        lv_obj_clear_flag(s_lbl_tx, LV_OBJ_FLAG_HIDDEN);

        // Show manual override buttons only during active QSO exchange.
        if (s_btn_override_resend) {
            bool show = (qso_st == FT8_QSO_WAIT_RPT ||
                         qso_st == FT8_QSO_WAIT_ROGER ||
                         qso_st == FT8_QSO_WAIT_RR73);
            if (show) {
                // Update "Re-send" label to show what will actually be re-sent
                // e.g. "Re-send\nJO45" in WAIT_RPT, "Re-send\n-07" in WAIT_ROGER
                if (s_lbl_resend) {
                    char extra[16] = {0};
                    ft8_qso_get_cur_extra(extra, sizeof(extra));
                    if (extra[0]) {
                        lv_label_set_text_fmt(s_lbl_resend, "Re-send\n%s", extra);
                        lv_obj_set_style_text_font(s_lbl_resend, &lv_font_montserrat_20, 0);
                    } else {
                        lv_label_set_text(s_lbl_resend, "Re-send");
                        lv_obj_set_style_text_font(s_lbl_resend, &lv_font_montserrat_24, 0);
                    }
                    lv_obj_center(s_lbl_resend);
                }
                lv_obj_clear_flag(s_btn_override_resend, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_btn_override_rr73,   LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_btn_override_73,      LV_OBJ_FLAG_HIDDEN);
            } else {
                // Reset to plain "Re-send" so if no extra is available on next show it
                // doesn't display stale content from the previous exchange.
                if (s_lbl_resend) {
                    lv_label_set_text(s_lbl_resend, "Re-send");
                    lv_obj_set_style_text_font(s_lbl_resend, &lv_font_montserrat_24, 0);
                    lv_obj_center(s_lbl_resend);
                }
                lv_obj_add_flag(s_btn_override_resend, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_btn_override_rr73,   LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_btn_override_73,      LV_OBJ_FLAG_HIDDEN);
            }
        }
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
// Forward declaration — defined later in this file

static void filter_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Filter button tapped");
    ft8_filter_modal_show();  // opens the exclude-prefix + future filters modal
}

static void override_resend_cb(lv_event_t *e)
{
    (void)e;
    char err[64];
    if (!ft8_qso_override_next(FT8_TX_KIND_REPLY, err, sizeof(err)))
        ESP_LOGW(TAG, "Re-send override failed: %s", err);
}

static void override_rr73_cb(lv_event_t *e)
{
    (void)e;
    char err[64];
    if (!ft8_qso_override_next(FT8_TX_KIND_ROGER_RPT, err, sizeof(err)))
        ESP_LOGW(TAG, "RR73 override failed: %s", err);
}

static void override_73_cb(lv_event_t *e)
{
    (void)e;
    char err[64];
    if (!ft8_qso_override_next(FT8_TX_KIND_73, err, sizeof(err)))
        ESP_LOGW(TAG, "73 override failed: %s", err);
}

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

// Dual-purpose button: shows "ADIF-log" normally, swaps to "Pileup" whenever
// ft8_pileup_count() > 0 (see t_clock_cb's 1 Hz label/colour refresh below) -
// dispatches by the SAME check at tap time rather than trusting the label
// text, so a pileup that clears in the instant between the last refresh and
// this tap still opens the right one.
static void adif_or_pileup_btn_cb(lv_event_t *e)
{
    (void)e;
    if (ft8_pileup_count() > 0) {
        ESP_LOGI(TAG, "Pileup/ADIF-log button short-tapped -> pileup viewer");
        ft8_pileup_modal_show();
    } else {
        ESP_LOGI(TAG, "Pileup/ADIF-log button short-tapped -> ADIF log viewer");
        adif_view_modal_show();
    }
}

// Long-press ALWAYS opens the ADIF log, even while the button is showing
// "Pileup". Without this a non-empty pile-up hid the on-device log entirely
// (Roy KI0ER: "kept seeing Pileup, could not get back to ADIF-log") - the
// short-press dispatches to whatever is timely, the hold is the guaranteed
// path to the log.
static void adif_long_press_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Pileup/ADIF-log button long-pressed -> ADIF log viewer (always)");
    adif_view_modal_show();
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

// Conventional FT4 dial frequencies (USB). FT4 shares the QMX's USB/DiGi data
// mode with FT8 (only the slot timing/protocol differ), so picking one of
// these sets the dial + flips the Tab5 to FT4 but sends NO CAT mode change.
// 160 m and 60 m have no standard FT4 frequency and are omitted. Note 40 m is
// 7047.5 kHz - the value is exact; only the "MHz.kkk" preset label rounds it.
// Wrapped in FT4_MODE_DISABLED (ft8_test.h) along with its only use site
// below - kept, not deleted, pending a final decision on FT4's fate.
#if !FT4_MODE_DISABLED
static const ft8_band_freq_t FT4_BAND_FREQS[] = {
    { "80",  3575000  },
    { "40",  7047500  },
    { "30",  10140000 },
    { "20",  14080000 },
    { "17",  18104000 },
    { "15",  21140000 },
    { "12",  24919000 },
    { "10",  28180000 },
    { "6",   50318000 },
};
#define N_FT4_BAND_FREQS (sizeof(FT4_BAND_FREQS) / sizeof(FT4_BAND_FREQS[0]))
#endif

// Cyan accent used for everything FT4: the dropdown's FT4 column header and the
// "MODE: FT4" label, so the two can never drift apart. FT8 keeps the gold
// UI_COLOR_ACCENT_GOLD.
#define FT4_ACCENT_HEX 0x00B4FF

static lv_obj_t *s_ft8_freq_popup = NULL;

static void ft8_freq_popup_close(void)
{
    if (s_ft8_freq_popup) { lv_obj_delete(s_ft8_freq_popup); s_ft8_freq_popup = NULL; }
}

// Apply a tapped preset: retune the radio, set the FT8/FT4 sub-mode, and
// update the two on-screen labels. FT4 and FT8 use the SAME radio data mode
// (USB/DiGi), so this never sends a CAT mode change - only the Tab5-side
// op-mode flag and the "MODE:" label move. (The 7.5 s FT4 slot engine is a
// pending follow-up; for now FT4 retunes + relabels but still decodes/TXes on
// FT8 timing - see ft8_op_mode_set() and the TODO in ft8_test.c.)
static void apply_freq_preset(uint32_t freq_hz, bool ft4)
{
    ft8_freq_popup_close();
    // Force bypasses the 200 ms rate-limiter so a deliberate preset tap always
    // goes through even if the sticky-settings restore just fired a freq write.
    if (cat_set_frequency_forced(freq_hz) != ESP_OK) return;

    // Persist the chosen FT8/FT4 frequency so it survives a reboot and FT8 mode
    // never opens on the panadapter's inherited (non-FT8) VFO. (The FT4/FT8
    // sub-mode itself is already persisted via ft8_op_mode_set() below.)
    settings_set_ft8_freq_hz(freq_hz);

    ft8_op_mode_set(ft4 ? FT8_OP_MODE_FT4 : FT8_OP_MODE_FT8);
    // FT4 TX is always forced through the simulation interlock (see ft8_tx.c's
    // FT4 SAFETY note) regardless of the drawer's general sim-mode toggle, so
    // the breathing red border must track the sub-mode too, not just that
    // checkbox.
    ui_refresh_sim_mode_indicator();
    ft8_screen_clear();  // flush stale decodes from previous mode/band
    if (s_lbl_mode) {
        lv_label_set_text(s_lbl_mode, ft4 ? "MODE: FT4" : "MODE: FT8");
        lv_obj_set_style_text_color(s_lbl_mode,
                                    ft4 ? lv_color_hex(FT4_ACCENT_HEX)
                                        : lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    }

    // Optimistically update both labels without waiting for the FA poll.
    ui_update_frequency(freq_hz);               // top-bar "Freq:" label
    if (s_btn_freq) {
        char b[40];
        snprintf(b, sizeof(b), "Preset: %lu.%03lu MHz",
                 (unsigned long)(freq_hz / 1000000),
                 (unsigned long)((freq_hz / 1000) % 1000));
        lv_obj_t *lbl = lv_obj_get_child(s_btn_freq, 0);
        if (lbl) lv_label_set_text(lbl, b);
    }
}

static void ft8_freq_preset_cb(lv_event_t *e)
{
    apply_freq_preset((uint32_t)(uintptr_t)lv_event_get_user_data(e), false);
}

#if !FT4_MODE_DISABLED
static void ft4_freq_preset_cb(lv_event_t *e)
{
    apply_freq_preset((uint32_t)(uintptr_t)lv_event_get_user_data(e), true);
}
#endif

static void ft8_freq_overlay_cb(lv_event_t *e)
{
    (void)e;
    ft8_freq_popup_close();
}

static void ft8_freq_label_clicked_cb(lv_event_t *e);

// Build one preset column (FT8 or FT4) into the overlay `ov` at screen x
// `panel_x`, top-aligned at `top_y`. Renders only the bands that appear in the
// live CAT band list AND have a known dial frequency in `table`, sizing the
// panel to fit them without scrolling. `row_cb` (ft8_/ft4_freq_preset_cb) is
// invoked with the chosen dial frequency on tap. Returns the panel height, or
// 0 if no rows matched (nothing drawn).
static int build_preset_column(lv_obj_t *ov, int panel_x, int top_y,
                               const char *header_txt, uint32_t header_bg,
                               const ft8_band_freq_t *table, size_t table_n,
                               const cat_band_entry_t *bands, int band_count,
                               uint32_t cur_hz, lv_event_cb_t row_cb,
                               bool is_ft4_col)
{
    // A row is "selected" only in the column matching the current sub-mode -
    // so the gold highlight appears once across both columns, on the band the
    // radio is actually on in the active mode.
    bool col_is_active_mode = (is_ft4_col == (ft8_op_mode_get() == FT8_OP_MODE_FT4));
    // Pre-count the rows that will actually be drawn (CAT reports bands we have
    // no dial freq for; those are skipped) so the panel is sized exactly and
    // doesn't force a needless scroll.
    int visible_count = 0;
    for (int i = 0; i < band_count; i++) {
        for (size_t j = 0; j < table_n; j++) {
            if (strcmp(bands[i].name, table[j].band) == 0) { visible_count++; break; }
        }
    }
    if (visible_count == 0) return 0;

    int panel_w = 240;
    int header_h = 40;
    int avail_h = MID_H - 16;  // small top/bottom breathing margin
    int btn_h = (avail_h - header_h) / visible_count;
    if (btn_h > 64) btn_h = 64;  // don't grow absurdly tall for very few rows
    if (btn_h < 40) btn_h = 40;  // floor for a usable touch target
    int panel_h = visible_count * btn_h + header_h;
    bool needs_scroll = panel_h > avail_h;  // safety net only, shouldn't trigger normally
    if (needs_scroll) panel_h = avail_h;

    lv_obj_t *panel = lv_obj_create(ov);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_pos(panel, panel_x, top_y);
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
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = lv_obj_create(panel);
    lv_obj_set_size(header, panel_w, header_h);
    lv_obj_set_style_bg_color(header, lv_color_hex(header_bg), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);

    lv_obj_t *header_lbl = lv_label_create(header);
    lv_label_set_text(header_lbl, header_txt);
    lv_obj_set_style_text_font(header_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(header_lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(header_lbl);

    if (needs_scroll) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    } else {
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    for (int i = 0; i < band_count; i++) {
        // Find the dial frequency for this band in this column's table.
        uint32_t dial_hz = 0;
        for (size_t j = 0; j < table_n; j++) {
            if (strcmp(bands[i].name, table[j].band) == 0) {
                dial_hz = table[j].freq_hz;
                break;
            }
        }
        if (dial_hz == 0) continue;  // band not in this column's table

        bool active = col_is_active_mode &&
                      (cur_hz >= bands[i].center_hz - 1000000 &&
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
        lv_obj_add_event_cb(btn, row_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)dial_hz);

        char bstr[24];
        snprintf(bstr, sizeof(bstr), "%sm  %lu.%03lu",
                 bands[i].name,
                 (unsigned long)(dial_hz / 1000000),
                 (unsigned long)((dial_hz / 1000) % 1000));
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, bstr);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, active ? lv_color_hex(UI_COLOR_ACCENT_GOLD) : lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_center(lbl);
    }
    return panel_h;
}

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

    uint32_t cur_hz = cat_get_frequency();
    const int top_y  = MID_Y + 8;          // both columns top-aligned

    // FT8 column (gold header) at the left, FT4 column (cyan header) to its
    // right. FT4 = same radio data mode, different slot timing.
    int h_ft8 = build_preset_column(ov, LEFT_W, top_y, "FT8", 0xFFDD00,
                                    FT8_BAND_FREQS, N_FT8_BAND_FREQS,
                                    bands, band_count, cur_hz, ft8_freq_preset_cb, false);
    // FT4 soft-disabled (see FT4_MODE_DISABLED in ft8_test.h) - skip building
    // the column entirely so it's invisible, not just inert; ft8_op_mode_set()
    // would coerce a tap to FT8 anyway, but a visible-but-nonfunctional
    // column would look broken rather than "never there".
    int h_ft4 = 0;
#if !FT4_MODE_DISABLED
    const int col_w  = 240;
    const int col_gap = 20;
    h_ft4 = build_preset_column(ov, LEFT_W + col_w + col_gap, top_y, "FT4", FT4_ACCENT_HEX,
                                FT4_BAND_FREQS, N_FT4_BAND_FREQS,
                                bands, band_count, cur_hz, ft4_freq_preset_cb, true);
#endif

    if (h_ft8 == 0 && h_ft4 == 0) {
        ESP_LOGW(TAG, "FT8 freq dropdown: no bands with a known FT8/FT4 freq");
        ft8_freq_popup_close();
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
    lv_obj_set_style_pad_top(s_left_pane, 8, 0);     // #4: 50% less space over MODE: FT8
    lv_obj_set_style_pad_bottom(s_left_pane, 16, 0);
    lv_obj_set_style_pad_left(s_left_pane, 16, 0);
    lv_obj_set_style_pad_right(s_left_pane, 16, 0);
    lv_obj_clear_flag(s_left_pane, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_mode = lv_label_create(s_left_pane);
    // Reflect the current FT8/FT4 sub-mode flag (not always FT8) so the label
    // stays truthful if the screen is rebuilt while FT4 is selected.
    bool init_ft4 = (ft8_op_mode_get() == FT8_OP_MODE_FT4);
    lv_label_set_text(s_lbl_mode, init_ft4 ? "MODE: FT4" : "MODE: FT8");
    lv_obj_set_style_text_color(s_lbl_mode,
                                init_ft4 ? lv_color_hex(FT4_ACCENT_HEX)
                                         : lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_text_font(s_lbl_mode, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(s_lbl_mode, 0, 0);

    // s_btn_freq is purely visual (background/border/label). It is NOT a
    // click target: it's nested under s_container/s_left_pane, while
    // ui_init()'s top-bar Band/Mode/BW/Freq/Zoom hit-zones (hit_zones[] in
    // ui.c) are direct children of the screen, created AFTER this and each
    // explicitly lv_obj_move_foreground()'d. LVGL hit-tests direct children
    // of a common parent in reverse creation order before ever descending
    // into a sibling's subtree, so those screen-level hit-zones win every
    // tap here regardless of any move_foreground() applied *inside*
    // s_left_pane - moving s_btn_freq forward only reorders it among its
    // own siblings, it can never out-rank a sibling of s_container itself.
    // The real click target is s_btn_freq_hit below, a separate object
    // created directly on `parent` (the screen) so it competes at the same
    // tree level as those hit-zones and can be foregrounded over them.
    s_btn_freq = lv_btn_create(s_left_pane);
    lv_obj_set_size(s_btn_freq, 288, 60);
    lv_obj_set_pos(s_btn_freq, 0, 55);
    lv_obj_set_style_bg_color(s_btn_freq, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_width(s_btn_freq, 1, 0);
    lv_obj_set_style_border_color(s_btn_freq, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_radius(s_btn_freq, 8, 0);
    lv_obj_set_style_pad_all(s_btn_freq, 8, 0);
    lv_obj_clear_flag(s_btn_freq, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *freq_lbl = lv_label_create(s_btn_freq);
    lv_label_set_text(freq_lbl, "Preset: --.--- MHz");
    lv_obj_set_style_text_color(freq_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(freq_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(freq_lbl);

    // Screen-level hit target, sized/positioned to exactly cover s_btn_freq's
    // own screen rect: s_left_pane sits at screen (0, MID_Y) with pad_top=8/
    // pad_left=16, s_btn_freq is offset (0,55) within that padded content
    // box, so its screen rect is x:16..321, y:(MID_Y+8+55)..(+60) = +63..+123
    // i.e. y:(MID_Y+63)..(MID_Y+123). Kept in sync manually with the
    // s_btn_freq geometry above - if that ever moves, update this too.
    s_btn_freq_hit = lv_obj_create(parent);
    lv_obj_set_size(s_btn_freq_hit, 288, 60);
    lv_obj_set_pos(s_btn_freq_hit, 16, MID_Y + 63);
    lv_obj_set_style_bg_opa(s_btn_freq_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_freq_hit, 0, 0);
    lv_obj_clear_flag(s_btn_freq_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_btn_freq_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_btn_freq_hit, LV_OBJ_FLAG_HIDDEN);  // shown only while FT8 is active
    lv_obj_add_event_cb(s_btn_freq_hit, ft8_freq_label_clicked_cb, LV_EVENT_CLICKED, NULL);

    // UTC clock removed — it's in the center bottom bar already

    s_lbl_count = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_count, "Slot: -- s");
    lv_obj_set_style_text_color(s_lbl_count, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_lbl_count, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_count, 0, 120);  // #2: 50% less space under Preset label

    // Tiny countdown bar to the right of "EVEN/ODD  N s", counting down
    // from full (start of slot) to empty (end of slot). Range is in ms so the
    // fast t_slotbar_cb tick can glide it smoothly; colour set in t_clock_cb.
    s_bar_slot = lv_bar_create(s_left_pane);
    lv_obj_set_size(s_bar_slot, 140, 8);
    lv_obj_set_pos(s_bar_slot, 140, 131);
    lv_obj_set_style_radius(s_bar_slot, 2, 0);
    lv_obj_set_style_bg_color(s_bar_slot, lv_color_hex(0x303044), 0);
    lv_obj_set_style_border_width(s_bar_slot, 0, 0);
    lv_bar_set_range(s_bar_slot, 0, 15000);
    lv_bar_set_value(s_bar_slot, 15000, LV_ANIM_OFF);

    // CQ TX parity preference: [TX: EVEN] [TX: ODD] toggle row.
    // Double-height buttons below the slot countdown.  Dim grey when
    // inactive; lights up in the same slot colours as s_lbl_count
    // (steel blue / warm orange) when active.
    s_btn_tx_even = lv_btn_create(s_left_pane);
    lv_obj_set_size(s_btn_tx_even, 140, 52);
    lv_obj_set_pos(s_btn_tx_even, 0, 152);
    lv_obj_set_style_bg_color(s_btn_tx_even, lv_color_hex(0x303044), 0);
    lv_obj_set_style_border_width(s_btn_tx_even, 0, 0);
    lv_obj_set_style_radius(s_btn_tx_even, 8, 0);
    lv_obj_set_style_pad_all(s_btn_tx_even, 0, 0);
    lv_obj_add_event_cb(s_btn_tx_even, tx_even_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *even_lbl = lv_label_create(s_btn_tx_even);
    lv_label_set_text(even_lbl, "TX: EVEN");
    lv_obj_set_style_text_color(even_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(even_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(even_lbl);

    s_btn_tx_odd = lv_btn_create(s_left_pane);
    lv_obj_set_size(s_btn_tx_odd, 140, 52);
    lv_obj_set_pos(s_btn_tx_odd, 148, 152);
    lv_obj_set_style_bg_color(s_btn_tx_odd, lv_color_hex(0x303044), 0);
    lv_obj_set_style_border_width(s_btn_tx_odd, 0, 0);
    lv_obj_set_style_radius(s_btn_tx_odd, 8, 0);
    lv_obj_set_style_pad_all(s_btn_tx_odd, 0, 0);
    lv_obj_add_event_cb(s_btn_tx_odd, tx_odd_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *odd_lbl = lv_label_create(s_btn_tx_odd);
    lv_label_set_text(odd_lbl, "TX: ODD");
    lv_obj_set_style_text_color(odd_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(odd_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(odd_lbl);

    update_parity_btns();  // sync colours to s_cq_parity (persists on FT8 re-entry)

    // Filter / ADIF-log — side by side, each half the old full-width Filter
    // button (140 px, 8 px gap). ADIF-log was a long-press on "Active: N"
    // (field feedback: hard to land a long-press reliably) - a dedicated
    // button is a much easier target. Same white text as Filter; ADIF-log
    // gets a darker blue fill to read as the secondary of the pair.
    s_btn_filter = lv_btn_create(s_left_pane);
    lv_obj_set_size(s_btn_filter, 140, 60);
    lv_obj_set_pos(s_btn_filter, 0, 212);  // 8 px below the TX row (uniform grid gap)
    lv_obj_set_style_bg_color(s_btn_filter, lv_color_hex(UI_COLOR_PRIMARY), 0);  // pale blue
    lv_obj_set_style_border_color(s_btn_filter, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
    lv_obj_set_style_border_width(s_btn_filter, 2, 0);
    lv_obj_set_style_radius(s_btn_filter, 8, 0);
    lv_obj_add_event_cb(s_btn_filter, filter_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *filter_lbl = lv_label_create(s_btn_filter);
    lv_label_set_text(filter_lbl, "Filter");
    lv_obj_set_style_text_color(filter_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(filter_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(filter_lbl);

    s_btn_adif = lv_btn_create(s_left_pane);
    lv_obj_set_size(s_btn_adif, 140, 60);
    lv_obj_set_pos(s_btn_adif, 148, 212);
    lv_obj_set_style_bg_color(s_btn_adif, lv_color_hex(0x163d5e), 0);  // darker blue
    lv_obj_set_style_border_color(s_btn_adif, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
    lv_obj_set_style_border_width(s_btn_adif, 2, 0);
    lv_obj_set_style_radius(s_btn_adif, 8, 0);
    // SHORT_CLICKED (not CLICKED) so a long-press doesn't also fire the short
    // action; long-press is the always-available ADIF-log path (see cbs above).
    lv_obj_add_event_cb(s_btn_adif, adif_or_pileup_btn_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btn_adif, adif_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *adif_lbl = lv_label_create(s_btn_adif);
    lv_label_set_text(adif_lbl, "ADIF-log");
    lv_obj_set_style_text_color(adif_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(adif_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(adif_lbl);

    // "ME:xxxx" label removed — callsign/grid now shown compactly under MODE

    // v0.12.0: "Call CQ" - opens the TX confirmation modal pre-filled with
    // a CQ message at the conventional default audio tone. Coloured the
    // same green as the modal's "Transmit" / identity modal's "Save"
    // buttons - this app's established "primary action" colour - tying
    // the two steps of the flow together visually.
    s_btn_cq = lv_btn_create(s_left_pane);
    lv_obj_set_size(s_btn_cq, 288, 60);
    lv_obj_set_pos(s_btn_cq, 0, 280);
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

    // "Active: N" - own line directly below the Call CQ button.
    s_lbl_heard = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_heard, "Active: 0");
    lv_obj_set_style_text_color(s_lbl_heard, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_lbl_heard, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_heard, 0, 346);

    // TX state indicator - hidden while idle; amber/armed or red/active,
    // tap to cancel/abort. See t_clock_cb (1 Hz refresh: state, colour,
    // countdown text) and tx_indicator_tap_cb (the tap action itself).
    s_lbl_tx = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_tx, "");
    lv_obj_set_style_text_font(s_lbl_tx, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_tx, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_label_set_long_mode(s_lbl_tx, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_tx, 288);
    lv_obj_set_pos(s_lbl_tx, 0, 380);

    // Live TX PWR/SWR line. Separate label (cyan) aligned just under s_lbl_tx:
    // LVGL v9 dropped in-label recolor markup, so a distinct colour from the
    // red "TRANSMITTING" text needs its own object. Hidden unless transmitting.
    s_lbl_tx_pswr = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_tx_pswr, "");
    lv_obj_set_style_text_font(s_lbl_tx_pswr, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_tx_pswr, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_add_flag(s_lbl_tx_pswr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_tx, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lbl_tx, tx_indicator_tap_cb, LV_EVENT_CLICKED, NULL);

    // Manual QSO override buttons — Re-send / RR73 / 73.
    // Hidden when IDLE/CQ/DONE; shown during active exchange (WAIT_RPT/ROGER/RR73).
    // Sized to fill the left pane's full usable width (288px after padding)
    // and given more height - the original 91x44 buttons were cramped for
    // big-finger field use.
    {
        // Re-send carries more text ("Re-send" + a second line like "JO45"/"-07")
        // than RR73/73, so it gets 20% more width (110px vs the uniform 92px) and
        // the other two shrink to 83px each - still sums to the full 288px pane
        // width with the same 6px gaps: 110 + 6 + 83 + 6 + 83 = 288.
        const int bh = 64, gap = 6, by = 540;
        const int bw_resend = 110, bw_other = 83;
        int x = 0;
        struct { const char *lbl; uint32_t col; lv_event_cb_t cb; lv_obj_t **ptr; int w; } btns[] = {
            { "Re-send", 0x604010, override_resend_cb, &s_btn_override_resend, bw_resend },
            { "RR73",    0x1a5090, override_rr73_cb,   &s_btn_override_rr73,   bw_other  },
            { "73",      0x1e6028, override_73_cb,     &s_btn_override_73,     bw_other  },
        };
        for (int j = 0; j < 3; j++) {
            lv_obj_t *b = lv_btn_create(s_left_pane);
            lv_obj_set_size(b, btns[j].w, bh);
            lv_obj_set_pos(b, x, by);
            x += btns[j].w + gap;
            lv_obj_set_style_bg_color(b, lv_color_hex(btns[j].col), 0);
            lv_obj_set_style_radius(b, 4, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_add_event_cb(b, btns[j].cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, btns[j].lbl);
            lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
            lv_obj_center(l);
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
            *btns[j].ptr = b;
            if (j == 0) s_lbl_resend = l;
        }
    }

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

    struct { const char *t; int x; int w; lv_text_align_t a; } cols[10] = {
        { "SL",      COL_SLOT_X,    COL_SLOT_W,    LV_TEXT_ALIGN_LEFT  },
        { "CALL",    COL_CALL_X,    COL_CALL_W,    LV_TEXT_ALIGN_LEFT  },
        { "MESSAGE", COL_TEXT_X,    COL_MSG_W,     LV_TEXT_ALIGN_LEFT  },
        { "CTY",     COL_COUNTRY_X, COL_COUNTRY_W, LV_TEXT_ALIGN_LEFT  },
        { "SNR",     COL_SNR_X,     COL_SNR_W,     LV_TEXT_ALIGN_RIGHT },
        { "DT",      COL_DT_X,      COL_DT_W,      LV_TEXT_ALIGN_RIGHT },
        { "HZ",      COL_HZ_X,      COL_HZ_W,      LV_TEXT_ALIGN_RIGHT },
        { "KM",      COL_KM_X,      COL_KM_W,      LV_TEXT_ALIGN_RIGHT },
        { "BRG",     COL_BRG_X,     COL_BRG_W,     LV_TEXT_ALIGN_RIGHT },
        { "HRD",     COL_HEARD_X,   COL_HEARD_W,   LV_TEXT_ALIGN_RIGHT },
    };
    for (int i = 0; i < 10; i++) {
        lv_obj_t *lbl = lv_label_create(hdr);
        lv_obj_add_style(lbl, &s_style_header_label, 0);
        lv_label_set_text(lbl, cols[i].t);
        lv_obj_set_width(lbl, cols[i].w);
        lv_obj_set_x(lbl, cols[i].x);
        lv_obj_set_style_text_align(lbl, cols[i].a, 0);
        if (cols[i].x == COL_KM_X) s_lbl_hdr_km = lbl;
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


    ESP_LOGI(TAG, "FT8 view built (container %dx%d at y=%d, hidden)",
             MID_W, MID_H, MID_Y);
}

void ft8_screen_view_show(void)
{
    if (!s_container) return;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_refresh_pending = true;
    s_active = true;
    // FT8 mode has no spectrum to show and the tightest slot-timing budget in
    // the firmware - the web UI's 10 fps WS stream competes for the same
    // WiFi link as everything else, so pause it here the same way an
    // upload does (webserver_ws_set_paused). Resumed in hide() below.
    webserver_ws_set_paused(true);

    // ui_init() foregrounds the top-bar Band/Mode/BW/Freq/Zoom hit-zones
    // (each spanning the full 200px screen top, see hit_zones[] in ui.c) as
    // direct children of the screen, created AFTER ft8_screen_view_init()
    // builds s_container. Those hit-zones are screen-level siblings of
    // s_container, not descendants of it, so no amount of move_foreground()
    // *inside* s_container/s_left_pane can out-rank them - LVGL hit-tests a
    // parent's direct children in reverse creation order and descends into
    // the first match's subtree, so the hit-zones win every tap over the
    // whole s_container branch regardless of internal z-order. s_btn_freq_hit
    // is the real click target: also a direct child of the screen, so
    // foregrounding IT here makes it outrank those hit-zones.
    if (s_btn_freq_hit) {
        lv_obj_clear_flag(s_btn_freq_hit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_btn_freq_hit);
    }

    // Own-call cache and grid-derived location are kept live in rebuild_list()
    // (runs every refresh cycle off the same settings_load_all() call), not
    // just here on screen entry - see the comment there for why.
    ESP_LOGI(TAG, "show");
}

void ft8_screen_view_hide(void)
{
    if (!s_container) return;
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_freq_hit) lv_obj_add_flag(s_btn_freq_hit, LV_OBJ_FLAG_HIDDEN);
    s_active = false;
    webserver_ws_set_paused(false);  // back to Panadapter - resume the WS spectrum stream
    ESP_LOGI(TAG, "hide");
}

lv_obj_t *ft8_screen_view_get_container(void)
{
    return s_container;
}

bool ft8_screen_view_is_active(void)
{
    return s_active;
}

void ft8_screen_view_request_refresh(void)
{
    s_refresh_pending = true;
}
