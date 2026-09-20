// See resource_mgmt_modal.h.

#include "resource_mgmt_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "ui_mode.h"
#include "settings.h"
#include "audio/rx_audio.h"
#include "net/net_quiet.h"
#include "wspr_rx.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "resmgmt_modal";

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_panel = NULL;

// Row 0 is RX audio - never gated, always the one doing the gating. Rows
// 1..N are the background feeds it holds off. Order matches the panel.
typedef struct {
    lv_obj_t   *cb;
    lv_obj_t   *lbl;      // the row's own label, dimmed to show "held off"
} resmgmt_row_t;

#define ROW_AUDIO    0
#define ROW_SPOTMAP  1
#define ROW_RBN      2
#define ROW_CLUSTER  3
#define ROW_SPOTS    4
#define ROW_PSKRX    5
#define ROW_PSKTX    6
#define ROW_WSPR     7
#define ROW_COUNT    8

static resmgmt_row_t s_rows[ROW_COUNT];
static lv_obj_t *s_cb_audio  = NULL;   // row 0's checkbox - kept separately,
                                        // never gated, never dimmed
static lv_obj_t *s_gate_note = NULL;

// Plain checkbox, themed square indicator, generous touch target - same
// idiom as ft8_filter_modal.c's make_checkbox(), copied rather than shared
// because that one is file-static there.
static lv_obj_t *make_checkbox(lv_obj_t *parent)
{
    static lv_style_t style_ind;
    static bool        style_inited = false;
    if (!style_inited) {
        lv_style_init(&style_ind);
        lv_style_set_bg_color(&style_ind, lv_color_hex(UI_COLOR_SURFACE_RAISED));
        lv_style_set_border_color(&style_ind, lv_color_hex(UI_COLOR_BORDER));
        lv_style_set_border_width(&style_ind, 2);
        lv_style_set_pad_all(&style_ind, 8);
        style_inited = true;
    }
    static lv_style_t style_ind_checked;
    static bool        style_checked_inited = false;
    if (!style_checked_inited) {
        lv_style_init(&style_ind_checked);
        lv_style_set_bg_color(&style_ind_checked, lv_color_hex(UI_COLOR_PRIMARY));
        lv_style_set_border_color(&style_ind_checked, lv_color_hex(UI_COLOR_PRIMARY_BORDER));
        style_checked_inited = true;
    }

    lv_obj_t *cb = lv_checkbox_create(parent);
    lv_checkbox_set_text(cb, "");
    lv_obj_add_style(cb, &style_ind, LV_PART_INDICATOR);
    lv_obj_add_style(cb, &style_ind_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(cb, 28);
    lv_obj_clear_flag(cb, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    return cb;
}

// Rows 1..N: greyed + un-clickable while RX audio is on, since net_quiet
// already stops them running in that state - a checkbox the operator could
// tick with no effect would be worse than one they can't reach.
static void refresh_gating(void)
{
    bool audio_on = rx_audio_is_enabled();
    for (int i = ROW_SPOTMAP; i < ROW_COUNT; i++) {
        if (!s_rows[i].cb) continue;
        if (audio_on) {
            lv_obj_add_state(s_rows[i].cb, LV_STATE_DISABLED);
            if (s_rows[i].lbl) lv_obj_set_style_text_opa(s_rows[i].lbl, LV_OPA_50, 0);
        } else {
            lv_obj_clear_state(s_rows[i].cb, LV_STATE_DISABLED);
            if (s_rows[i].lbl) lv_obj_set_style_text_opa(s_rows[i].lbl, LV_OPA_COVER, 0);
        }
    }
    if (s_gate_note) {
        if (audio_on) lv_obj_clear_flag(s_gate_note, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_gate_note, LV_OBJ_FLAG_HIDDEN);
    }
}

static void audio_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    rx_audio_set_enabled(on);
    ESP_LOGI(TAG, "RX audio %s from resource panel", on ? "enabled" : "disabled");
    refresh_gating();
}

static void spotmap_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_spotmap_en(on);
}

static void rbn_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_rbn_en(on);
}

static void cluster_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_cluster_en(on);
}

static void spots_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_spots_en(on);
}

static void pskrx_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_psk_rx_en(on);
}

static void psktx_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_pskreporter_en(on);
}

// WSPR is a whole page/mode, not a quiet background feed, so turning it off
// here follows the SAME exit-cleanly path as the drawer's own seven-tap
// unlock (ota_modal.c): stop a live RX session and leave the WSPR screen if
// it was the one open, rather than leaving a stopped feature's page showing.
static void wspr_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_wspr_en(on);
    if (!on && ui_mode_get() == UI_MODE_WSPR) {
        wspr_rx_stop();
        ui_request_base_mode_m(UI_MODE_PANADAPTER);
    }
}

// Tapping the label toggles its row's checkbox - same reasoning as every
// other modal in this app: a 31 px box next to 200 px of dead label space
// is the single biggest reason these rows feel hard to hit.
static void label_toggles_cb(lv_event_t *e)
{
    lv_obj_t *cb = (lv_obj_t *)lv_event_get_user_data(e);
    if (!cb || lv_obj_has_state(cb, LV_STATE_DISABLED)) return;
    if (lv_obj_has_state(cb, LV_STATE_CHECKED)) lv_obj_clear_state(cb, LV_STATE_CHECKED);
    else                                        lv_obj_add_state(cb, LV_STATE_CHECKED);
    lv_obj_send_event(cb, LV_EVENT_VALUE_CHANGED, NULL);
}

typedef struct {
    int         row;
    const char *label;
    lv_event_cb_t cb;
} row_def_t;

static const row_def_t ROW_DEFS[ROW_COUNT] = {
    { ROW_AUDIO,   "RX Audio (speaker/headphone)",       audio_cb },
    { ROW_SPOTMAP, "SelfSpotter (spot map)",              spotmap_cb },
    { ROW_RBN,     "RBN (CW skimmer spots)",              rbn_cb },
    { ROW_CLUSTER, "DX cluster",                          cluster_cb },
    { ROW_SPOTS,   "POTA / SOTA spots",                   spots_cb },
    { ROW_PSKRX,   "PSK Reporter - who's hearing me",     pskrx_cb },
    { ROW_PSKTX,   "PSK Reporter - report my decodes",    psktx_cb },
    { ROW_WSPR,    "WSPR",                                wspr_cb },
};

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_size(s_panel, 760, 560);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Resource Management");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // The rule, stated plainly once rather than repeated per row.
    lv_obj_t *sub = lv_label_create(s_panel);
    lv_label_set_text(sub, "RX audio and the background feeds share the same scarce memory.");
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_20, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 44);

    s_gate_note = lv_label_create(s_panel);
    lv_label_set_text(s_gate_note, LV_SYMBOL_WARNING " Held off while RX audio is on");
    lv_obj_set_style_text_color(s_gate_note, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_gate_note, &lv_font_montserrat_20, 0);
    lv_obj_align(s_gate_note, LV_ALIGN_TOP_LEFT, 0, 72);
    lv_obj_add_flag(s_gate_note, LV_OBJ_FLAG_HIDDEN);

    int y = 116;
    const int ROW_H = 52;
    for (int i = 0; i < ROW_COUNT; i++) {
        const row_def_t *d = &ROW_DEFS[i];

        lv_obj_t *lbl = lv_label_create(s_panel);
        lv_label_set_text(lbl, d->label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *cb = make_checkbox(s_panel);
        lv_obj_align(cb, LV_ALIGN_TOP_RIGHT, 0, y - 6);
        lv_obj_add_event_cb(cb, d->cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(lbl, label_toggles_cb, LV_EVENT_CLICKED, cb);

        // Row 0 (RX audio) itself is never dimmed/disabled - it is what
        // decides everyone else's state, not something the rule applies to.
        if (i == ROW_AUDIO) {
            s_cb_audio = cb;
        } else {
            s_rows[d->row].cb  = cb;
            s_rows[d->row].lbl = lbl;
        }

        if (i == ROW_AUDIO) {
            // A thin separator under the priority row, so the "this one
            // decides the rest" relationship reads visually, not just in
            // the sub-label above.
            y += ROW_H;
            lv_obj_t *sep = lv_obj_create(s_panel);
            lv_obj_set_size(sep, 760 - 48, 2);
            lv_obj_set_style_bg_color(sep, lv_color_hex(UI_COLOR_BORDER), 0);
            lv_obj_set_style_border_width(sep, 0, 0);
            lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 0, y);
            y += 16;
        } else {
            y += ROW_H;
        }
    }

    lv_obj_t *close_btn = lv_btn_create(s_panel);
    lv_obj_set_size(close_btn, 200, 64);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(close_lbl);

    ui_kbd_set_buttons(NULL, close_btn);

    ESP_LOGI(TAG, "Resource management modal built");
}

// Re-read every row from the live settings each open, same reasoning as
// every drawer checkbox in this app: a value changed from the web UI or a
// different code path must not show stale here (see feedback_ note "every
// drawer checkbox re-reads its setting on open").
static void rows_refresh_from_settings(void)
{
    qmx_settings_t s;
    settings_load_all(&s);

    struct { int row; bool val; } vals[] = {
        { ROW_SPOTMAP, s.spotmap_en },
        { ROW_RBN,     s.rbn_en },
        { ROW_CLUSTER, s.cluster_en },
        { ROW_SPOTS,   s.spots_en },
        { ROW_PSKRX,   s.psk_rx_en },
        { ROW_PSKTX,   s.pskreporter_en },
        { ROW_WSPR,    wspr_feature_enabled() },
    };
    if (s_cb_audio) {
        if (rx_audio_is_enabled()) lv_obj_add_state(s_cb_audio, LV_STATE_CHECKED);
        else                       lv_obj_clear_state(s_cb_audio, LV_STATE_CHECKED);
    }
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        lv_obj_t *cb = s_rows[vals[i].row].cb;
        if (!cb) continue;
        if (vals[i].val) lv_obj_add_state(cb, LV_STATE_CHECKED);
        else             lv_obj_clear_state(cb, LV_STATE_CHECKED);
    }
    refresh_gating();
}

void resource_mgmt_modal_open(void)
{
    modal_build();
    rows_refresh_from_settings();
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    ESP_LOGI(TAG, "opened");
}

bool resource_mgmt_modal_is_open(void)
{
    return s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}
