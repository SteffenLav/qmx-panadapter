// See archive_adopt_modal.h.

#include "archive_adopt_modal.h"
#include "ui_theme.h"
#include "storage/sd_archive.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "arc_adopt";

/* Late enough that the boot's own mount, rotation and worked-index pass are
 * done and the screen has settled - this is a question about a file, not an
 * emergency, and it must never land on top of a boot still drawing itself. */
#define ASK_AFTER_UPTIME_S 45

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_lbl_title = NULL;
static lv_obj_t *s_lbl_body = NULL;

bool archive_adopt_modal_is_open(void)
{
    return s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void close_modal(void)
{
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void use_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGW(TAG, "operator accepted the card's archive");
    sd_archive_adoption_answer(true);
    close_modal();
}

static void leave_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGW(TAG, "operator declined the card's archive");
    sd_archive_adoption_answer(false);
    close_modal();
}

/* A tap beside the panel does NOTHING. Dismissing this by accident would
 * leave the question unanswered AND unremembered, so it would come back on
 * the next boot - the nag the decline marker exists to prevent. */
static void swallow_cb(lv_event_t *e) { (void)e; }

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, uint32_t col, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_center(l);
    return b;
}

/* ⛔ GEOMETRY IS WRITTEN DOWN, because the date window's was not and the
 * explanation ended up printed under the buttons (see date_confirm_modal.c).
 * Panel 920 x 420, pad 28 -> content box 864 x 364:
 *     title    0..40    (font 32)
 *     body    56..200   (font 24, wraps to three lines at width 800)
 *     buttons bottom, h 84, 360 wide, 24 apart */
static void build(void)
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
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_modal, swallow_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *p = lv_obj_create(s_modal);
    lv_obj_set_size(p, 920, 420);
    lv_obj_center(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_pad_all(p, 28, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_title = lv_label_create(p);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_32, 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 0);

    s_lbl_body = lv_label_create(p);
    lv_label_set_long_mode(s_lbl_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_body, 800);
    lv_obj_set_style_text_align(s_lbl_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_lbl_body, lv_color_hex(0xB8C4D0), 0);
    lv_obj_set_style_text_font(s_lbl_body, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_body, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *use = make_btn(p, "Use them", 0x1e6028, 360, 84, use_cb);
    lv_obj_align(use, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *leave = make_btn(p, "Leave it", 0x2a3a4a, 360, 84, leave_cb);
    lv_obj_align(leave, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

void archive_adopt_modal_tick(void)
{
    uint32_t recs = 0, bytes = 0;
    bool offered = sd_archive_adoption_offer(&recs, &bytes);

    if (archive_adopt_modal_is_open()) {
        /* The card was pulled, or something else answered it. Nothing is
         * pending any more, so the question has no subject - take it down
         * rather than leave a button that would act on a card that is gone. */
        if (!offered) close_modal();
        return;
    }
    if (!offered) return;
    if (esp_timer_get_time() < (int64_t)ASK_AFTER_UPTIME_S * 1000000) return;

    build();
    /* ⛔ The title counts lines in a file and claims NOTHING about where they
     * came from. "contacts this radio has never logged" was the first wording
     * and it is not knowable - they may be this radio's own, from before a
     * flash or a clear (operator, 2026-11-14). */
    lv_label_set_text_fmt(s_lbl_title, "%lu contacts on the card", (unsigned long)recs);
    lv_label_set_text(s_lbl_body,
                      "qso-archive.adi holds QSOs that are new to this device. "
                      "Use them as your log history? They stay on the card and "
                      "count as worked before from the next start.\n"
                      "That start is slower.");
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    ESP_LOGW(TAG, "asking: adopt %lu records (%lu B) from the card?",
             (unsigned long)recs, (unsigned long)bytes);
}
