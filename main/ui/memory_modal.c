// Memory channels modal - 4x8 grid of frequency/mode/label slots.
// Tap occupied = recall. Long-press + lift in place = Edit/Delete/Cancel.
// Long-press + drag = move a channel to an empty slot, snaps on release.
// Cell colour matches the mode (see ui_theme_mode_color), same palette the
// freq keypad's DiGi/USB/LSB/CW mode row uses.
#include "memory_modal.h"
#include "ui_theme.h"
#include "mem_channels.h"
#include "cat.h"
#include "ui.h"
#include "ui_mode.h"   // ui_mode_get() / UI_MODE_FT8 — restrict recall to DiGi in FT8/FT4
#include "display.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "mem_modal";

#define PANEL_W     1200
#define PANEL_H      660
#define PAD           16
#define TITLE_H       48
#define CELL_W       282
#define CELL_H        64
#define CELL_GAP       6

// The last cell is a permanent wastebin, not a 32nd storage slot: drag any
// occupied channel's button onto it to delete that channel (with a "poof"
// animation). Always takes over this cell regardless of whether it happens
// to hold data from before this feature existed (deliberate simplicity -
// see memory project_memory_channel_defaults_and_wastebin for the tradeoff).
#define MEM_TRASH_IDX (MEM_SLOTS - 1)
#define COLS           4
#define ROWS           8

static lv_obj_t *s_modal       = NULL;
static lv_obj_t *s_panel       = NULL;
static lv_obj_t *s_grid        = NULL;
static lv_obj_t *s_cell_btn[MEM_SLOTS];
static lv_obj_t *s_cell_lbl[MEM_SLOTS];   /* top line: memory name      */
static lv_obj_t *s_cell_lbl2[MEM_SLOTS];  /* bottom line: mode + freq   */
static bool      s_open        = false;

/* Action menu for long-press occupied */
static lv_obj_t *s_action_panel = NULL;
static lv_obj_t *s_action_ta    = NULL;  /* label edit textarea */
static lv_obj_t *s_action_kb    = NULL;
static int       s_action_idx   = -1;

/* Frequency confirmed/edited via the freq keypad before naming a slot */
static uint32_t  s_pending_freq_hz = 0;
static char      s_pending_mode[8] = "";

/* Long-press drag-to-move: a long press alone arms the gesture (button
 * follows the finger once moved past DRAG_THRESHOLD_PX); lifting without
 * having dragged opens the edit panel instead (old long-press behaviour,
 * now deferred to release per groups.io item from Samuel/Dirk's general
 * "long-press feels accidental" feedback class - see feedback_grep_the_bug_class
 * pattern: row-select had the same press-vs-drag ambiguity, fixed the same way). */
#define DRAG_THRESHOLD_PX 14
static int       s_press_idx        = -1;   /* slot armed by LONG_PRESSED, -1 = none */
static bool      s_press_dragging   = false;
static lv_point_t s_press_start_pt;
static lv_point_t s_press_orig_pos;          /* button's original grid x,y (its own slot) */
static bool      s_skip_next_click  = false; /* swallow the CLICKED that follows a drag release */

#define MODAL_SLIDE_TIME_MS 250

static int s_drag_start_y = -1;
#define DRAG_CLOSE_MIN_DY 60

static void modal_anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

// Forward decls: the real drag-to-trash delete animation and the grid
// repaint function, both defined further down. The demo block (below) is
// defined early in the file but needs both - play_trash_delete_anim so the
// "sacrifice" channel disappears exactly the same way a user's own
// drag-to-trash would, and memory_modal_refresh to show the freshly-seeded
// bait channel before animating it.
static void play_trash_delete_anim(lv_obj_t *btn);
static void memory_modal_refresh(void);

// One-time "look, these can be moved (and deleted)" intro: cell 13 visits
// cell 06 then cell 01 then returns home; then cell 15 does the same; then
// (if slot 23 is free) a temporary "Waste-land" channel seeded just for this
// demo slides into the wastebin and is deleted for real, teaching that
// gesture too. Three acts total, ~1s pause between each. The first two acts
// are purely cosmetic (slide LVGL objects, never touch channel data); the
// third genuinely creates then deletes slot 23's data, self-cleaning by
// design. Gated by mem_channels_demo_shown()/mem_channels_mark_demo_shown()
// to play at most once ever per device, on whichever visit to this page
// happens first.
#define DEMO_LEG_MS        550
#define DEMO_PAUSE_MS     1000
#define MEM_WASTELAND_IDX   22   // slot 23, 1-based - demo-only, never a permanent default
static const int s_demo_tour_slots[] = { 12, 14 };  // "13" then "15", 0-based
static int       s_demo_seq_pos  = -1;   // -1 = not running; index into s_demo_tour_slots while touring
static int       s_demo_btn_idx  = -1;   // slot whose button is currently touring
static lv_point_t s_demo_home;           // that button's own grid position
static bool      s_demo_bait_placed = false;  // true if slot 23 got seeded for this demo run's third act

static void demo_anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void cell_grid_xy(int idx, int *x, int *y)
{
    *x = (idx % COLS) * (CELL_W + CELL_GAP);
    *y = (idx / COLS) * (CELL_H + CELL_GAP);
}

static void demo_move_to(lv_obj_t *btn, int x, int y, uint32_t delay_ms,
                          void (*ready_cb)(lv_anim_t *))
{
    lv_anim_t ax, ay;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, btn);
    lv_anim_set_exec_cb(&ax, demo_anim_x_cb);
    lv_anim_set_values(&ax, lv_obj_get_x(btn), x);
    lv_anim_set_time(&ax, DEMO_LEG_MS);
    lv_anim_set_delay(&ax, delay_ms);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in_out);
    lv_anim_start(&ax);

    lv_anim_init(&ay);
    lv_anim_set_var(&ay, btn);
    lv_anim_set_exec_cb(&ay, modal_anim_y_cb);
    lv_anim_set_values(&ay, lv_obj_get_y(btn), y);
    lv_anim_set_time(&ay, DEMO_LEG_MS);
    lv_anim_set_delay(&ay, delay_ms);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_in_out);
    if (ready_cb) lv_anim_set_ready_cb(&ay, ready_cb);
    lv_anim_start(&ay);
}

static void demo_start_next_button(void);
static void demo_leg1_done_cb(lv_anim_t *a);
static void demo_leg2_done_cb(lv_anim_t *a);

static void demo_leg3_done_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_t *btn = s_cell_btn[s_demo_btn_idx];
    lv_obj_set_style_border_color(btn, lv_color_hex(0x404040), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    s_demo_btn_idx = -1;
    s_demo_seq_pos++;
    demo_start_next_button();
}

static void demo_leg2_done_cb(lv_anim_t *a)
{
    (void)a;
    demo_move_to(s_cell_btn[s_demo_btn_idx], s_demo_home.x, s_demo_home.y, 0, demo_leg3_done_cb);
}

static void demo_leg1_done_cb(lv_anim_t *a)
{
    (void)a;
    int x, y;
    cell_grid_xy(0, &x, &y);   // cell "01"
    demo_move_to(s_cell_btn[s_demo_btn_idx], x, y, 0, demo_leg2_done_cb);
}

// Third act: the seeded "Waste-land" bait has arrived at the trash cell's
// position - hand off to the real delete animation (fade + clear + snap
// back + refresh), exactly as a genuine user drag-to-trash would use.
// Nothing further to chain afterward, so the demo sequence just ends here.
static void demo_wasteland_arrived_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_t *btn = s_cell_btn[MEM_WASTELAND_IDX];
    // Match the manual drag-to-trash path: clear the "actively moving" gold
    // border before handing off to the delete animation (memory_modal_refresh
    // also resets this defensively once the fade completes, but this mirrors
    // cell_press_state_cb's own pattern at the equivalent handoff point).
    lv_obj_set_style_border_color(btn, lv_color_hex(0x404040), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    play_trash_delete_anim(btn);
    s_demo_btn_idx = -1;
    s_demo_seq_pos = -1;
}

static void demo_start_next_button(void)
{
    int n = (int)(sizeof(s_demo_tour_slots) / sizeof(s_demo_tour_slots[0]));

    if (s_demo_seq_pos < n) {
        int idx = s_demo_tour_slots[s_demo_seq_pos];
        s_demo_btn_idx = idx;
        lv_obj_t *btn = s_cell_btn[idx];
        s_demo_home.x = (lv_coord_t)lv_obj_get_x(btn);
        s_demo_home.y = (lv_coord_t)lv_obj_get_y(btn);

        lv_obj_move_foreground(btn);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
        lv_obj_set_style_border_width(btn, 3, 0);

        int x, y;
        cell_grid_xy(5, &x, &y);   // cell "06"
        // First leg of the whole sequence waits for the modal's own
        // slide-in to finish (MODAL_SLIDE_TIME_MS) plus a short beat, so it
        // doesn't fight that animation visually; every later leg uses the
        // full DEMO_PAUSE_MS instead.
        uint32_t delay = (s_demo_seq_pos == 0) ? (MODAL_SLIDE_TIME_MS + 200) : DEMO_PAUSE_MS;
        demo_move_to(btn, x, y, delay, demo_leg1_done_cb);
        return;
    }

    if (s_demo_seq_pos == n && s_demo_bait_placed) {
        s_demo_seq_pos++;   // consume this slot so we can't re-enter it
        s_demo_btn_idx = MEM_WASTELAND_IDX;
        lv_obj_t *btn = s_cell_btn[MEM_WASTELAND_IDX];
        lv_obj_move_foreground(btn);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
        lv_obj_set_style_border_width(btn, 3, 0);

        int x, y;
        cell_grid_xy(MEM_TRASH_IDX, &x, &y);
        demo_move_to(btn, x, y, DEMO_PAUSE_MS, demo_wasteland_arrived_cb);
        return;
    }

    s_demo_seq_pos = -1;  // sequence fully complete
}

// Called once from memory_modal_show(). See the block comment above.
static void maybe_play_move_demo(void)
{
    if (mem_channels_demo_shown()) return;
    mem_channels_mark_demo_shown();

    // Seed the "Waste-land" bait channel for the third act, but only if
    // slot 23 doesn't already hold real user data - same non-destructive
    // rule as the permanent default channels in mem_channels.c. If it's
    // occupied, the demo just skips the third act (2 legs instead of 3).
    mem_slot_t existing;
    mem_channels_get(MEM_WASTELAND_IDX, &existing);
    s_demo_bait_placed = false;
    if (!existing.occupied) {
        mem_slot_t bait = { 0 };
        bait.freq_hz = 21190000;
        strncpy(bait.mode, "DiGi", sizeof(bait.mode) - 1);
        strncpy(bait.label, "Waste-land", sizeof(bait.label) - 1);
        bait.occupied = 1;
        mem_channels_set(MEM_WASTELAND_IDX, &bait);
        s_demo_bait_placed = true;
        memory_modal_refresh();   // show the freshly-seeded bait before animating it
    }

    s_demo_seq_pos = 0;
    demo_start_next_button();
}

static void modal_close_ready_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_modal, 0);
}

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    s_open = false;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_modal);
    lv_anim_set_exec_cb(&a, modal_anim_y_cb);
    lv_anim_set_values(&a, 0, DISPLAY_V_RES);
    lv_anim_set_time(&a, MODAL_SLIDE_TIME_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, modal_close_ready_cb);
    lv_anim_start(&a);
}

// Swipe down (drag down) anywhere on the modal background closes it,
// replacing the Close button.
static void modal_swipe_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_drag_start_y = (int)p.y;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (s_drag_start_y >= 0 &&
            (int)p.y - s_drag_start_y >= DRAG_CLOSE_MIN_DY) {
            modal_close();
        }
        s_drag_start_y = -1;
    }
}

/* Format hz as "12.345.678 Hz" (dot every 3 digits from the right). */
static void format_freq_hz(uint32_t hz, char *out, size_t out_sz)
{
    char digits[12];
    snprintf(digits, sizeof(digits), "%lu", (unsigned long)hz);
    size_t len = strlen(digits);

    int oi = 0;
    for (size_t i = 0; i < len && (size_t)oi < out_sz - 1; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[oi++] = '.';
        if ((size_t)oi < out_sz - 1) out[oi++] = digits[i];
    }
    out[oi] = '\0';
    snprintf(out + oi, out_sz - (size_t)oi, " Hz");
}

// In FT8/FT4 mode the QMX must stay in its DiGi data mode, so recalling a
// non-DiGi memory (CW, USB, ...) makes no sense — it would knock the radio out
// of data mode mid-session. Such slots are greyed out in the grid and their
// recall (tap) is blocked. Both FT8 and FT4 run under UI_MODE_FT8. Editing,
// deleting and drag-moving those slots is still allowed.
static bool mem_recall_blocked(const mem_slot_t *slot)
{
    return slot->occupied &&
           ui_mode_get() == UI_MODE_FT8 &&
           strcmp(slot->mode, "DiGi") != 0;
}

static void memory_modal_refresh(void)
{
    for (int i = 0; i < MEM_SLOTS; i++) {
        if (i == MEM_TRASH_IDX) {
            // Always the wastebin icon, regardless of any underlying slot
            // data - see MEM_TRASH_IDX's comment.
            lv_obj_t *btn  = s_cell_btn[i];
            lv_obj_t *lbl  = s_cell_lbl[i];
            lv_obj_t *lbl2 = s_cell_lbl2[i];
            if (!btn || !lbl || !lbl2) continue;
            lv_label_set_text(lbl, LV_SYMBOL_TRASH);
            lv_label_set_text(lbl2, "");
            lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0x404040), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
            continue;
        }

        mem_slot_t slot;
        mem_channels_get(i, &slot);

        lv_obj_t *btn  = s_cell_btn[i];
        lv_obj_t *lbl  = s_cell_lbl[i];
        lv_obj_t *lbl2 = s_cell_lbl2[i];
        if (!btn || !lbl || !lbl2) continue;

        // Always reset to the normal border before the occupied/empty
        // branches below - a button that arrives here via the wastebin
        // (dragged manually, or animated by the demo) may still have its
        // gold "actively moving" border set from cell_press_state_cb or
        // demo_start_next_button, and neither branch below otherwise touches
        // border style, so a stray highlight would linger forever on what's
        // now an ordinary (usually empty) cell.
        lv_obj_set_style_border_color(btn, lv_color_hex(0x404040), 0);
        lv_obj_set_style_border_width(btn, 1, 0);

        if (slot.occupied) {
            char freq_str[20];
            format_freq_hz(slot.freq_hz, freq_str, sizeof(freq_str));

            char buf2[32];
            snprintf(buf2, sizeof(buf2), "%s   %s", freq_str, slot.mode);

            lv_label_set_text(lbl, slot.label[0] ? slot.label : freq_str);
            lv_label_set_text(lbl2, slot.label[0] ? buf2 : slot.mode);
            if (mem_recall_blocked(&slot)) {
                // Not recallable in FT8/FT4 (non-DiGi): grey it right out so it
                // reads as unavailable; the tap is also blocked in cell_tap_cb.
                lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
                lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
                lv_obj_set_style_text_color(lbl2, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
                lv_obj_set_style_opa(btn, LV_OPA_40, 0);
            } else {
                lv_obj_set_style_bg_color(btn, lv_color_hex(ui_theme_mode_color(slot.mode)), 0);
                lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
                lv_obj_set_style_text_color(lbl2, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
                lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
            }
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02d", i + 1);
            lv_label_set_text(lbl, buf);
            lv_label_set_text(lbl2, "");
            lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_text_color(lbl2, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
        }
    }
}

/* Action menu callbacks */
static void action_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    s_action_idx = -1;
}

static void action_ta_focused_cb(lv_event_t *e)
{
    (void)e;

    if (!s_action_kb) return;
    lv_keyboard_set_textarea(s_action_kb, s_action_ta);
    lv_obj_clear_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_action_kb);
}

static void action_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
}

static void action_delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_action_idx < 0) return;
    mem_channels_clear(s_action_idx);
    ESP_LOGI(TAG, "deleted slot %d", s_action_idx);
    lv_obj_add_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    s_action_idx = -1;
    memory_modal_refresh();
}

static void action_save_cb(lv_event_t *e)
{
    (void)e;
    if (s_action_idx < 0) return;
    mem_slot_t slot = {0};
    mem_channels_get(s_action_idx, &slot);

    if (s_pending_freq_hz == 0) {
        ESP_LOGW(TAG, "save: no freq");
        return;
    }
    // Band validation already happened in mem_freq_picker_cb, right when the
    // freq pad was confirmed - s_pending_freq_hz can't reach here out of band.
    slot.freq_hz = s_pending_freq_hz;
    slot.occupied = 1;
    strncpy(slot.mode, s_pending_mode[0] ? s_pending_mode : "???", sizeof(slot.mode) - 1);
    slot.mode[sizeof(slot.mode) - 1] = '\0';

    /* Both new and edit: update label from textarea */
    const char *new_label = lv_textarea_get_text(s_action_ta);
    if (new_label) strncpy(slot.label, new_label, sizeof(slot.label) - 1);

    mem_channels_set(s_action_idx, &slot);
    ESP_LOGI(TAG, "saved slot %d: %lu Hz %s '%s'",
             s_action_idx, (unsigned long)slot.freq_hz, slot.mode, slot.label);

    lv_obj_add_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    s_action_idx = -1;
    memory_modal_refresh();
}

static void open_edit_panel(int idx);  /* forward decl - defined below, also called from cell_tap_cb for empty slots */

static void cell_tap_cb(lv_event_t *e)
{
    if (s_skip_next_click) { s_skip_next_click = false; return; }  /* a long-press/drag release already handled this gesture */
    if (s_action_idx >= 0) return;  /* suppress tap if long-press action panel is open */
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= MEM_SLOTS) return;
    if (idx == MEM_TRASH_IDX) return;  /* not a real slot - tap does nothing */

    mem_slot_t slot;
    mem_channels_get(idx, &slot);
    if (!slot.occupied) {
        // Empty slot: a plain tap is unambiguous (there's nothing to recall
        // or drag), so skip the long-press requirement and go straight to
        // the save-new-channel editor.
        open_edit_panel(idx);
        return;
    }

    if (mem_recall_blocked(&slot)) {
        // Non-DiGi memory tapped while the FT8/FT4 screen is up — recalling it
        // would drop the radio out of data mode. Refuse (the cell is greyed).
        ESP_LOGI(TAG, "recall slot %d blocked in FT8/FT4 (mode '%s' != DiGi)", idx, slot.mode);
        ui_toast("FT8/FT4: only DiGi memories can be recalled");
        return;
    }

    ESP_LOGI(TAG, "recall slot %d: %lu Hz %s '%s'",
             idx, (unsigned long)slot.freq_hz, slot.mode, slot.label);

    cat_set_frequency_forced(slot.freq_hz);  // deliberate user action — bypass the 200ms rate-limiter so it always lands
    // Optimistically move the Tab5 display now instead of waiting for the FA
    // poll: the mode write below can briefly garble FA responses on the shared
    // CDC pipe, so a poll-only update could leave the display frozen on the old
    // freq (Ian G4LXX, v0.18.0). Same pattern the freq keypad already uses.
    ui_update_frequency(slot.freq_hz);
    // Mode via the poll task (deferred), NOT a direct LVGL-thread cat_set_mode:
    // the old timer-driven cat_set_mode raced the FA/MD/FW poll on the CDC pipe.
    if (slot.mode[0]) cat_request_mode(slot.mode);
    modal_close();
}

static void show_action_panel(int idx)
{
    mem_slot_t slot;
    mem_channels_get(idx, &slot);

    lv_obj_t *ttl = lv_obj_get_child(s_action_panel, 0);

    if (slot.occupied) {
        /* Edit occupied slot */
        char title[32];
        snprintf(title, sizeof(title), "M%02d: %s", idx + 1,
                 slot.label[0] ? slot.label : slot.mode);
        if (ttl) lv_label_set_text(ttl, title);
        lv_textarea_set_text(s_action_ta, slot.label);
    } else {
        /* Save new slot */
        char title[32];
        snprintf(title, sizeof(title), "M%02d: New", idx + 1);
        if (ttl) lv_label_set_text(ttl, title);
        lv_textarea_set_text(s_action_ta, "");
    }

    ui_theme_focus_textarea(s_action_ta);
    lv_obj_clear_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_action_panel);
    lv_obj_clear_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_action_kb, s_action_ta);
    lv_obj_move_foreground(s_action_kb);
    // Reset the shift cycle to "Abc" (capitalize the next letter, then
    // lowercase) at the start of every edit session, not just the first.
    ui_theme_kb_apply_state(s_action_kb, 1, ui_theme_kb_find_shift_btn(s_action_kb));
}

/* Called after the frequency keypad is confirmed/cancelled. Return value only
 * matters for the accepted==true (Enter) case: true closes the freq pad and
 * proceeds to the label editor, false rejects and leaves the freq pad open
 * (with whatever the user typed still showing) so they can correct it. */
static bool mem_freq_picker_cb(uint32_t freq_hz, const char *mode, bool accepted)
{
    if (!accepted || s_action_idx < 0) {
        s_action_idx = -1;
        return true;  // Cancel always closes regardless of this return value
    }
    // Validate right here, as soon as the freq pad is confirmed - not after
    // the user has also typed a label and tapped Save.
    if (!ui_validate_band_freq_hz(freq_hz, NULL, NULL)) {
        ESP_LOGW(TAG, "freq pad: %lu Hz is out of band, refusing", (unsigned long)freq_hz);
        ui_toast("Out of band - not saved");
        // Keep s_action_idx armed (don't reset to -1) so the next Enter
        // attempt, once corrected, still targets the same slot.
        return false;
    }
    s_pending_freq_hz = freq_hz;
    strncpy(s_pending_mode, mode && mode[0] ? mode : "???", sizeof(s_pending_mode) - 1);
    s_pending_mode[sizeof(s_pending_mode) - 1] = '\0';
    show_action_panel(s_action_idx);
    return true;
}

static void open_edit_panel(int idx)
{
    if (idx < 0 || idx >= MEM_SLOTS) return;

    mem_slot_t slot;
    mem_channels_get(idx, &slot);
    s_action_idx = idx;

    /* Pre-fill the freq keypad: existing slot's freq+mode when editing, or
     * the QMX's current VFO freq+mode when saving a new slot. The user can
     * change either before naming the slot; the chosen freq+mode are
     * carried through to action_save_cb via mem_freq_picker_cb. */
    uint32_t initial_hz;
    const char *mode;
    if (slot.occupied) {
        initial_hz = slot.freq_hz;
        mode = slot.mode;
    } else {
        initial_hz = cat_get_frequency();
        mode = cat_get_mode_str();
    }

    ui_freq_picker_open(initial_hz, mode, mem_freq_picker_cb);
}

/* Combined long-press / drag-to-move state machine, registered on
 * LV_EVENT_LONG_PRESSED, LV_EVENT_PRESSING and LV_EVENT_RELEASED.
 *
 * LONG_PRESSED arms the gesture (records the start point + the button's
 * own grid position) without acting yet. PRESSING then either does nothing
 * (movement under DRAG_THRESHOLD_PX - still a candidate for "open edit on
 * lift") or, once past the threshold, drags the button to follow the
 * finger. RELEASED decides what actually happened: dragged -> try to move
 * the slot's data to the dropped-on cell (only if that cell is empty;
 * otherwise the drop is rejected and the button just snaps back); not
 * dragged -> open the edit/save panel, same as the old immediate
 * long-press behaviour just deferred to release.
 *
 * The button objects are permanently bound 1:1 to slot indices (set up
 * once in modal_build), so a successful move never relocates the LVGL
 * object itself - it snaps back to its own slot's position and
 * memory_modal_refresh() repaints both the source (now empty) and
 * destination (now occupied) cells from the underlying data. */

// Wastebin delete animation: fade the dropped-on-trash button out in place
// (wherever it currently sits, near the trash cell), then clear its data
// and restore it. Opacity-only - an earlier version also animated
// transform_scale_x/y down to 0 and froze the LVGL/UI thread solid on real
// hardware (no crash, no backtrace - background tasks kept running fine,
// only rendering/touch stopped responding), most likely a degenerate case
// in LVGL's transform-matrix rendering at scale exactly 0. Opacity alone is
// a code path already exercised elsewhere in this codebase with no issues;
// do not reintroduce a scale/zoom transform here without confirming
// LVGL handles scale=0 safely first (test on a throwaway object, off the
// main UI thread's critical path, before wiring it back into this modal).
// Reads idx/home position back out of the button object itself (user_data +
// cell_grid_xy) rather than any shared drag-state variable, so it stays
// correct even if a new press/drag gesture starts on a different button
// before this ~350ms animation finishes.
#define TRASH_ANIM_MS 350

static void trash_anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void trash_anim_done_cb(lv_anim_t *a)
{
    lv_obj_t *btn = (lv_obj_t *)a->var;
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    mem_channels_clear(idx);
    ESP_LOGI(TAG, "slot %d deleted via wastebin", idx);

    lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
    int x, y;
    cell_grid_xy(idx, &x, &y);
    lv_obj_set_pos(btn, x, y);

    memory_modal_refresh();
}

static void play_trash_delete_anim(lv_obj_t *btn)
{
    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, btn);
    lv_anim_set_exec_cb(&a_opa, trash_anim_opa_cb);
    lv_anim_set_values(&a_opa, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a_opa, TRASH_ANIM_MS);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a_opa, trash_anim_done_cb);
    lv_anim_start(&a_opa);
}

static void cell_press_state_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= MEM_SLOTS) return;

    if (code == LV_EVENT_LONG_PRESSED) {
        if (idx == MEM_TRASH_IDX) return;  /* the wastebin is a drop target only, never a drag source */
        s_press_idx = idx;
        s_press_dragging = false;
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) lv_indev_get_point(indev, &s_press_start_pt);
        s_press_orig_pos.x = lv_obj_get_x(btn);
        s_press_orig_pos.y = lv_obj_get_y(btn);
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (s_press_idx != idx) return;
        mem_slot_t slot;
        mem_channels_get(idx, &slot);
        if (!slot.occupied) return;  /* nothing to drag out of an empty slot */

        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int ddx = (int)p.x - (int)s_press_start_pt.x;
        int ddy = (int)p.y - (int)s_press_start_pt.y;

        if (!s_press_dragging) {
            if (abs(ddx) <= DRAG_THRESHOLD_PX && abs(ddy) <= DRAG_THRESHOLD_PX) return;
            s_press_dragging = true;
            lv_obj_move_foreground(btn);
            lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
            lv_obj_set_style_border_width(btn, 3, 0);
            // The grid's own scroll range is tiny (content fits the panel with
            // a small margin) but disable it anyway while dragging so LVGL's
            // scroll-vs-click arbitration can't steal the gesture mid-drag.
            lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
        }

        int nx = s_press_orig_pos.x + ddx;
        int ny = s_press_orig_pos.y + ddy;
        int max_x = (COLS - 1) * (CELL_W + CELL_GAP);
        int max_y = (ROWS - 1) * (CELL_H + CELL_GAP);
        if (nx < 0) nx = 0; else if (nx > max_x) nx = max_x;
        if (ny < 0) ny = 0; else if (ny > max_y) ny = max_y;
        lv_obj_set_pos(btn, nx, ny);
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (s_press_idx != idx) return;
        bool was_dragging = s_press_dragging;
        s_press_idx = -1;
        s_press_dragging = false;

        if (was_dragging) {
            lv_obj_set_style_border_color(btn, lv_color_hex(0x404040), 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_add_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

            int cur_x = lv_obj_get_x(btn), cur_y = lv_obj_get_y(btn);
            int col = (cur_x + (CELL_W + CELL_GAP) / 2) / (CELL_W + CELL_GAP);
            int row = (cur_y + (CELL_H + CELL_GAP) / 2) / (CELL_H + CELL_GAP);
            if (col < 0) col = 0; else if (col >= COLS) col = COLS - 1;
            if (row < 0) row = 0; else if (row >= ROWS) row = ROWS - 1;
            int target_idx = row * COLS + col;

            if (target_idx == MEM_TRASH_IDX) {
                // Dropped on the wastebin: delete with a "poof" animation
                // instead of the normal snap-back-and-move below. The
                // animation's own completion callback clears the data,
                // restores the button's appearance, and snaps it back home.
                play_trash_delete_anim(btn);
                s_skip_next_click = true;
                return;
            }

            // The button always snaps back to its own fixed grid slot - only
            // the data may have moved (see function comment above).
            lv_obj_set_pos(btn, s_press_orig_pos.x, s_press_orig_pos.y);

            if (target_idx != idx) {
                mem_slot_t tgt_slot;
                mem_channels_get(target_idx, &tgt_slot);
                if (!tgt_slot.occupied) {
                    mem_slot_t src_slot;
                    mem_channels_get(idx, &src_slot);
                    mem_channels_set(target_idx, &src_slot);
                    mem_channels_clear(idx);
                    ESP_LOGI(TAG, "moved slot %d -> %d", idx, target_idx);
                } else {
                    ESP_LOGI(TAG, "drop on occupied slot %d rejected, snapped back", target_idx);
                }
            }
            memory_modal_refresh();
            s_skip_next_click = true;
            return;
        }

        /* Long-press + lift in place, no drag: open the edit/save panel. */
        open_edit_panel(idx);
        s_skip_next_click = true;
    }
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
    lv_obj_set_size(s_panel, PANEL_W, PANEL_H);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, PAD, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Memory Channels");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    int grid_w = COLS * CELL_W + (COLS - 1) * CELL_GAP;
    int grid_h = PANEL_H - PAD * 2 - TITLE_H - 8;
    s_grid = lv_obj_create(s_panel);
    lv_obj_set_size(s_grid, grid_w, grid_h);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, TITLE_H + 8);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_ACTIVE);

    for (int i = 0; i < MEM_SLOTS; i++) {
        int col = i % COLS;
        int row = i / COLS;
        int x   = col * (CELL_W + CELL_GAP);
        int y   = row * (CELL_H + CELL_GAP);

        lv_obj_t *btn = lv_btn_create(s_grid);
        lv_obj_set_size(btn, CELL_W, CELL_H);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_KEY_BG), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x404040), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, cell_tap_cb,          LV_EVENT_CLICKED,      NULL);
        lv_obj_add_event_cb(btn, cell_press_state_cb,  LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_event_cb(btn, cell_press_state_cb,  LV_EVENT_PRESSING,     NULL);
        lv_obj_add_event_cb(btn, cell_press_state_cb,  LV_EVENT_RELEASED,     NULL);
        s_cell_btn[i] = btn;

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(lbl, CELL_W - 8, 28);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 2);
        s_cell_lbl[i] = lbl;

        lv_obj_t *lbl2 = lv_label_create(btn);
        lv_label_set_long_mode(lbl2, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(lbl2, CELL_W - 8, 26);
        lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(lbl2, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl2, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_align(lbl2, LV_ALIGN_BOTTOM_MID, 0, -2);
        s_cell_lbl2[i] = lbl2;

        if (i == MEM_TRASH_IDX) {
            // Bigger icon, centered (overriding the top-aligned two-line
            // layout every other cell uses) and nudged up 15px per request -
            // lbl2 stays blank (set in memory_modal_refresh) and unused here.
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_48, 0);
            lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -7);
        }
    }

    // Slim grip handle at the top of the panel: swipe down to close.
    lv_obj_t *grip = lv_obj_create(s_panel);
    lv_obj_set_size(grip, 120, 10);
    lv_obj_align(grip, LV_ALIGN_TOP_MID, 0, -PAD + 4);
    lv_obj_set_style_bg_color(grip, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_30, 0);
    lv_obj_set_style_border_width(grip, 0, 0);
    lv_obj_set_style_radius(grip, 5, 0);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);

    // Swipe down anywhere on the modal background (outside the grid) closes it.
    lv_obj_add_event_cb(s_modal, modal_swipe_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_modal, modal_swipe_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_panel, modal_swipe_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_panel, modal_swipe_cb, LV_EVENT_RELEASED, NULL);

    /* Action panel for long-press */
    s_action_panel = lv_obj_create(s_panel);
    lv_obj_set_size(s_action_panel, 600, 280);
    lv_obj_align(s_action_panel, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_style_bg_color(s_action_panel, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_border_color(s_action_panel, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
    lv_obj_set_style_border_width(s_action_panel, 2, 0);
    lv_obj_set_style_radius(s_action_panel, 10, 0);
    lv_obj_set_style_pad_all(s_action_panel, 12, 0);
    lv_obj_clear_flag(s_action_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *act_title = lv_label_create(s_action_panel);
    lv_label_set_text(act_title, "");
    lv_obj_set_style_text_color(act_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(act_title, &lv_font_montserrat_24, 0);
    lv_obj_align(act_title, LV_ALIGN_TOP_MID, 0, 0);

    s_action_ta = lv_textarea_create(s_action_panel);
    lv_obj_set_size(s_action_ta, 560, 60);
    lv_obj_align(s_action_ta, LV_ALIGN_TOP_MID, 0, 32);
    lv_textarea_set_one_line(s_action_ta, true);
    lv_textarea_set_max_length(s_action_ta, 15);
    lv_obj_set_style_text_font(s_action_ta, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_action_ta);
    lv_obj_add_event_cb(s_action_ta, action_ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *act_cancel = lv_btn_create(s_action_panel);
    lv_obj_set_size(act_cancel, 160, 56);
    lv_obj_align(act_cancel, LV_ALIGN_BOTTOM_LEFT, 20, -12);
    lv_obj_set_style_bg_color(act_cancel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(act_cancel, 8, 0);
    lv_obj_add_event_cb(act_cancel, action_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(act_cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *act_delete = lv_btn_create(s_action_panel);
    lv_obj_set_size(act_delete, 160, 56);
    lv_obj_align(act_delete, LV_ALIGN_BOTTOM_LEFT, 190, -12);
    lv_obj_set_style_bg_color(act_delete, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(act_delete, 8, 0);
    lv_obj_add_event_cb(act_delete, action_delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *delete_lbl = lv_label_create(act_delete);
    lv_label_set_text(delete_lbl, "Delete");
    lv_obj_set_style_text_color(delete_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(delete_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(delete_lbl);

    lv_obj_t *act_save = lv_btn_create(s_action_panel);
    lv_obj_set_size(act_save, 160, 56);
    lv_obj_align(act_save, LV_ALIGN_BOTTOM_RIGHT, -20, -12);
    lv_obj_set_style_bg_color(act_save, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_radius(act_save, 8, 0);
    lv_obj_add_event_cb(act_save, action_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(act_save);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(save_lbl);

    // Physical keyboard: Enter -> Save, Esc -> Cancel (label-edit action panel).
    ui_kbd_set_buttons(act_save, act_cancel);

    lv_obj_add_flag(s_action_panel, LV_OBJ_FLAG_HIDDEN);

    s_action_kb = lv_keyboard_create(s_modal);
    static lv_style_t style_kb_main;
    static lv_style_t style_kb_items;
    static bool kb_inited = false;
    if (!kb_inited) {
        /* Match the frequency keypad's look: dark panel, no border on keys,
         * grey key fill, small radius, even gaps between keys. */
        lv_style_init(&style_kb_main);
        lv_style_set_bg_color(&style_kb_main, lv_color_hex(UI_COLOR_SURFACE));
        lv_style_set_bg_opa(&style_kb_main, LV_OPA_COVER);
        lv_style_set_border_color(&style_kb_main, lv_color_hex(UI_COLOR_BORDER));
        lv_style_set_border_width(&style_kb_main, 1);
        lv_style_set_radius(&style_kb_main, 10);
        lv_style_set_pad_all(&style_kb_main, 12);
        lv_style_set_pad_row(&style_kb_main, 8);
        lv_style_set_pad_column(&style_kb_main, 8);

        lv_style_init(&style_kb_items);
        lv_style_set_bg_color(&style_kb_items, lv_color_hex(UI_COLOR_KEY_BG));
        lv_style_set_bg_opa(&style_kb_items, LV_OPA_COVER);
        lv_style_set_text_color(&style_kb_items, lv_color_white());
        lv_style_set_border_width(&style_kb_items, 0);
        lv_style_set_radius(&style_kb_items, 6);
        kb_inited = true;
    }
    lv_obj_add_style(s_action_kb, &style_kb_main, 0);
    lv_obj_add_style(s_action_kb, &style_kb_items, LV_PART_ITEMS);
    ui_theme_style_keyboard(s_action_kb);
    lv_obj_set_size(s_action_kb, LV_PCT(100), 280);
    lv_obj_align(s_action_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui_theme_keyboard_attach_caps_cycle_pending(s_action_kb);
    lv_obj_set_style_text_font(s_action_kb, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_action_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_action_kb, action_kb_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_action_kb, action_kb_cb, LV_EVENT_CANCEL, NULL);

    ESP_LOGI(TAG, "built");
}

void memory_modal_init(void)
{
    modal_build();
}

void memory_modal_show(void)
{
    modal_build();
    if (s_open) return;
    memory_modal_refresh();
    lv_obj_set_y(s_modal, DISPLAY_V_RES);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_modal);
    lv_anim_set_exec_cb(&a, modal_anim_y_cb);
    lv_anim_set_values(&a, DISPLAY_V_RES, 0);
    lv_anim_set_time(&a, MODAL_SLIDE_TIME_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    maybe_play_move_demo();

    ESP_LOGI(TAG, "shown");
}
