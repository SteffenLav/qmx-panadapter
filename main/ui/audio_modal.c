// One-tap audio panel - see audio_modal.h for whose complaint this answers
// and what deliberately is NOT in it.
//
// EVERY SIZE HERE IS SET FOR POOR EYESIGHT, not for balance. The operator,
// after looking at the first version: "As Sam has poor eye sight and poor
// dexterity - we need to improve this ... All text needs to be much bigger."
// Title 48, labels and buttons 32, the warning 24. The SLIDERS are
// deliberately plain - see make_slider(); dressing them up made them worse.
// The panel grew to
// 940x520 to hold it rather than the text being squeezed to fit the panel -
// which is the mistake that produced the first version.

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
/* T78, Sam W7STF: "Can you add RF gain so we may experiment more with using
 * the pop-up for adjusting the experience between the ears?" - and an AGC
 * selection with it. Both already had working controls; this is placement. */
static lv_obj_t *s_sl_rf = NULL,   *s_lbl_rf = NULL;
static lv_obj_t *s_btn_agc[3] = {0};

/* What Slow / Fast / Off mean here, in the units rx_audio already takes.
 *
 * ⛔ ONE TABLE, and the drawer's two sliders remain the fine control. A
 * three-way button cannot express attack and release independently, so these
 * are presets over the same two numbers, taken from the guidance already in
 * rx_audio.h ("Fast (1) for CW, 3 default, 5 for SSB"; "30 for CW, 150
 * default, 400 for SSB"). Pressing one here overwrites what the drawer's
 * sliders hold - that is the trade for a one-tap control, and the drawer
 * still shows the result. */
typedef struct { const char *name; uint8_t attack_ms; uint16_t release_ms; bool off; } agc_preset_t;
static const agc_preset_t AGC_PRESETS[3] = {
    { "Fast", 1,  30,  false },
    { "Slow", 5,  400, false },
    { "Off",  0,  0,   true  },
};

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

    /* RF gain, same read-back rule as AF below, plus one more reason: RF gain
     * is PER BAND on this radio, so a value from the previous band is not
     * merely stale, it is wrong. */
    int rf = cat_get_rf_gain();
    if (s_lbl_rf) {
        char b[48];
        if (rf < 0) snprintf(b, sizeof(b), "QMX RF gain  --");
        else        snprintf(b, sizeof(b), "QMX RF gain  %d dB", rf);
        lv_label_set_text(s_lbl_rf, b);
    }
    if (s_sl_rf && rf >= 0 && lv_slider_get_value(s_sl_rf) != rf)
        lv_slider_set_value(s_sl_rf, rf, LV_ANIM_OFF);

    /* Which AGC preset is in force. Off is its own flag; otherwise the
     * RELEASE time decides, because that is the one a listener hears. */
    int sel;
    if (settings_get_rxaud_agc_off()) {
        sel = 2;
    } else {
        uint16_t rel = settings_get_rxaud_agc_release_ms();
        sel = (rel <= 100) ? 0 : 1;
    }
    for (int i = 0; i < 3; i++) {
        if (!s_btn_agc[i]) continue;
        bool on = (i == sel);
        lv_obj_set_style_bg_color(s_btn_agc[i],
                                  lv_color_hex(on ? 0x2a4a2a : 0x3a4149), 0);
        lv_obj_set_style_border_color(s_btn_agc[i],
                                      lv_color_hex(on ? 0x5ad05a : 0x5a6169), 0);
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
    lv_obj_set_style_text_font(l, &lv_font_montserrat_32, 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

/* RF gain. VALUE_CHANGED moves the LABEL only; RELEASED is the one that
 * writes - the same rule as the drawer's own slider, and for the same reason:
 * the QMX stores RF gain per band in the operator's configuration, so a drag
 * must not stream sixty writes into it. */
static void rf_cb(lv_event_t *e)
{
    int db = (int)lv_slider_get_value(lv_event_get_target(e));
    if (s_lbl_rf) {
        char b[48];
        snprintf(b, sizeof(b), "QMX RF gain  %d dB", db);
        lv_label_set_text(s_lbl_rf, b);
    }
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    cat_request_rf_gain((uint8_t)db);
    /* RF gain moves the noise floor the panadapter is calibrated against, so
     * flat mode has to re-learn it or the whole trace sits at the wrong
     * height until it drifts back. Same call the drawer makes. */
    ui_flat_mode_reset();
    ESP_LOGI(TAG, "QMX RF gain -> %d dB (this band)", db);
}

static void agc_cb(lv_event_t *e)
{
    /* Which button, by identity rather than by user data: make_btn() does not
     * carry user data, and registering a SECOND callback just to attach an
     * index would fire the first one with NULL and act as button 0. */
    lv_obj_t *t = lv_event_get_target(e);
    int i = -1;
    for (int k = 0; k < 3; k++) if (s_btn_agc[k] == t) i = k;
    if (i < 0) return;
    const agc_preset_t *p = &AGC_PRESETS[i];
    rx_audio_set_agc_off(p->off);
    settings_set_rxaud_agc_off(p->off);
    if (!p->off) {
        rx_audio_set_agc_attack_ms(p->attack_ms);
        rx_audio_set_agc_release_ms(p->release_ms);
        settings_set_rxaud_agc_attack_ms(p->attack_ms);
        settings_set_rxaud_agc_release_ms(p->release_ms);
    }
    ESP_LOGI(TAG, "AGC -> %s", p->name);
    refresh();
}

static lv_obj_t *make_slider(lv_obj_t *parent, int y, int max, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(parent);
    /* ⛔ PLAIN, LIKE EVERY OTHER SLIDER ON THE DEVICE. The first version gave
     * these a 20 px knob pad and hand-picked track colours, on the theory that
     * low vision wants a bigger target. On the glass it was wrong twice over:
     * the knob drew as an outsized disc that overhung the track, and at the
     * bottom of its range it sat outside the bar altogether. The operator,
     * looking at it: "all good except please revert the sliders only on the
     * Audio window".
     *
     * The text around them is what carries the legibility here - that part he
     * kept. A control that looks like nothing else in the UI is not clearer,
     * it is just unfamiliar, and the drawer's sliders are the shape his hands
     * already know. Same construction as those: size, range, callback. */
    lv_obj_set_size(s, 860, 30);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, y);
    lv_slider_set_range(s, 0, max);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* RELEASED as well, for the controls that must commit on release rather
     * than on every tick of a drag (RF gain). A callback that does not care
     * simply never sees a difference - the volume and AF ones return the same
     * value either way. */
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
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
    /* 700 tall, not 520: two more rows went in (T78) and the text sizes are
     * set for poor eyesight, so the panel grows rather than the text
     * shrinking - the mistake the first version of this panel made. */
    lv_obj_set_size(s_panel, 940, 700);
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
    /* 48, not 40: the build carries 14/18/20/22/24/26/28/32/48 and nothing
     * between 32 and 48. Adding a font size costs flash in a binary with 25%
     * left, and 48 is the right end of the choice anyway for the one word
     * that says which panel this is. */
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_lbl_vol = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_lbl_vol, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(0xdddddd), 0);
    lv_obj_align(s_lbl_vol, LV_ALIGN_TOP_LEFT, 0, 74);
    s_sl_vol = make_slider(s_panel, 126, 100, vol_cb);

    s_lbl_af = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_lbl_af, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_af, lv_color_hex(0xdddddd), 0);
    lv_obj_align(s_lbl_af, LV_ALIGN_TOP_LEFT, 0, 206);
    s_sl_af = make_slider(s_panel, 258, CAT_AF_GAIN_DB_MAX, af_cb);

    s_lbl_rf = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_lbl_rf, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_rf, lv_color_hex(0xdddddd), 0);
    lv_obj_align(s_lbl_rf, LV_ALIGN_TOP_LEFT, 0, 360);
    s_sl_rf = make_slider(s_panel, 412, CAT_RF_GAIN_DB_MAX, rf_cb);

    {
        lv_obj_t *agc = lv_label_create(s_panel);
        lv_label_set_text(agc, "AGC");
        lv_obj_set_style_text_font(agc, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(agc, lv_color_hex(0xdddddd), 0);
        lv_obj_align(agc, LV_ALIGN_TOP_LEFT, 0, 446);
        for (int i = 0; i < 3; i++) {
            s_btn_agc[i] = make_btn(s_panel, AGC_PRESETS[i].name, 200, 72,
                                    /* 478, not 492: the inner height is
                                     * 700 - 2*24 = 652 and Mute/Close are
                                     * bottom-aligned at 92 tall, so anything
                                     * past 560 is under them. 478 + 72 = 550
                                     * clears it by 10 px. */
                                    LV_ALIGN_TOP_LEFT, i * 220, 478,
                                    agc_cb, NULL);
        }
    }

    s_lbl_note = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_lbl_note, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_note, lv_color_hex(0xFFC864), 0);
    lv_obj_align(s_lbl_note, LV_ALIGN_TOP_LEFT, 0, 318);
    lv_label_set_text(s_lbl_note, "");
    lv_obj_add_flag(s_lbl_note, LV_OBJ_FLAG_HIDDEN);

    s_btn_mute = make_btn(s_panel, "Mute", 320, 92, LV_ALIGN_BOTTOM_LEFT, 0, 0,
                          mute_cb, &s_lbl_mute);
    lv_obj_t *close = make_btn(s_panel, "Close", 260, 92,
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
    /* And the RF gain, for the stronger version of the same reason: it is
     * stored PER BAND on the radio, so the last value this UI saw belongs to
     * whichever band was in use then. */
    cat_query_rf_gain();
    refresh();
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
}
