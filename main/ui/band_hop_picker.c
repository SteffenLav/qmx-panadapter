/* The band-hop picker - see band_hop_picker.h. Moved here unchanged in look
 * from wspr_screen_view.c (the WSPR picker the operator already uses). */
#include "band_hop_picker.h"

#include <string.h>

#include "lvgl.h"
#include "ui_theme.h"
#include "cat/cat.h"

#define MAX_CHOICES 16

static lv_obj_t          *s_modal;            /* NULL when closed */
static lv_obj_t          *s_cb[MAX_CHOICES];
static uint8_t            s_bit[MAX_CHOICES];
static int                s_n;
static band_hop_mask_cb_t s_changed;
static void             (*s_closed)(void);

static void toggled_cb(lv_event_t *e)
{
    (void)e;
    uint16_t mask = 0;
    for (int i = 0; i < s_n; i++)
        if (s_cb[i] && lv_obj_has_state(s_cb[i], LV_STATE_CHECKED))
            mask |= (uint16_t)(1u << s_bit[i]);
    if (s_changed) s_changed(mask);
}

static void close_modal(void)
{
    if (!s_modal) return;
    lv_obj_del(s_modal);
    s_modal = NULL;
    for (int i = 0; i < MAX_CHOICES; i++) s_cb[i] = NULL;
    s_n = 0;
    void (*closed)(void) = s_closed;
    s_changed = NULL;
    s_closed  = NULL;
    if (closed) closed();
}

static void close_cb(lv_event_t *e) { (void)e; close_modal(); }

void band_hop_picker_open(const char *title, const char *hint,
                          const band_hop_choice_t *choices, int n,
                          uint16_t mask,
                          band_hop_mask_cb_t changed, void (*closed)(void),
                          void (*extra)(lv_obj_t *panel))
{
    if (s_modal) return;
    if (n > MAX_CHOICES) n = MAX_CHOICES;
    if (n < 0) n = 0;
    s_changed = changed;
    s_closed  = closed;

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, 1280, 720);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    /* A scrim you dismiss, not a control you press - so the mouse pointer
     * stays white over it (ui_theme.h). */
    lv_obj_add_flag(s_modal, UI_FLAG_NOT_HOT);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal, close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_modal);
    if (extra) lv_obj_set_size(panel, BAND_HOP_PANEL_W_EXTRA, 690);   /* widen, never shrink the font */
    else       lv_obj_set_size(panel, 780, 660);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Presses inside the panel must not reach the scrim's dismiss handler. */
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(panel);
    lv_label_set_text(t, title ? title : "Band hop");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_48, 0);   /* one step up from 32: 36 and 40 are not built into this image */
    lv_obj_set_style_text_color(t, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *h = lv_label_create(panel);
    lv_label_set_text(h, hint ? hint : "");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, 70);

    if (n == 0) {
        /* Says which of the two it is. "No bands" with the radio off reads as
         * a broken feature; it is a disconnected radio. */
        lv_obj_t *none = lv_label_create(panel);
        lv_label_set_text(none, cat_is_ready()
            ? "The radio reported no bands."
            : "Waiting for the radio - connect the QMX and reopen this.");
        lv_obj_set_style_text_font(none, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(none, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_align(none, LV_ALIGN_TOP_LEFT, 0, BAND_HOP_EXTRA_Y);
    }

    /* Three columns of finger-sized rows, 170 px x 80 px pitch, 32 px tick box
     * (operator 2026-11-26: two columns at 340 x 64 were too close vertically
     * and too far apart horizontally). The grid before that used 30 px rows
     * and a 20 px box, which is what made it unusable with a finger. */
    s_n = n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *cb = lv_checkbox_create(panel);
        lv_checkbox_set_text(cb, choices[i].name);
        lv_obj_set_style_text_font(cb, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(cb, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_pad_all(cb, 8, 0);
        lv_obj_set_style_width(cb, 32, LV_PART_INDICATOR);
        lv_obj_set_style_height(cb, 32, LV_PART_INDICATOR);
        lv_obj_align(cb, LV_ALIGN_TOP_LEFT, (i % 3) * 170, BAND_HOP_EXTRA_Y + (i / 3) * 80);
        s_bit[i] = choices[i].bit;
        if (mask & (1u << choices[i].bit)) lv_obj_add_state(cb, LV_STATE_CHECKED);
        lv_obj_add_event_cb(cb, toggled_cb, LV_EVENT_VALUE_CHANGED, NULL);
        s_cb[i] = cb;
    }

    if (extra) extra(panel);

    lv_obj_t *done = lv_btn_create(panel);
    lv_obj_set_size(done, 200, 64);
    lv_obj_align(done, extra ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(done, 8, 0);
    lv_obj_add_event_cb(done, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "Done");
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_32, 0);
    lv_obj_center(dl);
}
