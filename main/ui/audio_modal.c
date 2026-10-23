// One-tap audio panel - see audio_modal.h for whose complaint this answers
// and what deliberately is NOT in it.

#include "audio_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "audio/rx_audio.h"
#include "cat/cat.h"
#include "storage/settings.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "audio_modal";

static lv_obj_t *s_modal = NULL, *s_panel = NULL;
static lv_obj_t *s_btn_mute = NULL, *s_lbl_mute = NULL;
static lv_obj_t *s_sl_vol = NULL,  *s_lbl_vol = NULL;
static lv_obj_t *s_sl_af = NULL,   *s_lbl_af = NULL;
static lv_obj_t *s_lbl_note = NULL;

static void refresh(void)
{
    if (!s_modal) return;

    bool muted = rx_audio_is_muted();
    if (s_lbl_mute) lv_label_set_text(s_lbl_mute, muted ? "Unmute" : "Mute");
    if (s_btn_mute) {
        /* Red while muted, because a muted radio looks exactly like a dead one
         * and the panel may be closed again in a second. */
        lv_obj_set_style_bg_color(s_btn_mute,
                                  lv_color_hex(muted ? 0xA02828 : 0x3a4149), 0);
        lv_obj_set_style_border_color(s_btn_mute,
                                      lv_color_hex(muted ? 0xD04848 : 0x5a6169), 0);
    }

    uint8_t vol = rx_audio_get_volume();
    if (s_sl_vol && lv_slider_get_value(s_sl_vol) != vol)
        lv_slider_set_value(s_sl_vol, vol, LV_ANIM_OFF);
    if (s_lbl_vol) {
        char b[48];
        snprintf(b, sizeof(b), "Tab5 speaker  %u%%%s", (unsigned)vol,
                 muted ? "   (muted)" : "");
        lv_label_set_text(s_lbl_vol, b);
    }

    /* The radio's own AF gain, in the dB its LCD shows. -1 means it has not
     * answered AG; yet - say so rather than drawing a slider at a number
     * nobody chose. */
    int ag = cat_get_af_gain();
    if (s_lbl_af) {
        char b[48];
        if (ag < 0) snprintf(b, sizeof(b), "Radio AF gain  --");
        else        snprintf(b, sizeof(b), "Radio AF gain  %d dB", ag / 4);
        lv_label_set_text(s_lbl_af, b);
    }
    if (s_sl_af && ag >= 0) {
        int db = ag / 4;
        if (db > CAT_AF_GAIN_DB_MAX) db = CAT_AF_GAIN_DB_MAX;
        if (lv_slider_get_value(s_sl_af) != db)
            lv_slider_set_value(s_sl_af, db, LV_ANIM_OFF);
    }

    /* ⛔ THE ONE THING THAT WOULD OTHERWISE BE A MYSTERY. If RX audio was off
     * at boot the codec was never opened, so nothing here can make a sound and
     * the sliders would move against silence. Samuel W7STF lost fifteen
     * minutes to exactly that in October - the behaviour was right and the UI
     * said nothing. */
    if (s_lbl_note) {
        if (!rx_audio_is_enabled()) {
            lv_label_set_text(s_lbl_note,
                "RX audio is off - turn it on in the drawer, then reboot.");
            lv_obj_clear_flag(s_lbl_note, LV_OBJ_FLAG_HIDDEN);
        } else if (rx_audio_restart_pending()) {
            lv_label_set_text(s_lbl_note,
                "RX audio was switched on after boot - reboot for sound.");
            lv_obj_clear_flag(s_lbl_note, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_note, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void modal_close(void)
{
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void close_cb(lv_event_t *e) { (void)e; modal_close(); }

static void mute_cb(lv_event_t *e)
{
    (void)e;
    rx_audio_set_muted(!rx_audio_is_muted());
    refresh();
}

static void vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    rx_audio_set_volume((uint8_t)v);
    refresh();
}

static void af_cb(lv_event_t *e)
{
    int db = lv_slider_get_value(lv_event_get_target(e));
    /* The radio takes 0.25 dB steps; the slider is in whole dB because that is
     * what its LCD shows. Deferred to the poll task inside cat.c - a direct
     * write here races the FA/MD/FW poll and comes back "?;". */
    cat_request_af_gain((uint16_t)(db * 4));
    if (s_lbl_af) {
        char b[48];
        snprintf(b, sizeof(b), "Radio AF gain  %d dB", db);
        lv_label_set_text(s_lbl_af, b);
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, int w, int h,
                          lv_align_t align, int x, int y,
                          lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, align, x, y);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

static lv_obj_t *make_slider(lv_obj_t *parent, int y, int max, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, 700, 28);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, y);
    lv_slider_set_range(s, 0, max);
    /* Generous knob: this is adjusted with a finger while listening, which is
     * the same argument Don WB0LQW made about the keyboard. */
    lv_obj_set_style_pad_all(s, 14, LV_PART_KNOB);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
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
    /* Tapping the scrim closes it. The whole point is to be quick to leave as
     * well as quick to reach - Samuel counted the dismissing gesture too. */
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal, close_cb, LV_EVENT_CLICKED, NULL);

    s_panel = lv_obj_create(s_modal);
    lv_obj_set_size(s_panel, 780, 420);
    lv_obj_center(s_panel);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Stop a tap inside the panel from reaching the scrim behind it. */
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Audio");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_lbl_vol = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_lbl_vol, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(0xdddddd), 0);
    lv_obj_align(s_lbl_vol, LV_ALIGN_TOP_LEFT, 0, 60);
    s_sl_vol = make_slider(s_panel, 94, 100, vol_cb);

    s_lbl_af = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_lbl_af, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_af, lv_color_hex(0xdddddd), 0);
    lv_obj_align(s_lbl_af, LV_ALIGN_TOP_LEFT, 0, 150);
    s_sl_af = make_slider(s_panel, 184, CAT_AF_GAIN_DB_MAX, af_cb);

    s_lbl_note = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_lbl_note, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_lbl_note, lv_color_hex(0xFFC864), 0);
    lv_obj_align(s_lbl_note, LV_ALIGN_TOP_LEFT, 0, 230);
    lv_label_set_text(s_lbl_note, "");
    lv_obj_add_flag(s_lbl_note, LV_OBJ_FLAG_HIDDEN);

    s_btn_mute = make_btn(s_panel, "Mute", 260, 72, LV_ALIGN_BOTTOM_LEFT, 0, 0,
                          mute_cb, &s_lbl_mute);
    lv_obj_t *close = make_btn(s_panel, "Close", 200, 72,
                               LV_ALIGN_BOTTOM_RIGHT, 0, 0, close_cb, NULL);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x3a4149), 0);
    lv_obj_set_style_border_color(close, lv_color_hex(0x5a6169), 0);

    ESP_LOGI(TAG, "Audio modal built");
}

void audio_modal_init(void) { modal_build(); }

bool audio_modal_is_open(void)
{
    return s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

void audio_modal_show(void)
{
    modal_build();
    /* Ask the radio what its gain actually is, so the slider shows the radio
     * rather than the last thing this UI sent - including a change made on the
     * QMX's own encoder. The answer lands asynchronously; refresh() draws
     * "--" until it does. */
    cat_query_af_gain();
    refresh();
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
}
