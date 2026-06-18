// "Set and Sync the Clock" modal.
//
// Three large boxes: [HH] : [MM] : [SS]
//
//  HH / MM  — tap to show a 2-row numpad BELOW this panel:
//    Row 1: [0][1][2][3][4][✓]   Row 2: [5][6][7][8][9][✗]
//    ✓ confirms; ✗ = backspace (tap on empty = cancel).
//    Validated: HH 0-23, MM 0-59. Pre-filled from current UTC.
//    "HH"/"MM" hint stays centred below the big digits.
//
//  SS  — live FT8-corrected seconds (syncs at each slot decode, ~15 s).
//    Blue frame = auto-syncing; grey frame = locked.
//    Tap SS to toggle locked / auto. While locked, seconds still count
//    at the captured offset — no FT8 drift after locking.
//    Hint shows "FT8 +Xms" / "FT8 ok" / "no FT8" / "locked".
//
//  Apply: time_sync_apply_correction_ms (sub-second) when only SS
//    correction is needed; time_sync_set_manual when HH or MM were edited.

#include "ft8_time_modal.h"
#include "ui_theme.h"
#include "ft8_test.h"
#include "time_sync/time_sync.h"
#include "cat.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <math.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ft8_time_modal";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef enum { SS_SYNC_FT8 = 0, SS_SYNC_NTP = 1, SS_SYNC_QMX = 2 } ss_mode_t;

static int       s_hh_val     = 0;
static int       s_mm_val     = 0;
static int       s_ss_display = 0;
static int       s_ss_err_ms  = 0;        // FT8 sub-second correction (ms)
static ss_mode_t s_ss_mode    = SS_SYNC_FT8;
static int       s_edit_field = 0;        // 0=none, 1=HH, 2=MM
// QMX-mode base time: TM; result + system time_t when it was queried
static int   s_qmx_h     = 0;
static int   s_qmx_m     = 0;
static int   s_qmx_s     = 0;
static time_t s_qmx_base = 0;
static bool  s_qmx_valid = false;
static char s_edit_buf[3];
static bool s_hh_edited  = false;
static bool s_mm_edited  = false;
static bool s_open       = false;

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

static lv_obj_t   *s_modal    = NULL;
static lv_obj_t   *s_panel    = NULL;
static lv_obj_t   *s_box_hh   = NULL;
static lv_obj_t   *s_box_mm   = NULL;
static lv_obj_t   *s_box_ss   = NULL;
static lv_obj_t   *s_lbl_hh   = NULL;
static lv_obj_t   *s_lbl_mm   = NULL;
static lv_obj_t   *s_lbl_ss   = NULL;
static lv_obj_t   *s_hint_ss  = NULL;  // dynamic FT8 status below SS digits
static lv_obj_t   *s_numpad   = NULL;
static lv_timer_t *s_timer    = NULL;

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

#define BOX_BG             0x0e1620
#define BOX_BORDER_DIM     0x334455
#define BOX_BORDER_ACT     0x5588cc   // HH or MM selected
#define BOX_BORDER_SS_FT8  0x1a5090   // SS FT8 sync (blue)
#define BOX_BORDER_SS_NTP  0x1a7030   // SS NTP sync (green)
#define BOX_BORDER_SS_QMX  0xb0b0b0   // SS QMX sync (white-ish)

static void refresh_box_styles(void)
{
    lv_obj_set_style_border_color(s_box_hh,
        lv_color_hex(s_edit_field == 1 ? BOX_BORDER_ACT : BOX_BORDER_DIM), 0);
    lv_obj_set_style_border_width(s_box_hh, s_edit_field == 1 ? 3 : 2, 0);

    lv_obj_set_style_border_color(s_box_mm,
        lv_color_hex(s_edit_field == 2 ? BOX_BORDER_ACT : BOX_BORDER_DIM), 0);
    lv_obj_set_style_border_width(s_box_mm, s_edit_field == 2 ? 3 : 2, 0);

    uint32_t ss_border; uint32_t ss_digit;
    switch (s_ss_mode) {
        case SS_SYNC_FT8: ss_border = BOX_BORDER_SS_FT8; ss_digit = 0x80bbff; break;
        case SS_SYNC_NTP: ss_border = BOX_BORDER_SS_NTP; ss_digit = 0x80e890; break;
        case SS_SYNC_QMX: ss_border = BOX_BORDER_SS_QMX; ss_digit = 0xffffff; break;
        default:          ss_border = BOX_BORDER_DIM;    ss_digit = 0xffffff; break;
    }
    lv_obj_set_style_border_color(s_box_ss, lv_color_hex(ss_border), 0);
    lv_obj_set_style_border_width(s_box_ss, 3, 0);
    lv_obj_set_style_text_color(s_lbl_ss, lv_color_hex(ss_digit), 0);
}

static void update_box_label(int field)
{
    char buf[8];
    if (field == 1) {
        if (s_edit_buf[0]) snprintf(buf, sizeof(buf), "%s_", s_edit_buf);
        else               snprintf(buf, sizeof(buf), "%02d", s_hh_val);
        lv_label_set_text(s_lbl_hh, buf);
    } else if (field == 2) {
        if (s_edit_buf[0]) snprintf(buf, sizeof(buf), "%s_", s_edit_buf);
        else               snprintf(buf, sizeof(buf), "%02d", s_mm_val);
        lv_label_set_text(s_lbl_mm, buf);
    }
}

// ---------------------------------------------------------------------------
// Numpad logic
// ---------------------------------------------------------------------------

static void confirm_edit(void)
{
    if (!s_edit_buf[0]) goto done;
    int val = atoi(s_edit_buf);
    bool ok = false;
    if (s_edit_field == 1 && val >= 0 && val <= 23) { s_hh_val = val; s_hh_edited = true; ok = true; }
    if (s_edit_field == 2 && val >= 0 && val <= 59) { s_mm_val = val; s_mm_edited = true; ok = true; }
    if (!ok) {
        lv_obj_t *b = (s_edit_field == 1) ? s_box_hh : s_box_mm;
        lv_obj_set_style_border_color(b, lv_color_hex(0xcc3333), 0);
        lv_obj_set_style_border_width(b, 3, 0);
        return;  // keep numpad open
    }
done:
    s_edit_buf[0] = '\0';
    update_box_label(1);
    update_box_label(2);
    lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
    s_edit_field = 0;
    refresh_box_styles();
}

static void digit_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (strlen(s_edit_buf) < 2) {
        size_t l = strlen(s_edit_buf);
        s_edit_buf[l] = '0' + d;
        s_edit_buf[l + 1] = '\0';
    }
    update_box_label(s_edit_field);
}

static void check_cb(lv_event_t *e) { (void)e; confirm_edit(); }

static void cross_cb(lv_event_t *e)
{
    (void)e;
    size_t l = strlen(s_edit_buf);
    if (l > 0) {
        s_edit_buf[l - 1] = '\0';
        update_box_label(s_edit_field);
    } else {
        s_edit_buf[0] = '\0';
        update_box_label(s_edit_field);
        lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
        s_edit_field = 0;
        refresh_box_styles();
    }
}

// ---------------------------------------------------------------------------
// Box tap callbacks
// ---------------------------------------------------------------------------

static void hh_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_edit_field == 1) { confirm_edit(); return; }
    if (s_edit_field == 2)   confirm_edit();
    s_edit_field = 1;
    s_edit_buf[0] = '\0';
    lv_obj_clear_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
    refresh_box_styles();
}

static void mm_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_edit_field == 2) { confirm_edit(); return; }
    if (s_edit_field == 1)   confirm_edit();
    s_edit_field = 2;
    s_edit_buf[0] = '\0';
    lv_obj_clear_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
    refresh_box_styles();
}

static void ss_tap_cb(lv_event_t *e)
{
    (void)e;
    // Discard any in-progress numpad edit before switching source
    if (s_edit_field) {
        s_edit_buf[0] = '\0';
        s_edit_field  = 0;
        lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
    }

    s_ss_mode = (ss_mode_t)((s_ss_mode + 1) % 3);
    char b[4];

    if (s_ss_mode == SS_SYNC_NTP) {
        s_hh_edited = false;
        s_mm_edited = false;
        s_ss_err_ms = 0;
        // Update HH/MM immediately from system clock (no wait for next timer tick)
        time_t now = time(NULL);
        struct tm tm; gmtime_r(&now, &tm);
        s_hh_val = tm.tm_hour;
        s_mm_val = tm.tm_min;
        snprintf(b, sizeof(b), "%02d", s_hh_val); lv_label_set_text(s_lbl_hh, b);
        snprintf(b, sizeof(b), "%02d", s_mm_val); lv_label_set_text(s_lbl_mm, b);

    } else if (s_ss_mode == SS_SYNC_QMX) {
        s_hh_edited = false;
        s_mm_edited = false;
        s_qmx_valid = false;
        if (cat_is_ready()) {
            if (cat_query_qmx_time(&s_qmx_h, &s_qmx_m, &s_qmx_s) == ESP_OK) {
                s_qmx_base = time(NULL);
                s_qmx_valid = true;
                // Update HH/MM immediately from QMX result
                s_hh_val = s_qmx_h;
                s_mm_val = s_qmx_m;
                snprintf(b, sizeof(b), "%02d", s_qmx_h); lv_label_set_text(s_lbl_hh, b);
                snprintf(b, sizeof(b), "%02d", s_qmx_m); lv_label_set_text(s_lbl_mm, b);
                ESP_LOGI(TAG, "QMX time: %02d:%02d:%02d", s_qmx_h, s_qmx_m, s_qmx_s);
            } else {
                ESP_LOGW(TAG, "QMX TM; query failed");
            }
        } else {
            ESP_LOGW(TAG, "QMX not ready for TM; query");
        }

    } else {
        // Back to FT8 — HH/MM revert to system clock on next timer tick
        s_ss_err_ms = 0;
        ft8_get_last_timing_ms(&s_ss_err_ms);
    }

    refresh_box_styles();
    ESP_LOGI(TAG, "SS mode: %s", s_ss_mode == SS_SYNC_FT8 ? "FT8" :
                                  s_ss_mode == SS_SYNC_NTP ? "NTP" : "QMX");
}

// ---------------------------------------------------------------------------
// Apply / Close
// ---------------------------------------------------------------------------

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    if (s_timer) lv_timer_pause(s_timer);
    s_open = false;
}

static void apply_cb(lv_event_t *e)
{
    (void)e;
    if (s_edit_field) confirm_edit();
    time_t now = time(NULL);
    struct tm tm; gmtime_r(&now, &tm);

    switch (s_ss_mode) {
        case SS_SYNC_FT8: {
            int err_ms = 0;
            if (!s_hh_edited && !s_mm_edited && ft8_get_last_timing_ms(&err_ms)) {
                time_sync_apply_correction_ms(err_ms);
                ESP_LOGI(TAG, "Applied FT8 correction: %+d ms", err_ms);
            } else {
                time_sync_set_manual(tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
                                     s_hh_val, s_mm_val, s_ss_display);
                time_sync_mark_ft8();
            }
            break;
        }
        case SS_SYNC_NTP:
            // System clock is already NTP-disciplined — push current time to QMX.
            time_sync_push_to_qmx();
            break;
        case SS_SYNC_QMX:
            if (s_qmx_valid) {
                time_sync_set_manual(tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
                                     s_hh_val, s_mm_val, s_ss_display);
                time_sync_mark_qmx();
                ESP_LOGI(TAG, "Applied QMX time: %02d:%02d:%02d", s_hh_val, s_mm_val, s_ss_display);
            }
            break;
    }
    modal_close();
}

static void cancel_cb(lv_event_t *e) { (void)e; modal_close(); }
static void swallow_click_cb(lv_event_t *e) { (void)e; }

// ---------------------------------------------------------------------------
// 1 Hz timer
// ---------------------------------------------------------------------------

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_open) return;

    time_t now = time(NULL);
    struct tm tm; gmtime_r(&now, &tm);

    if (s_ss_mode == SS_SYNC_QMX && s_qmx_valid) {
        // QMX mode: all three values extrapolated from the TM; base
        time_t elapsed = now - s_qmx_base;
        int total_s = s_qmx_h * 3600 + s_qmx_m * 60 + s_qmx_s + (int)elapsed;
        total_s %= 86400;
        if (total_s < 0) total_s += 86400;
        int qh = total_s / 3600;
        int qm = (total_s % 3600) / 60;
        int qs = total_s % 60;
        if (s_edit_field != 1) {
            s_hh_val = qh;
            char b[4]; snprintf(b, sizeof(b), "%02d", qh);
            lv_label_set_text(s_lbl_hh, b);
        }
        if (s_edit_field != 2) {
            s_mm_val = qm;
            char b[4]; snprintf(b, sizeof(b), "%02d", qm);
            lv_label_set_text(s_lbl_mm, b);
        }
        s_ss_display = qs;
        char b[4]; snprintf(b, sizeof(b), "%02d", qs);
        lv_label_set_text(s_lbl_ss, b);
        lv_label_set_text(s_hint_ss, "SS  QMX sync");
    } else {
        // FT8 or NTP mode — HH/MM from system clock
        // NTP forces live tracking even if user previously tapped HH/MM
        bool ntp = (s_ss_mode == SS_SYNC_NTP);
        if ((!s_hh_edited || ntp) && s_edit_field != 1) {
            s_hh_val = tm.tm_hour;
            char b[4]; snprintf(b, sizeof(b), "%02d", s_hh_val);
            lv_label_set_text(s_lbl_hh, b);
        }
        if ((!s_mm_edited || ntp) && s_edit_field != 2) {
            s_mm_val = tm.tm_min;
            char b[4]; snprintf(b, sizeof(b), "%02d", s_mm_val);
            lv_label_set_text(s_lbl_mm, b);
        }

        // SS — FT8-corrected or raw NTP
        int err_ms = 0;
        if (s_ss_mode == SS_SYNC_FT8) {
            if (ft8_get_last_timing_ms(&err_ms)) s_ss_err_ms = err_ms;
            err_ms = s_ss_err_ms;
        }
        int64_t corrected_ms = (int64_t)now * 1000LL - (int64_t)err_ms;
        time_t corrected = (time_t)(corrected_ms / 1000LL);
        struct tm tc; gmtime_r(&corrected, &tc);
        s_ss_display = tc.tm_sec;
        char b[4]; snprintf(b, sizeof(b), "%02d", s_ss_display);
        lv_label_set_text(s_lbl_ss, b);

        lv_label_set_text(s_hint_ss,
            s_ss_mode == SS_SYNC_FT8 ? "SS  now syncing..." : "SS  NTP sync");
    }
}

// ---------------------------------------------------------------------------
// Build helpers
// ---------------------------------------------------------------------------

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(BOX_BG), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(BOX_BORDER_DIM), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static void build_numpad(lv_obj_t *panel_ref)
{
    // 6 buttons × 100 px + 5 × 8 px gaps = 640 px wide; 2 rows × 60 + 1 × 8 = 128 px
    const int NW = 640, BW = 100, BH = 60, GAPX = 8, GAPY = 8;

    s_numpad = lv_obj_create(s_modal);
    lv_obj_set_size(s_numpad, NW, 2 * BH + GAPY);
    lv_obj_align_to(s_numpad, panel_ref, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lv_obj_set_style_bg_color(s_numpad, lv_color_hex(0x141c24), 0);
    lv_obj_set_style_bg_opa(s_numpad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_numpad, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_numpad, 2, 0);
    lv_obj_set_style_radius(s_numpad, 10, 0);
    lv_obj_set_style_pad_all(s_numpad, 0, 0);
    lv_obj_clear_flag(s_numpad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);

    struct { const char *lbl; lv_event_cb_t cb; void *ud; uint32_t col; } keys[12] = {
        {"0", digit_cb, (void*)0, 0x2a3a4a},
        {"1", digit_cb, (void*)1, 0x2a3a4a},
        {"2", digit_cb, (void*)2, 0x2a3a4a},
        {"3", digit_cb, (void*)3, 0x2a3a4a},
        {"4", digit_cb, (void*)4, 0x2a3a4a},
        {LV_SYMBOL_OK,    check_cb, NULL, 0x1e6028},
        {"5", digit_cb, (void*)5, 0x2a3a4a},
        {"6", digit_cb, (void*)6, 0x2a3a4a},
        {"7", digit_cb, (void*)7, 0x2a3a4a},
        {"8", digit_cb, (void*)8, 0x2a3a4a},
        {"9", digit_cb, (void*)9, 0x2a3a4a},
        {LV_SYMBOL_CLOSE, cross_cb, NULL, 0x962020},
    };
    for (int i = 0; i < 12; i++) {
        int col = i % 6, row = i / 6;
        lv_obj_t *b = lv_btn_create(s_numpad);
        lv_obj_set_size(b, BW, BH);
        lv_obj_set_pos(b, col * (BW + GAPX), row * (BH + GAPY));
        lv_obj_set_style_bg_color(b, lv_color_hex(keys[i].col), 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, keys[i].cb, LV_EVENT_CLICKED, keys[i].ud);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, keys[i].lbl);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_center(l);
    }
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

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
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_modal, cancel_cb, LV_EVENT_CLICKED, NULL);

    // Main panel — 640 × 296, centred 108 px above screen centre
    // so the 128 px numpad fits below with 8 px gap without clipping.
    // Screen centre 360 - 108 = 252 → top 252-148=104, bottom 252+148=400.
    // Numpad: top 408, bottom 536 < 720 ✓
    s_panel = lv_obj_create(s_modal);
    lv_obj_set_size(s_panel, 640, 316);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, -108);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 20, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_panel, swallow_click_cb, LV_EVENT_CLICKED, NULL);

    // Inner: 600 × 256 (after pad_all=20)

    // Title
    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Set and Sync the Clock");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    // One-line hint
    lv_obj_t *hint = lv_label_create(s_panel);
    lv_label_set_text(hint, "Tap HH/MM to edit. Tap SS to sync to FT8/NTP/QMX");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, 600);
    lv_obj_set_pos(hint, 0, 42);

    // HH : MM : SS boxes — font_48 for digits, BY=85
    // Box w=182, colon gap 17 px → 3×182 + 2×17 = 580 ≤ 600 ✓
    // Box h=116: digit (font_48 ≈ 56 px) centred -10, hint at bottom -6.
    const int BW = 182, BH = 116, BY = 85;
    const int x_hh = 0, x_c1 = 185, x_mm = 202, x_c2 = 387, x_ss = 404;

    // HH
    s_box_hh = make_box(s_panel, x_hh, BY, BW, BH);
    lv_obj_add_event_cb(s_box_hh, hh_tap_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_hh = lv_label_create(s_box_hh);
    lv_label_set_text(s_lbl_hh, "00");
    lv_obj_set_style_text_color(s_lbl_hh, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_hh, &lv_font_montserrat_48, 0);
    lv_obj_align(s_lbl_hh, LV_ALIGN_CENTER, 0, -12);
    {
        lv_obj_t *h = lv_label_create(s_box_hh);
        lv_label_set_text(h, "HH");
        lv_obj_set_style_text_color(h, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
        lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    // Colon 1
    {
        lv_obj_t *c = lv_label_create(s_panel);
        lv_label_set_text(c, ":");
        lv_obj_set_style_text_color(c, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_48, 0);
        lv_obj_set_pos(c, x_c1, BY + (BH - 56) / 2 - 8);
    }

    // MM
    s_box_mm = make_box(s_panel, x_mm, BY, BW, BH);
    lv_obj_add_event_cb(s_box_mm, mm_tap_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_mm = lv_label_create(s_box_mm);
    lv_label_set_text(s_lbl_mm, "00");
    lv_obj_set_style_text_color(s_lbl_mm, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_mm, &lv_font_montserrat_48, 0);
    lv_obj_align(s_lbl_mm, LV_ALIGN_CENTER, 0, -12);
    {
        lv_obj_t *h = lv_label_create(s_box_mm);
        lv_label_set_text(h, "MM");
        lv_obj_set_style_text_color(h, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
        lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    // Colon 2
    {
        lv_obj_t *c = lv_label_create(s_panel);
        lv_label_set_text(c, ":");
        lv_obj_set_style_text_color(c, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_48, 0);
        lv_obj_set_pos(c, x_c2, BY + (BH - 56) / 2 - 8);
    }

    // SS
    s_box_ss = make_box(s_panel, x_ss, BY, BW, BH);
    lv_obj_add_event_cb(s_box_ss, ss_tap_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_ss = lv_label_create(s_box_ss);
    lv_label_set_text(s_lbl_ss, "00");
    lv_obj_set_style_text_color(s_lbl_ss, lv_color_hex(0x80bbff), 0);
    lv_obj_set_style_text_font(s_lbl_ss, &lv_font_montserrat_48, 0);
    lv_obj_align(s_lbl_ss, LV_ALIGN_CENTER, 0, -12);
    s_hint_ss = lv_label_create(s_box_ss);
    lv_label_set_text(s_hint_ss, "SS  now syncing...");
    lv_obj_set_style_text_color(s_hint_ss, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(s_hint_ss, &lv_font_montserrat_18, 0);
    lv_obj_align(s_hint_ss, LV_ALIGN_BOTTOM_MID, 0, -6);

    // Apply + Cancel — right under the boxes (y = BY+BH+8 = 70+116+8 = 194)
    const int ABY = BY + BH + 8;
    {
        lv_obj_t *b = lv_btn_create(s_panel);
        lv_obj_set_size(b, 292, 61);
        lv_obj_set_pos(b, 0, ABY);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1e6028), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, apply_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, "Save");
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_center(l);
    }
    {
        lv_obj_t *b = lv_btn_create(s_panel);
        lv_obj_set_size(b, 292, 61);
        lv_obj_set_pos(b, 308, ABY);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x962020), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, cancel_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, "Cancel");
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_center(l);
    }

    build_numpad(s_panel);

    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    lv_timer_pause(s_timer);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ft8_time_modal_show(void)
{
    modal_build();

    s_edit_field  = 0;
    s_edit_buf[0] = '\0';
    s_hh_edited   = false;
    s_mm_edited   = false;
    s_ss_mode     = SS_SYNC_FT8;
    s_qmx_valid   = false;

    time_t now = time(NULL);
    struct tm tm; gmtime_r(&now, &tm);
    s_hh_val     = tm.tm_hour;
    s_mm_val     = tm.tm_min;
    s_ss_display = tm.tm_sec;
    s_ss_err_ms  = 0;
    ft8_get_last_timing_ms(&s_ss_err_ms);

    char b[4];
    snprintf(b, sizeof(b), "%02d", s_hh_val);    lv_label_set_text(s_lbl_hh, b);
    snprintf(b, sizeof(b), "%02d", s_mm_val);    lv_label_set_text(s_lbl_mm, b);
    snprintf(b, sizeof(b), "%02d", s_ss_display); lv_label_set_text(s_lbl_ss, b);
    lv_label_set_text(s_hint_ss, "SS  now syncing...");

    lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
    refresh_box_styles();

    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    lv_timer_resume(s_timer);
    s_open = true;
}
