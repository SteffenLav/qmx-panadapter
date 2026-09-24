// See resource_mgmt_modal.h.

#include "resource_mgmt_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "settings.h"
#include "audio/rx_audio.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "resmgmt_modal";

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_scroll = NULL;   // scrolling content strip - see modal_build()

// Row 0 is RX audio - never gated, always the one doing the gating. Rows
// 1..N are the background feeds it CAN hold off - but only the ones with a
// standing task/connection actually do (see ROW_DEFS' gated flag below).
// WSPR was dropped from this panel entirely (2026-09-20, operator's call):
// it is a whole separate mode already exclusive with CW/SSB by RADIO MODE,
// not a quiet background feed competing for the same memory, so it never
// belonged in the same list as these.
typedef struct {
    lv_obj_t   *cb;
    lv_obj_t   *lbl;      // the row's own label, dimmed to show "held off"
} resmgmt_row_t;

/* Order is the ON-SCREEN order, and it is deliberate (2026-09-21):
 *   - RX Audio first, because it decides everyone else's state;
 *   - Binaural CW directly under it, since it is a sub-feature of audio rather
 *     than a competitor for its memory, with the three pan sliders beneath it;
 *   - then the network feeds, with the two that are merely held off by audio
 *     (SelfSpotter, RBN, DX cluster, PSK RX) grouped together, so the rows that
 *     grey out do so as one block instead of alternating with rows that stay
 *     live. PSK RX moved up beside DX cluster for exactly that reason.
 * ROW_GATED and refresh_gating() are written to be order-independent, so this
 * list can be reshuffled again without hunting for an index that assumed it. */
#define ROW_AUDIO     0
#define ROW_BINAURAL  1
#define ROW_SPOTMAP   2
#define ROW_RBN       3
#define ROW_CLUSTER   4
#define ROW_PSKRX     5
#define ROW_SPOTS     6
#define ROW_PSKTX     7
#define ROW_COUNT     8

static resmgmt_row_t s_rows[ROW_COUNT];
static lv_obj_t *s_cb_audio  = NULL;   // row 0's checkbox - kept separately,
                                        // never gated, never dimmed
static lv_obj_t *s_gate_note = NULL;

/* Panel width, and the separator that has to match it. Was a bare 760 in two
 * places; the separator silently kept the old width when the panel grew. */
/* 1000 -> 1100 (2026-09-23): the scroll strip's scrollbar runs down the right
 * edge and was touching the row check boxes, which align to that same edge.
 * The extra width plus SCROLL_PAD_R below gives the bar its own lane. The
 * screen is 1280, so a 1100-wide panel still centres with 90 px either side. */
#define PANEL_W      1100
#define PANEL_PAD    24
/* Right inset inside the scrolling strip, reserved for the scrollbar so no
 * right-aligned child (the check boxes) ever sits under it. */
#define SCROLL_PAD_R 20

/* Gain sits on the RX Audio line and follows that checkbox; the three pan
 * sliders sit under Binaural CW and follow THAT one, because they shape the
 * stereo split and do nothing while it is off. Declared here so
 * refresh_gating() - which is above their callbacks - can reach them. */
static lv_obj_t *s_sld_gain      = NULL;
static lv_obj_t *s_sld_width     = NULL;
static lv_obj_t *s_sld_blend     = NULL;
static lv_obj_t *s_sld_ovlp      = NULL;
static lv_obj_t *s_lbl_gain_val  = NULL;
static lv_obj_t *s_lbl_width_val = NULL;
static lv_obj_t *s_lbl_blend_val = NULL;
static lv_obj_t *s_lbl_ovlp_val  = NULL;
static lv_obj_t *s_pan_hdr       = NULL;   // "Panoramic split" caption

// AGC attack/release, ms - sit on their own line directly under Gain,
// gated the same way (live whenever RX audio is on).
static lv_obj_t *s_sld_attack     = NULL;
static lv_obj_t *s_sld_release    = NULL;
static lv_obj_t *s_lbl_attack_val = NULL;
static lv_obj_t *s_lbl_release_val = NULL;

/* RX output volume, 0..100. Moved here 2026-09-23 (operator): it was only in
 * the settings drawer, one screen away from the AGC controls it interacts
 * with, and "it is confusing to have it two places" - the level you hear is
 * decided by the ceiling AND this together, so they belong on one page. */
static lv_obj_t *s_sld_vol     = NULL;
static lv_obj_t *s_lbl_vol_val = NULL;

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

// Only the rows with a standing task/connection are gated: SelfSpotter's
// MQTT client, RBN and DX cluster's telnet sessions, PSK Reporter's "who's
// hearing me" query (its own comment calls it "by far the largest periodic
// allocation on the device"). POTA/SOTA and PSK Reporter's TX reports are
// periodic/batched with nothing standing between fetches, and the operator
// asked to keep those running - see rx_audio.h's 2026-09-20 note. Greyed +
// un-clickable, not just informational: a checkbox the operator could tick
// with no effect would be worse than one they can't reach.
static const bool ROW_GATED[ROW_COUNT] = {
    [ROW_AUDIO]    = false,
    [ROW_SPOTMAP]  = true,
    [ROW_RBN]      = true,
    [ROW_CLUSTER]  = true,
    [ROW_SPOTS]    = false,
    [ROW_PSKRX]    = true,
    [ROW_PSKTX]    = false,
    [ROW_BINAURAL] = false,   // gated the OPPOSITE way - see refresh_gating()
};

static void refresh_gating(void)
{
    bool audio_on = rx_audio_is_enabled();
    bool any_gated_shown = false;
    /* Every row except RX Audio itself, which is the one doing the gating.
     * Deliberately NOT "from ROW_SPOTMAP": that only worked while SPOTMAP
     * happened to be index 1, and the rows were reordered on 2026-09-21. */
    for (int i = 0; i < ROW_COUNT; i++) {
        if (i == ROW_AUDIO) continue;
        if (!s_rows[i].cb || !ROW_GATED[i]) continue;
        any_gated_shown = true;
        if (audio_on) {
            lv_obj_add_state(s_rows[i].cb, LV_STATE_DISABLED);
            if (s_rows[i].lbl) lv_obj_set_style_text_opa(s_rows[i].lbl, LV_OPA_50, 0);
        } else {
            lv_obj_clear_state(s_rows[i].cb, LV_STATE_DISABLED);
            if (s_rows[i].lbl) lv_obj_set_style_text_opa(s_rows[i].lbl, LV_OPA_COVER, 0);
        }
    }
    if (s_gate_note) {
        if (audio_on && any_gated_shown) lv_obj_clear_flag(s_gate_note, LV_OBJ_FLAG_HIDDEN);
        else                             lv_obj_add_flag(s_gate_note, LV_OBJ_FLAG_HIDDEN);
    }

    // Binaural is a SUB-feature of RX audio, not a competitor for its
    // memory - it does nothing (silently falls back to mono, see
    // rx_audio.c) unless audio is already on, so the opposite rule applies:
    // dimmed/disabled while audio is OFF, live once it is ON.
    if (s_rows[ROW_BINAURAL].cb) {
        if (audio_on) {
            lv_obj_clear_state(s_rows[ROW_BINAURAL].cb, LV_STATE_DISABLED);
            if (s_rows[ROW_BINAURAL].lbl) lv_obj_set_style_text_opa(s_rows[ROW_BINAURAL].lbl, LV_OPA_COVER, 0);
        } else {
            lv_obj_add_state(s_rows[ROW_BINAURAL].cb, LV_STATE_DISABLED);
            if (s_rows[ROW_BINAURAL].lbl) lv_obj_set_style_text_opa(s_rows[ROW_BINAURAL].lbl, LV_OPA_50, 0);
        }
    }

    /* LV_STATE_DISABLED blocks input but does NOT dim a slider - there is no
     * style bound to that state on these - so a disabled slider still drew in
     * full blue and read as live. Seen on the bench 2026-09-21 with Binaural
     * unchecked and all three pan sliders looking usable. Set the opacity
     * explicitly on every part, alongside the state. */
    #define SLD_SET_ENABLED(s, on)                                              \
        do {                                                                    \
            if (!(s)) break;                                                    \
            if (on) lv_obj_clear_state((s), LV_STATE_DISABLED);                 \
            else    lv_obj_add_state((s), LV_STATE_DISABLED);                   \
            const lv_opa_t _o = (on) ? LV_OPA_COVER : LV_OPA_40;                \
            lv_obj_set_style_opa((s), _o, LV_PART_MAIN);                        \
            lv_obj_set_style_opa((s), _o, LV_PART_INDICATOR);                   \
            lv_obj_set_style_opa((s), _o, LV_PART_KNOB);                        \
        } while (0)

    /* Gain (AGC ceiling) and the AGC timing pair are live whenever audio is -
     * they shape the level/response in every supported mode, not just CW. */
    SLD_SET_ENABLED(s_sld_gain, audio_on);
    if (s_lbl_gain_val)
        lv_obj_set_style_text_opa(s_lbl_gain_val, audio_on ? LV_OPA_COVER : LV_OPA_50, 0);
    SLD_SET_ENABLED(s_sld_vol, audio_on);
    SLD_SET_ENABLED(s_sld_attack, audio_on);
    SLD_SET_ENABLED(s_sld_release, audio_on);
    if (s_lbl_vol_val)
        lv_obj_set_style_text_opa(s_lbl_vol_val, audio_on ? LV_OPA_COVER : LV_OPA_50, 0);
    if (s_lbl_attack_val)
        lv_obj_set_style_text_opa(s_lbl_attack_val, audio_on ? LV_OPA_COVER : LV_OPA_50, 0);
    if (s_lbl_release_val)
        lv_obj_set_style_text_opa(s_lbl_release_val, audio_on ? LV_OPA_COVER : LV_OPA_50, 0);

    /* The pan sliders need audio AND binaural: they shape a split that is not
     * being produced otherwise, so leaving them live would offer three controls
     * that audibly do nothing - the exact complaint this panel is fixing. */
    const bool pan_live = audio_on && rx_audio_get_binaural_enabled();
    lv_obj_t *const pan_w[] = { s_sld_width, s_sld_blend, s_sld_ovlp };
    for (unsigned i = 0; i < sizeof pan_w / sizeof pan_w[0]; i++)
        SLD_SET_ENABLED(pan_w[i], pan_live);
    #undef SLD_SET_ENABLED
    lv_obj_t *const pan_l[] = { s_lbl_width_val, s_lbl_blend_val, s_lbl_ovlp_val, s_pan_hdr };
    for (unsigned i = 0; i < sizeof pan_l / sizeof pan_l[0]; i++)
        if (pan_l[i]) lv_obj_set_style_text_opa(pan_l[i], pan_live ? LV_OPA_COVER : LV_OPA_50, 0);
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

static void binaural_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    rx_audio_set_binaural_enabled(on);
    refresh_gating();     // the three pan sliders follow this checkbox
}

/* ---- Gain and the three pan sliders ---------------------------------------
 *
 * These four existed from the first audio build but only over /api/cmd, which
 * is not something an operator can reach - the v1.16.0 notes described them as
 * adjustable and they were not. Each slider writes the SETTING (persisted, and
 * carried in a config backup) and applies the live value, because the DSP holds
 * its own copy and would otherwise not hear the change until the next boot.
 *
 * Scaling matches settings.h: gain /10, width x10, blend and overlap x100. */
static void gain_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    settings_set_rxaud_gain_d10((uint8_t)v);
    rx_audio_set_agc_gain_max((float)v * 10.0f);
    if (s_lbl_gain_val) lv_label_set_text_fmt(s_lbl_gain_val, "%d", v * 10);
}

/* rx_audio_set_volume() persists to settings itself, so unlike the sliders
 * below there is no separate settings_set_* call here - adding one would just
 * write the same key twice. */
static void rx_vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    rx_audio_set_volume((uint8_t)v);
    if (s_lbl_vol_val) lv_label_set_text_fmt(s_lbl_vol_val, "%d", v);
}

static void agc_attack_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    settings_set_rxaud_agc_attack_ms((uint8_t)v);
    rx_audio_set_agc_attack_ms((uint8_t)v);
    if (s_lbl_attack_val) lv_label_set_text_fmt(s_lbl_attack_val, "%d ms", v);
}

static void agc_release_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    settings_set_rxaud_agc_release_ms((uint16_t)v);
    rx_audio_set_agc_release_ms((uint16_t)v);
    if (s_lbl_release_val) lv_label_set_text_fmt(s_lbl_release_val, "%d ms", v);
}

static void pan_width_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    settings_set_rxaud_pan_width_x10((uint8_t)v);
    rx_audio_set_pan_width((float)v / 10.0f);
    if (s_lbl_width_val) lv_label_set_text_fmt(s_lbl_width_val, "%d.%d", v / 10, v % 10);
}

static void pan_blend_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    settings_set_rxaud_pan_blend_x100((uint8_t)v);
    rx_audio_set_pan_blend((float)v / 100.0f);
    if (s_lbl_blend_val) lv_label_set_text_fmt(s_lbl_blend_val, "0.%02d", v);
}

static void pan_ovlp_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    settings_set_rxaud_pan_ovlp_x100((uint8_t)v);
    rx_audio_set_pan_overlap((float)v / 100.0f);
    if (s_lbl_ovlp_val) lv_label_set_text_fmt(s_lbl_ovlp_val, "0.%02d", v);
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

/* On-screen order - see the ROW_ defines for why. */
static const row_def_t ROW_DEFS[ROW_COUNT] = {
    { ROW_AUDIO,   "RX Audio (speaker/headphone)",       audio_cb },
    { ROW_BINAURAL,"Binaural CW (stereo separation)",     binaural_cb },
    { ROW_SPOTMAP, "SelfSpotter (spot map)",              spotmap_cb },
    { ROW_RBN,     "RBN (CW skimmer spots)",              rbn_cb },
    { ROW_CLUSTER, "DX cluster",                          cluster_cb },
    { ROW_PSKRX,   "PSK Reporter - who's hearing me",     pskrx_cb },
    { ROW_SPOTS,   "POTA / SOTA spots",                   spots_cb },
    { ROW_PSKTX,   "PSK Reporter - report my decodes",    psktx_cb },
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
    // 700: the screen itself is only 720 tall and centering anything taller
    // leaves too little top/bottom margin to trust without a screenshot.
    // Content (8 rows + pan trio + AGC attack/release, 2026-09-23) no longer
    // fits inside a fixed 700 without doing fragile per-row pixel arithmetic
    // every time a row is added - see the growth history this comment used
    // to track (492->548->today). Fixed that CLASS of bug instead of the
    // instance: the row content below now lives in s_scroll, a genuinely
    // scrollable strip between the header and the Close button, so a future
    // addition here cannot silently collide with the button or the screen
    // edge - it just scrolls. Close stays pinned outside the scroll area.
    /* 760 -> 1000 wide: the three pan sliders sit side by side under Binaural
     * and need room to be draggable rather than fiddly. */
    lv_obj_set_size(s_panel, PANEL_W, 700);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    /* "Audio Settings", not "Resource Management" - operator, 2026-09-24:
     * "it is not really about resources as it is about real audio settings".
     * Correct: five of its rows are RX audio, the panoramic trio is audio,
     * and the feed switches are only here because they compete with audio
     * for internal heap. The old name described the mechanism; this one
     * describes what the operator came to do. */
    lv_label_set_text(title, "Audio Settings");
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

    // Header (title/subtitle/gate note above) ends around y=92; Close sits in
    // the last 64 px of the panel's content box (648 = 700 - 2*24 pad -
    // 2*2 border) plus a 16 px gap above it. Everything in between scrolls.
    s_scroll = lv_obj_create(s_panel);
    lv_obj_set_pos(s_scroll, 0, 100);
    lv_obj_set_size(s_scroll, LV_PCT(100), 648 - 100 - 64 - 16);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_scroll, 0, 0);
    // Keep right-aligned children (the check boxes) out from under the
    // scrollbar - see SCROLL_PAD_R.
    lv_obj_set_style_pad_right(s_scroll, SCROLL_PAD_R, 0);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);

    int y = 16;   // relative to s_scroll now, not s_panel
    const int ROW_H = 46;
    for (int i = 0; i < ROW_COUNT; i++) {
        const row_def_t *d = &ROW_DEFS[i];

        lv_obj_t *lbl = lv_label_create(s_scroll);
        lv_label_set_text(lbl, d->label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *cb = make_checkbox(s_scroll);
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

        if (d->row == ROW_AUDIO) {
            y += ROW_H;

            /* RX Volume, then AGC Ceiling, Attack, Release - each its OWN full row, same
             * label/slider/value shape as every other row (0 label, 490
             * slider, 780 value). Ceiling used to ride on the RX Audio row's
             * own line at x=430, sharing it with that row's checkbox label -
             * fine at the old "Gain" (4 chars) but "AGC Ceiling" (11 chars)
             * ran into the slider (operator screenshot, 2026-09-23: the
             * label text sat under the slider knob). Given its own row
             * instead of tuning more x-offset arithmetic by eye. */
            {
                struct {
                    const char   *cap;
                    lv_obj_t    **sld;
                    lv_obj_t    **val;
                    int           min, max, cur;
                    lv_event_cb_t cb;
                    const char   *fmt;
                } agcs[4] = {
                    { "RX Volume",   &s_sld_vol,     &s_lbl_vol_val,     0, 100,  rx_audio_get_volume(),                rx_vol_cb,      "%d" },
                    { "AGC Ceiling", &s_sld_gain,    &s_lbl_gain_val,    5, 150,  settings_get_rxaud_gain_d10(),        gain_cb,        "%d" },
                    { "AGC Attack",  &s_sld_attack,  &s_lbl_attack_val,  1, 50,   settings_get_rxaud_agc_attack_ms(),  agc_attack_cb,  "%d ms" },
                    { "AGC Release", &s_sld_release, &s_lbl_release_val, 10, 500, settings_get_rxaud_agc_release_ms(), agc_release_cb, "%d ms" },
                };
                /* ⭐ THESE ROWS HAVE NO CHECK BOX, so unlike the feed rows they
                 * can use nearly the whole width - and they SHOULD. At 270 px
                 * the operator could not set them accurately: "they are too
                 * short now - there is plenty of space on each side ... it is
                 * very difficult to adjust precisely when so short". A ceiling
                 * of 50..1500 across 270 px is about 5 units per pixel; across
                 * 700 px it is under 2. Content width is 1028 (panel 1100 less
                 * 2x24 pad, 2x2 border and SCROLL_PAD_R), so 210 + 700 + value
                 * at 930 leaves the value text clear to the right edge. */
                const int SLD_X = 210, SLD_W = 700, SLD_VAL_X = 930;
                for (int p = 0; p < 4; p++) {
                    lv_obj_t *cap = lv_label_create(s_scroll);
                    lv_label_set_text(cap, agcs[p].cap);
                    lv_obj_set_style_text_color(cap, lv_color_hex(UI_COLOR_TEXT), 0);
                    lv_obj_set_style_text_font(cap, &lv_font_montserrat_24, 0);
                    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, y);

                    lv_obj_t *sld = lv_slider_create(s_scroll);
                    lv_obj_set_size(sld, SLD_W, 14);
                    lv_obj_align(sld, LV_ALIGN_TOP_LEFT, SLD_X, y + 10);
                    lv_slider_set_range(sld, agcs[p].min, agcs[p].max);
                    lv_slider_set_value(sld, agcs[p].cur, LV_ANIM_OFF);
                    lv_obj_add_event_cb(sld, agcs[p].cb, LV_EVENT_VALUE_CHANGED, NULL);
                    *agcs[p].sld = sld;

                    lv_obj_t *val = lv_label_create(s_scroll);
                    lv_label_set_text_fmt(val, agcs[p].fmt, agcs[p].cur);
                    lv_obj_set_style_text_color(val, lv_color_hex(UI_COLOR_TEXT), 0);
                    lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
                    lv_obj_align(val, LV_ALIGN_TOP_LEFT, SLD_VAL_X, y + 4);
                    *agcs[p].val = val;

                    y += ROW_H;
                }
            }
        } else if (d->row == ROW_BINAURAL) {
            // The three pan controls, side by side directly under the switch
            // that makes them do anything.
            y += ROW_H - 8;

            s_pan_hdr = lv_label_create(s_scroll);
            lv_label_set_text(s_pan_hdr, "Panoramic split - adjust while listening");
            lv_obj_set_style_text_color(s_pan_hdr, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
            lv_obj_set_style_text_font(s_pan_hdr, &lv_font_montserrat_20, 0);
            lv_obj_align(s_pan_hdr, LV_ALIGN_TOP_LEFT, 16, y);
            y += 30;

            /* 292 -> 320: the panel went 1000 -> 1100 wide, and the trio was
             * still laid out for the old width, leaving ~96 px dead on the
             * right. Same reason the four audio sliders above grew - a wider
             * slider is a more precise one. 16 + 2*340 + 320 = 1016, inside
             * the 1028 content width. */
            const int SW = 320, SGAP = 20;
            struct {
                const char   *cap;
                lv_obj_t    **sld;
                lv_obj_t    **val;
                int           min, max, cur;
                lv_event_cb_t cb;
            } pans[3] = {
                { "Width",   &s_sld_width, &s_lbl_width_val, 0,  30, settings_get_rxaud_pan_width_x10(),  pan_width_cb },
                { "Blend",   &s_sld_blend, &s_lbl_blend_val, 0,  50, settings_get_rxaud_pan_blend_x100(), pan_blend_cb },
                { "Overlap", &s_sld_ovlp,  &s_lbl_ovlp_val,  0, 100, settings_get_rxaud_pan_ovlp_x100(),  pan_ovlp_cb  },
            };

            for (int p = 0; p < 3; p++) {
                const int x = 16 + p * (SW + SGAP);

                lv_obj_t *cap = lv_label_create(s_scroll);
                lv_label_set_text(cap, pans[p].cap);
                lv_obj_set_style_text_color(cap, lv_color_hex(UI_COLOR_TEXT), 0);
                lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
                lv_obj_align(cap, LV_ALIGN_TOP_LEFT, x, y);

                lv_obj_t *val = lv_label_create(s_scroll);
                lv_obj_set_style_text_color(val, lv_color_hex(UI_COLOR_TEXT), 0);
                lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
                lv_obj_align(val, LV_ALIGN_TOP_LEFT, x + SW - 60, y);
                *pans[p].val = val;

                lv_obj_t *s = lv_slider_create(s_scroll);
                lv_obj_set_size(s, SW, 14);
                lv_obj_align(s, LV_ALIGN_TOP_LEFT, x, y + 30);
                lv_slider_set_range(s, pans[p].min, pans[p].max);
                lv_slider_set_value(s, pans[p].cur, LV_ANIM_OFF);
                lv_obj_add_event_cb(s, pans[p].cb, LV_EVENT_VALUE_CHANGED, NULL);
                *pans[p].sld = s;
            }

            // Seed the three value labels through their own callbacks' format
            // strings, so the text can never disagree with what a drag shows.
            lv_label_set_text_fmt(s_lbl_width_val, "%d.%d",
                                  settings_get_rxaud_pan_width_x10() / 10,
                                  settings_get_rxaud_pan_width_x10() % 10);
            lv_label_set_text_fmt(s_lbl_blend_val, "0.%02d", settings_get_rxaud_pan_blend_x100());
            lv_label_set_text_fmt(s_lbl_ovlp_val,  "0.%02d", settings_get_rxaud_pan_ovlp_x100());

            y += 62;

            /* The separator belongs HERE, under the whole audio block, not
             * under the RX Audio row alone. It divides "audio and the controls
             * that shape it" from "background feeds that audio holds off" -
             * which is the division the panel is actually about. Sitting
             * directly beneath RX Audio it cut the audio group in half and
             * implied Binaural belonged with the network feeds. */
            lv_obj_t *sep = lv_obj_create(s_scroll);
            /* LV_PCT, not PANEL_W - 2*PANEL_PAD: the strip now also reserves
             * SCROLL_PAD_R on the right, so a width computed from the panel
             * overflows the content box and would arm a horizontal scroll. */
            lv_obj_set_size(sep, LV_PCT(100), 2);
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
        { ROW_SPOTMAP,  s.spotmap_en },
        { ROW_RBN,      s.rbn_en },
        { ROW_CLUSTER,  s.cluster_en },
        { ROW_SPOTS,    s.spots_en },
        { ROW_PSKRX,    s.psk_rx_en },
        { ROW_PSKTX,    s.pskreporter_en },
        { ROW_BINAURAL, rx_audio_get_binaural_enabled() },
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

    /* Slider positions and their value labels, re-read on every open. The
     * modal is built once and reused, so without this a config import - which
     * can change all four - would leave the sliders showing what they were
     * built with while the audio played something else. */
    if (s_sld_gain) {
        const uint8_t g = settings_get_rxaud_gain_d10();
        lv_slider_set_value(s_sld_gain, g, LV_ANIM_OFF);
        if (s_lbl_gain_val) lv_label_set_text_fmt(s_lbl_gain_val, "%d", g * 10);
    }
    if (s_sld_vol) {
        const uint8_t v = rx_audio_get_volume();
        lv_slider_set_value(s_sld_vol, v, LV_ANIM_OFF);
        if (s_lbl_vol_val) lv_label_set_text_fmt(s_lbl_vol_val, "%d", v);
    }
    if (s_sld_attack) {
        const uint8_t a = settings_get_rxaud_agc_attack_ms();
        lv_slider_set_value(s_sld_attack, a, LV_ANIM_OFF);
        if (s_lbl_attack_val) lv_label_set_text_fmt(s_lbl_attack_val, "%d ms", a);
    }
    if (s_sld_release) {
        const uint16_t r = settings_get_rxaud_agc_release_ms();
        lv_slider_set_value(s_sld_release, r, LV_ANIM_OFF);
        if (s_lbl_release_val) lv_label_set_text_fmt(s_lbl_release_val, "%d ms", r);
    }
    if (s_sld_width) {
        const uint8_t w = settings_get_rxaud_pan_width_x10();
        lv_slider_set_value(s_sld_width, w, LV_ANIM_OFF);
        if (s_lbl_width_val) lv_label_set_text_fmt(s_lbl_width_val, "%d.%d", w / 10, w % 10);
    }
    if (s_sld_blend) {
        const uint8_t b = settings_get_rxaud_pan_blend_x100();
        lv_slider_set_value(s_sld_blend, b, LV_ANIM_OFF);
        if (s_lbl_blend_val) lv_label_set_text_fmt(s_lbl_blend_val, "0.%02d", b);
    }
    if (s_sld_ovlp) {
        const uint8_t o = settings_get_rxaud_pan_ovlp_x100();
        lv_slider_set_value(s_sld_ovlp, o, LV_ANIM_OFF);
        if (s_lbl_ovlp_val) lv_label_set_text_fmt(s_lbl_ovlp_val, "0.%02d", o);
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
