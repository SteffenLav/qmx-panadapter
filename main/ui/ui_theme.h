#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/*
 * Shared colour tokens for the panadapter UI. Goal: collapse the many
 * near-duplicate greys/blues that accumulated as screens were added
 * one at a time, without flattening the existing two-tier shape
 * hierarchy (full modals vs. anchored top-bar dropdowns).
 *
 * Staged migration - see CLAUDE.md UI theme notes. This first pass
 * only introduces the "primary" action-blue token and migrates the
 * worst offenders (the "9 blues"). Remaining clusters (gold/orange
 * accent split, grey consolidation, surface/border unification) come
 * in follow-up passes.
 */

/* Surfaces / borders (full modals) */
#define UI_COLOR_SURFACE        0x1c2128
#define UI_COLOR_SURFACE_RAISED 0x252b33
#define UI_COLOR_BORDER         0x555555

/* Primary action blue (drawer presets, FT8 pounce/transmit accents,
 * memory recall, CQ field add button, TX-slot parity buttons). */
#define UI_COLOR_PRIMARY        0x2a6fb0
#define UI_COLOR_PRIMARY_BORDER 0x4a9fe0

/* Status colours */
#define UI_COLOR_SUCCESS        0x2e8b3a
#define UI_COLOR_SUCCESS_BORDER 0x4caf50
#define UI_COLOR_DANGER         0x962020
#define UI_COLOR_DANGER_BORDER  0xc04040

/* Accent - semantic split: gold for passive selection/VFO/highlight,
 * a distinct warm colour reserved for TX-on-air-only cues. */
#define UI_COLOR_ACCENT_GOLD    0xffd700
#define UI_COLOR_TX_ACTIVE      0xff6020

/* Text */
#define UI_COLOR_TEXT           0xffffff
#define UI_COLOR_TEXT_SECONDARY 0xc0c0c0
#define UI_COLOR_TEXT_MUTED     0x808890

/* Neutral keyboard/keypad button background (was a mix of 0x2A2A2A
 * and 0x303030 across the on-screen keyboards and freq keypad). */
#define UI_COLOR_KEY_BG         0x2a2a2a

/* Textareas and keyboards default to LVGL's light theme (near-white)
 * unless explicitly restyled - these helpers apply the dark theme. */
static inline void ui_theme_style_textarea(lv_obj_t *ta)
{
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COLOR_KEY_BG), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(ta, 1, 0);

    /* Blinking line-cursor, shown only in the focused field (not every
     * field at once - that was confusing with multiple text entries
     * on screen). Pair with ui_theme_focus_textarea() to mark the
     * initially-focused field on modal open. */
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_COLOR_ACCENT_GOLD), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(ta, 500, LV_PART_CURSOR | LV_STATE_FOCUSED);
}

/* Mark a textarea as the initially-focused field when a modal opens, so
 * its cursor blinks immediately without the user tapping in first. */
static inline void ui_theme_focus_textarea(lv_obj_t *ta)
{
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
}

static inline void ui_theme_style_keyboard(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(kb, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(kb, 1, 0);

    /* The default theme draws "checked" keys (active mode toggle, shift,
     * etc.) with a bright primary-colour fill - override to keep the same
     * dark key background used everywhere else. CHECKED is repurposed by
     * the shift-cycle below to mark the "Abc" pending-shift state on the
     * shift key only, shown via gold text rather than a highlight fill. */
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_KEY_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_COLOR_ACCENT_GOLD), LV_PART_ITEMS | LV_STATE_CHECKED);
}

/* iPad-style 3-state shift key: abc -> Abc -> ABC -> abc, shown via the
 * shift key's own label text. "Abc" types ONE capital letter then reverts
 * to abc; "ABC" is a caps-lock that stays until pressed again. All keys
 * keep the same dark background in every state - "Abc" is distinguished
 * only by the shift key's label being shown in gold (via LV_STATE_CHECKED,
 * repurposed for this - see ui_theme_style_keyboard).
 *
 * State is stored packed into the keyboard's user_data as
 * (shift_btn_id << 3) | (state << 1) | active, where shift_btn_id is the
 * button-matrix index of the shift key in the TEXT-mode map, state: 0=abc,
 * 1=Abc (pending), 2=ABC, and active marks "the TEXT map currently has our
 * custom shift label applied" (cleared while the keyboard is showing the
 * 1# number/special layout, which has a different map - see
 * ui_theme_kb_shift_cb). */
static inline void ui_theme_kb_apply_state(lv_obj_t *kb, int state, uint32_t shift_btn)
{
    lv_keyboard_mode_t mode = (state == 0) ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER;
    lv_keyboard_set_mode(kb, mode);

    if (shift_btn != LV_BUTTONMATRIX_BUTTON_NONE) {
        static const char *labels[3] = {"abc", "Abc", "ABC"};
        const char * const *map = lv_buttonmatrix_get_map(kb);

        /* Copy the map's pointer array, swapping in our label for the
         * shift key. lv_buttonmatrix_set_map() copies this array into its
         * own storage and (since the button count is unchanged) leaves
         * ctrl_bits - including widths - untouched. */
        static const char *map_buf[64];
        uint32_t n = 0, btn = 0;
        while (map[n][0] != '\0' && n < 62) {
            map_buf[n] = map[n];
            if (map[n][0] != '\n') {
                if (btn == shift_btn) map_buf[n] = labels[state];
                btn++;
            }
            n++;
        }
        map_buf[n] = map[n]; /* "" terminator */

        lv_buttonmatrix_set_map(kb, map_buf);

        /* CHECKED is repurposed (see ui_theme_style_keyboard) to render the
         * shift key's label in gold while in the "Abc" pending state -
         * same background as every other key. */
        lv_buttonmatrix_clear_button_ctrl_all(kb, LV_BUTTONMATRIX_CTRL_CHECKED);
        if (state == 1) lv_buttonmatrix_set_button_ctrl(kb, shift_btn, LV_BUTTONMATRIX_CTRL_CHECKED);
    }

    lv_obj_set_user_data(kb, (void *)(intptr_t)((shift_btn << 3) | ((uint32_t)state << 1) | 1u));
}

/* Find the shift key's button-matrix index in the keyboard's CURRENT map
 * (must be the TEXT lower/upper layout - both place "abc"/"ABC" at the
 * same slot). */
static inline uint32_t ui_theme_kb_find_shift_btn(lv_obj_t *kb)
{
    const char * const *map = lv_buttonmatrix_get_map(kb);
    uint32_t shift_btn = LV_BUTTONMATRIX_BUTTON_NONE, btn = 0;
    for (uint32_t i = 0; map[i][0] != '\0'; i++) {
        if (map[i][0] == '\n') continue;
        if (strcmp(map[i], "abc") == 0 || strcmp(map[i], "ABC") == 0) { shift_btn = btn; break; }
        btn++;
    }
    return shift_btn;
}

/* Sole VALUE_CHANGED handler for a caps-cycle keyboard (LVGL's built-in
 * lv_keyboard_def_event_cb is removed in attach below, and we invoke it
 * ourselves for normal keys). This ordering is the whole point: the built-in
 * handler types ANY button label it doesn't recognise as a control key
 * ("abc"/"ABC"/"1#"/symbols are recognised, nothing else is), so when the shift
 * key shows the pending "Abc" label, the built-in handler would insert literal
 * "Abc" into the field (the v0.15.12 keyboard bug: tapping Abc->ABC typed
 * "Abc"). By driving the shift key entirely here and never passing it to the
 * default handler, the label is never typed - regardless of field state
 * (a delete-after-type fix would corrupt a max-length-full field). */
static inline void ui_theme_kb_shift_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uintptr_t packed = (uintptr_t)lv_obj_get_user_data(kb);
    int active = (int)(packed & 0x1);
    int state = (int)((packed >> 1) & 0x3);
    uint32_t shift_btn = (uint32_t)(packed >> 3);

    uint32_t btn_id = lv_buttonmatrix_get_selected_button(kb);
    if (btn_id == LV_BUTTONMATRIX_BUTTON_NONE) return;

    lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
    bool text_mode = (mode == LV_KEYBOARD_MODE_TEXT_LOWER ||
                      mode == LV_KEYBOARD_MODE_TEXT_UPPER);

    /* The shift key drives our 3-state cycle and must NEVER reach the default
     * handler (which would type its "Abc" pending label). We do the mode +
     * label change ourselves and stop here. */
    if (text_mode && active && btn_id == shift_btn) {
        ui_theme_kb_apply_state(kb, (state + 1) % 3, shift_btn);
        return;
    }

    /* Every other key: let LVGL's built-in handler do its normal thing - type
     * the character (in whatever mode we're in: upper while state 1/2),
     * backspace, OK/ready, or switch to/from the 1# number layout. */
    lv_keyboard_def_event_cb(e);

    lv_keyboard_mode_t new_mode = lv_keyboard_get_mode(kb);
    if (new_mode != LV_KEYBOARD_MODE_TEXT_LOWER &&
        new_mode != LV_KEYBOARD_MODE_TEXT_UPPER) {
        /* Switched to the 1# number/special layout (a different map where our
         * shift index no longer applies): clear the gold pending highlight and
         * mark inactive so the text layout reinitialises to "abc" on return. */
        lv_buttonmatrix_clear_button_ctrl_all(kb, LV_BUTTONMATRIX_CTRL_CHECKED);
        lv_obj_set_user_data(kb, (void *)(intptr_t)((shift_btn << 3) | 0u));
        return;
    }

    if (!active) {
        /* Just returned to the text layout from the number pad (the default
         * handler processed the "abc" key): re-find the shift key in the fresh
         * map and reset the cycle to lowercase. */
        ui_theme_kb_apply_state(kb, 0, ui_theme_kb_find_shift_btn(kb));
        return;
    }

    if (state == 1 && btn_id != shift_btn) {
        /* Pending single-shift consumed by a normal key (the default handler
         * just typed it in upper case): drop back to lowercase for the next. */
        ui_theme_kb_apply_state(kb, 0, shift_btn);
    }
}

/* Attach the iPad-style shift-cycle behaviour to a keyboard. Removes LVGL's
 * built-in VALUE_CHANGED handler and installs ours as the sole handler (which
 * calls the built-in one explicitly for non-shift keys). Always starts at
 * "abc" (lowercase), regardless of the keyboard's mode before this call. */
static inline void ui_theme_keyboard_attach_caps_cycle(lv_obj_t *kb)
{
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    ui_theme_kb_apply_state(kb, 0, ui_theme_kb_find_shift_btn(kb));
    lv_obj_add_event_cb(kb, ui_theme_kb_shift_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

#endif /* UI_THEME_H */
