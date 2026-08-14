// CQ message preset editor modal. Long-press "Call CQ" to open.
// Three editable message fields, each with a radio button to pick the active
// one; a short Call CQ tap transmits the selected message. Persisted to NVS.
//
// Structurally based on identity_config.c (modal + text areas + on-screen
// keyboard), with a single-select radio column and three fields.

#include "ft8_cq_modal.h"
#include "ui_theme.h"
#include "ft8_screen_view.h"
#include "ft8_msg_guard.h"
#include "settings.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "ft8_cq_modal";

#define N_CQ 3

static lv_obj_t *s_modal     = NULL;
static lv_obj_t *s_keyboard  = NULL;
static lv_obj_t *s_ta[N_CQ]    = { NULL, NULL, NULL };
static lv_obj_t *s_radio[N_CQ] = { NULL, NULL, NULL };
static lv_obj_t *s_add_lbl   = NULL;  // "+ <call> <grid>" quick-insert button label
static lv_obj_t *s_add_btn   = NULL;
static lv_obj_t *s_save_btn  = NULL;
static lv_obj_t *s_fd_hint   = NULL;  // Field Day status note - set in show()
static int       s_sel       = 0;
static bool      s_open      = false;
// CQ auto-stop limit (Don WB0LQW: "I usually send CQ 2-4 times and then
// pause"). Top-right cycle button; 0 = never stop. Commits IMMEDIATELY on
// tap (not via Save) - same instant-cycle semantics as the pane's TXCQ
// parity button - which also keeps it usable while Field Day mode locks the
// preset editor (the limit is orthogonal to what the CQ text says).
static lv_obj_t *s_stop_btn  = NULL;
static lv_obj_t *s_stop_lbl  = NULL;
static uint8_t   s_stop_val  = 0;
static const uint8_t s_stop_vals[] = { 0, 1, 2, 3, 4, 5, 10 };
// Listening slot (Roy KI0ER): spend one slot receiving after every N CQ calls,
// so the occupancy picture for your OWN time window stays current. Same
// commit-on-tap behaviour as the stop button beside it.
static lv_obj_t *s_listen_btn = NULL;
static lv_obj_t *s_listen_lbl = NULL;
static uint8_t   s_listen_val = 0;
static const uint8_t s_listen_vals[] = { 0, 3, 5, 10 };
// While true (Field Day mode on), the whole editor is locked - everything
// except Cancel is both visually dimmed/disabled AND ignored by its handler,
// so there's no path to edit/save/select a preset while it's in effect,
// regardless of how LVGL's DISABLED state behaves for a given widget type.
static bool      s_fd_locked = false;

static void to_upper_inplace(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

// Strip leading/trailing spaces in place. A stray leading space breaks
// ftx_message_encode's "CQ" detection, so a preset saved as " CQ JP ..."
// silently failed to transmit and fell through to the identity modal
// (field-hit on the 3rd preset, 2026-07-15).
// Interior runs matter as much as the edges - see ft8_msg_guard.h. Don WB0LQW's
// preset was stored as "CQ  POTA WB0LQW" (two spaces) and transmitted as a
// signal report to a hashed callsign for as long as it was saved that way.
static void trim_inplace(char *s)
{
    ft8_msg_normalize(s);
}

// Build "CQ <call> <grid>" from stored identity (or "CQ" if unset).
static void build_default_cq(char *out, size_t len)
{
    qmx_settings_t s;
    settings_load_all(&s);
    if (s.my_callsign[0] && s.my_grid[0]) {
        snprintf(out, len, "CQ %s %s", s.my_callsign, s.my_grid);
    } else {
        snprintf(out, len, "CQ");
    }
}

// True if the (space-terminated) token at the start of `tok` contains a
// digit - callsigns always do, CQ modifier tokens (POTA, SOTA, DX, ...)
// never do.
static bool token_has_digit(const char *tok)
{
    for (; *tok && *tok != ' '; tok++) {
        if (*tok >= '0' && *tok <= '9') return true;
    }
    return false;
}

// Core Field Day tagging logic, shared between the real TX path
// (ft8_cq_get_active_text, reading the saved preset) and the editor's live
// preview (reading whatever is currently typed, unsaved) - one function, so
// the preview can never drift out of sync with what actually gets sent.
static void apply_fd_tag(const char *base, char *out, size_t len, bool field_day_en)
{
    // Not a "CQ" message (or "CQ" isn't a standalone token) - nothing to tag.
    bool is_cq = (strncmp(base, "CQ", 2) == 0) && (base[2] == ' ' || base[2] == '\0');
    if (!field_day_en || !is_cq) {
        snprintf(out, len, "%s", base);
        return;
    }

    // Field Day mode: force the standard "CQ FD ..." modifier (same
    // mechanism as "CQ POTA"/"CQ SOTA"), REPLACING any other modifier the
    // preset carries (DX/POTA/SOTA/...) - FD signalling takes priority for
    // the whole-operation duration FD mode is on, and FT8's CQ format only
    // has room for one modifier token.
    const char *rest = base + 2;
    while (*rest == ' ') rest++;
    if (*rest && !token_has_digit(rest)) {
        while (*rest && *rest != ' ') rest++;  // skip the existing modifier token
        while (*rest == ' ') rest++;
    }
    if (*rest) snprintf(out, len, "CQ FD %s", rest);
    else       snprintf(out, len, "CQ FD");
}

void ft8_cq_get_active_text(char *out, size_t len)
{
    if (!out || !len) return;
    qmx_settings_t s;
    settings_load_all(&s);
    uint8_t sel = (s.cq_sel <= 2) ? s.cq_sel : 0;
    char base[28];
    if (s.cq_msg[sel][0]) {
        strncpy(base, s.cq_msg[sel], sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    } else {
        build_default_cq(base, sizeof(base));  // empty slot -> default CQ
    }
    trim_inplace(base);   // tolerate an already-stored leading/trailing space
    apply_fd_tag(base, out, len, s.field_day_en);
}

// Reflect single-selection in the radio column.
static void apply_radio_state(void)
{
    for (int i = 0; i < N_CQ; i++) {
        if (!s_radio[i]) continue;
        if (i == s_sel) lv_obj_add_state(s_radio[i], LV_STATE_CHECKED);
        else            lv_obj_remove_state(s_radio[i], LV_STATE_CHECKED);
    }
}

// Live, always-accurate preview of what Call CQ will actually transmit for
// whichever preset is currently selected, reading the textarea's *unsaved*
// text so it tracks every keystroke and every preset switch. Bullet-proof by
// construction: it calls the exact same apply_fd_tag() the real TX path
// uses, so it can never show something different from what gets sent.
static void update_fd_preview(void)
{
    if (!s_fd_hint) return;
    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.field_day_en) {
        lv_label_set_text(s_fd_hint, "");
        return;
    }
    if (s_sel < 0 || s_sel >= N_CQ || !s_ta[s_sel]) return;

    char base[28];
    const char *raw = lv_textarea_get_text(s_ta[s_sel]);
    strncpy(base, raw ? raw : "", sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    to_upper_inplace(base);  // presets are saved upper-cased; match that

    char tx[28];
    apply_fd_tag(base, tx, sizeof(tx), true);

    char line[128];
    snprintf(line, sizeof(line),
             LV_SYMBOL_WARNING " Field Day mode ON - any tag above is overridden. Will transmit: \"%s\"", tx);
    lv_label_set_text(s_fd_hint, line);
}

// Lock the whole editor when Field Day mode is on: editing/selecting/saving
// a preset is moot either way (FD always overrides whatever modifier is
// there), so there is nothing useful to do here except back out. Every
// widget below is both visually dimmed+disabled AND its handler short-
// circuits via s_fd_locked - belt and suspenders, so there's no path to
// sneak past LVGL's DISABLED state for a particular widget type. Cancel is
// deliberately left untouched: it's the only thing you can do.
static void apply_fd_dim(bool field_day_en)
{
    s_fd_locked = field_day_en;
    lv_opa_t opa = field_day_en ? LV_OPA_50 : LV_OPA_COVER;
    for (int i = 0; i < N_CQ; i++) {
        if (s_ta[i]) {
            lv_obj_set_style_opa(s_ta[i], opa, 0);
            if (field_day_en) lv_obj_add_state(s_ta[i], LV_STATE_DISABLED);
            else              lv_obj_remove_state(s_ta[i], LV_STATE_DISABLED);
        }
        if (s_radio[i]) {
            lv_obj_set_style_opa(s_radio[i], opa, 0);
            if (field_day_en) lv_obj_add_state(s_radio[i], LV_STATE_DISABLED);
            else              lv_obj_remove_state(s_radio[i], LV_STATE_DISABLED);
        }
    }
    if (s_add_btn) {
        lv_obj_set_style_opa(s_add_btn, opa, 0);
        if (field_day_en) lv_obj_add_state(s_add_btn, LV_STATE_DISABLED);
        else               lv_obj_remove_state(s_add_btn, LV_STATE_DISABLED);
    }
    if (s_save_btn) {
        lv_obj_set_style_opa(s_save_btn, opa, 0);
        if (field_day_en) lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
        else               lv_obj_remove_state(s_save_btn, LV_STATE_DISABLED);
    }
}

static void stop_btn_update_label(void)
{
    if (!s_stop_lbl) return;
    if (s_stop_val == 0) lv_label_set_text(s_stop_lbl, "CQ stop: never");
    else                 lv_label_set_text_fmt(s_stop_lbl, "CQ stop after %u", (unsigned)s_stop_val);
}

// Advance to the next listed value above the current one (a custom value from
// a config import lands on the next larger step), wrapping to 0 = never.
static void stop_btn_cb(lv_event_t *e)
{
    (void)e;
    uint8_t next = 0;
    for (size_t i = 0; i < sizeof(s_stop_vals); i++) {
        if (s_stop_vals[i] > s_stop_val) { next = s_stop_vals[i]; break; }
    }
    s_stop_val = next;
    settings_set_cq_max_calls(s_stop_val);
    stop_btn_update_label();
}

static void listen_btn_update_label(void)
{
    if (!s_listen_lbl) return;
    if (s_listen_val == 0) lv_label_set_text(s_listen_lbl, "Listen: never");
    else                   lv_label_set_text_fmt(s_listen_lbl, "Listen every %u",
                                                 (unsigned)s_listen_val);
}

static void listen_btn_cb(lv_event_t *e)
{
    (void)e;
    uint8_t next = 0;
    for (size_t i = 0; i < sizeof(s_listen_vals); i++) {
        if (s_listen_vals[i] > s_listen_val) { next = s_listen_vals[i]; break; }
    }
    s_listen_val = next;
    settings_set_cq_listen_every(s_listen_val);
    listen_btn_update_label();
}

static void radio_clicked_cb(lv_event_t *e)
{
    if (s_fd_locked) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_sel = idx;
    apply_radio_state();
    update_fd_preview();
}

static void ta_value_changed_cb(lv_event_t *e)
{
    (void)e;
    if (s_fd_locked) return;
    update_fd_preview();
}

static void ta_focused_cb(lv_event_t *e)
{
    if (s_fd_locked) return;
    lv_obj_t *ta = lv_event_get_target(e);
    // Editing a field implies you want to use it: select its radio too.
    // Also only one field shows the blinking cursor at a time.
    for (int i = 0; i < N_CQ; i++) {
        if (s_ta[i] == ta) {
            s_sel = i; apply_radio_state();
        } else {
            lv_obj_remove_state(s_ta[i], LV_STATE_FOCUSED);
        }
    }
    update_fd_preview();
    if (!s_keyboard) return;
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

// Append "<call> <grid>" (from saved identity) to the active field, with a
// leading space only if needed. Every standard CQ ends with this fixed tail,
// so the user just types the prefix ("CQ DX ") and taps this.
static void add_suffix_cb(lv_event_t *e)
{
    (void)e;
    if (s_fd_locked) return;
    if (s_sel < 0 || s_sel >= N_CQ || !s_ta[s_sel]) return;
    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.my_callsign[0] || !s.my_grid[0]) return;  // nothing to add

    lv_obj_t *ta = s_ta[s_sel];
    const char *cur = lv_textarea_get_text(ta);
    size_t len = cur ? strlen(cur) : 0;
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    if (len > 0 && cur[len - 1] != ' ') lv_textarea_add_text(ta, " ");
    char tail[24];
    snprintf(tail, sizeof(tail), "%s %s", s.my_callsign, s.my_grid);
    lv_textarea_add_text(ta, tail);
}

static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_fd_locked) return;  // Cancel is the only button that works while locked
    for (int i = 0; i < N_CQ; i++) {
        char buf[28] = {0};
        const char *raw = s_ta[i] ? lv_textarea_get_text(s_ta[i]) : NULL;
        if (raw) { strncpy(buf, raw, sizeof(buf) - 1); to_upper_inplace(buf); trim_inplace(buf); }
        settings_set_cq_msg((uint8_t)i, buf);
    }
    settings_set_cq_sel((uint8_t)s_sel);
    ESP_LOGI(TAG, "saved CQ presets, active=%d", s_sel);
    modal_close();
    // Refresh the Call CQ button label to the newly-selected message.
    ft8_screen_view_refresh_cq_label();
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
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

    lv_obj_t *panel = lv_obj_create(s_modal);
    // +40 again for the Listen button, which sits under CQ stop: the message
    // rows share the buttons' horizontal band, so they start BELOW both.
    lv_obj_set_size(panel, 1040, 464);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "CQ Messages  (check the active)");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // CQ auto-stop cycle button, top-right. Applies on tap - no Save needed
    // (and deliberately NOT part of the Field Day lock, see the statics).
    s_stop_btn = lv_btn_create(panel);
    lv_obj_set_size(s_stop_btn, 290, 56);
    lv_obj_align(s_stop_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_stop_btn, lv_color_hex(0x2a2f37), 0);
    lv_obj_set_style_border_color(s_stop_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_stop_btn, 2, 0);
    lv_obj_set_style_radius(s_stop_btn, 8, 0);
    lv_obj_add_event_cb(s_stop_btn, stop_btn_cb, LV_EVENT_CLICKED, NULL);
    s_stop_lbl = lv_label_create(s_stop_btn);
    lv_label_set_text(s_stop_lbl, "CQ stop: never");
    lv_obj_set_style_text_color(s_stop_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_stop_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_stop_lbl);

    // Listening-slot cycle button, directly under CQ stop. Same commit-on-tap,
    // same exemption from the Field Day lock.
    s_listen_btn = lv_btn_create(panel);
    lv_obj_set_size(s_listen_btn, 290, 56);
    lv_obj_align(s_listen_btn, LV_ALIGN_TOP_RIGHT, 0, 64);
    lv_obj_set_style_bg_color(s_listen_btn, lv_color_hex(0x2a2f37), 0);
    lv_obj_set_style_border_color(s_listen_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_listen_btn, 2, 0);
    lv_obj_set_style_radius(s_listen_btn, 8, 0);
    lv_obj_add_event_cb(s_listen_btn, listen_btn_cb, LV_EVENT_CLICKED, NULL);
    s_listen_lbl = lv_label_create(s_listen_btn);
    lv_label_set_text(s_listen_lbl, "Listen: never");
    lv_obj_set_style_text_color(s_listen_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_listen_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_listen_lbl);

    // Field Day status note - text/visibility set in show() from live
    // settings, since this modal can be reopened without rebuilding it.
    s_fd_hint = lv_label_create(panel);
    lv_obj_set_width(s_fd_hint, 700);   // clear of the top-right CQ-stop button
    lv_label_set_long_mode(s_fd_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_fd_hint, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_fd_hint, lv_color_hex(0xFF8800), 0);
    lv_obj_align(s_fd_hint, LV_ALIGN_TOP_LEFT, 0, 40);

    // Square themed checkbox indicator — same style as filter modal / drawer.
    static lv_style_t style_ind;
    static lv_style_t style_ind_chk;
    static bool styles_inited = false;
    if (!styles_inited) {
        lv_style_init(&style_ind);
        lv_style_set_bg_color(&style_ind, lv_color_hex(UI_COLOR_SURFACE_RAISED));
        lv_style_set_border_color(&style_ind, lv_color_hex(UI_COLOR_BORDER));
        lv_style_set_border_width(&style_ind, 2);
        lv_style_set_pad_all(&style_ind, 8);
        lv_style_init(&style_ind_chk);
        lv_style_set_bg_color(&style_ind_chk, lv_color_hex(UI_COLOR_PRIMARY));
        lv_style_set_border_color(&style_ind_chk, lv_color_hex(UI_COLOR_PRIMARY_BORDER));
        styles_inited = true;
    }

    for (int i = 0; i < N_CQ; i++) {
        int y = 136 + i * 72;   // clear of the two stacked cycle buttons above

        // Text area first, so the checkbox can be vertically centred against it.
        s_ta[i] = lv_textarea_create(panel);
        lv_obj_set_size(s_ta[i], 900, 60);
        lv_obj_align(s_ta[i], LV_ALIGN_TOP_LEFT, 80, y);
        lv_textarea_set_one_line(s_ta[i], true);
        // 22 chars covers the longest valid standard CQ ("CQ <4ch> <call> <grid>");
        // the ft8_lib encoder is the real gate and rejects anything unpackable.
        lv_textarea_set_max_length(s_ta[i], 22);
        lv_textarea_set_placeholder_text(s_ta[i], "e.g. CQ DX OZ1LAV JO65FR");
        lv_obj_set_style_text_font(s_ta[i], &lv_font_montserrat_24, 0);
        ui_theme_style_textarea(s_ta[i]);
        lv_obj_add_event_cb(s_ta[i], ta_focused_cb, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(s_ta[i], ta_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

        s_radio[i] = lv_checkbox_create(panel);
        lv_checkbox_set_text(s_radio[i], "");
        lv_obj_add_style(s_radio[i], &style_ind,     LV_PART_INDICATOR);
        lv_obj_add_style(s_radio[i], &style_ind_chk, LV_PART_INDICATOR | LV_STATE_CHECKED);
        // Centre the checkbox on the text field's vertical midline.
        lv_obj_align_to(s_radio[i], s_ta[i], LV_ALIGN_OUT_LEFT_MID, -16, 0);
        lv_obj_add_event_cb(s_radio[i], radio_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 220, 64);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 40, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cancel_lbl);

    // Quick-insert: append "<call> <grid>" to the active field. Label is set
    // dynamically in show() from the saved identity.
    s_add_btn = lv_btn_create(panel);
    lv_obj_set_size(s_add_btn, 380, 64);
    lv_obj_align(s_add_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_add_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_radius(s_add_btn, 8, 0);
    lv_obj_add_event_cb(s_add_btn, add_suffix_cb, LV_EVENT_CLICKED, NULL);
    s_add_lbl = lv_label_create(s_add_btn);
    lv_label_set_text(s_add_lbl, "+ Call Grid");
    lv_obj_set_style_text_color(s_add_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_add_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_add_lbl);

    s_save_btn = lv_btn_create(panel);
    lv_obj_set_size(s_save_btn, 220, 64);
    lv_obj_align(s_save_btn, LV_ALIGN_BOTTOM_RIGHT, -40, 0);
    lv_obj_set_style_bg_color(s_save_btn, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_radius(s_save_btn, 8, 0);
    lv_obj_add_event_cb(s_save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(s_save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(save_lbl);

    // Physical keyboard: Enter -> Save, Esc -> Cancel. (save_btn_cb itself
    // still checks s_fd_locked, so Enter can't bypass the lock either.)
    ui_kbd_set_buttons(s_save_btn, cancel_btn);

    s_keyboard = lv_keyboard_create(s_modal);
    static lv_style_t style_kb_btn;
    static bool kb_btn_style_inited = false;
    if (!kb_btn_style_inited) {
        lv_style_init(&style_kb_btn);
        lv_style_set_bg_color(&style_kb_btn, lv_color_hex(UI_COLOR_KEY_BG));
        lv_style_set_bg_opa(&style_kb_btn, LV_OPA_COVER);
        lv_style_set_text_color(&style_kb_btn, lv_color_white());
        lv_style_set_border_width(&style_kb_btn, 1);
        lv_style_set_border_color(&style_kb_btn, lv_color_hex(0x505050));
        kb_btn_style_inited = true;
    }
    lv_obj_add_style(s_keyboard, &style_kb_btn, LV_PART_ITEMS);
    ui_theme_style_keyboard(s_keyboard);
    lv_obj_set_size(s_keyboard, LV_PCT(100), 280);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    // Start in ABC (caps-lock) — CQ messages are always uppercase.
    ui_theme_keyboard_attach_caps_cycle_upper(s_keyboard);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    ESP_LOGI(TAG, "CQ modal built");
}

void ft8_cq_modal_init(void)
{
    modal_build();
}

void ft8_cq_modal_show(void)
{
    modal_build();
    if (s_open) return;

    qmx_settings_t s;
    settings_load_all(&s);
    s_sel = (s.cq_sel <= 2) ? s.cq_sel : 0;
    s_stop_val = s.cq_max_calls;
    s_listen_val = s.cq_listen_every;
    listen_btn_update_label();
    stop_btn_update_label();

    // Quick-insert button shows the actual call+grid (or a hint if unset).
    if (s_add_lbl) {
        char lbl[32];
        if (s.my_callsign[0] && s.my_grid[0]) {
            snprintf(lbl, sizeof(lbl), "+ %s %s", s.my_callsign, s.my_grid);
        } else {
            snprintf(lbl, sizeof(lbl), "+ Call Grid (set ID)");
        }
        lv_label_set_text(s_add_lbl, lbl);
    }

    for (int i = 0; i < N_CQ; i++) {
        if (i == 0 && !s.cq_msg[0][0]) {
            char def[28];
            build_default_cq(def, sizeof(def));
            lv_textarea_set_text(s_ta[0], def);
        } else {
            // Show the NORMALISED text, so a preset already stored with a
            // doubled space repairs itself visibly the next time the editor is
            // opened rather than being quietly corrected on the way to the
            // radio. What is on screen is then what actually goes out.
            char shown[28];
            strncpy(shown, s.cq_msg[i], sizeof(shown) - 1);
            shown[sizeof(shown) - 1] = '\0';
            trim_inplace(shown);
            lv_textarea_set_text(s_ta[i], shown);
        }
    }
    apply_radio_state();
    ui_theme_focus_textarea(s_ta[0]);

    // Field Day mode: dim the presets (their modifier word is overridden
    // either way) and show a live, always-accurate preview of what Call CQ
    // will actually transmit for the active preset.
    apply_fd_dim(s.field_day_en);
    update_fd_preview();

    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
    ESP_LOGI(TAG, "CQ modal shown (active=%d)", s_sel);
}
