// Manual CW/SSB QSO entry - see manual_log_modal.h.
//
// Structurally based on js8_text_modal.c (modal + textareas + the project's
// on-screen keyboard), extended to three fields.
//
// ⭐ THREE FIELDS, AND NOT ONE MORE. Brian KB9WFS asked for exactly this and
// he is right: "you only need the worked station call and RST. The rest is
// there already." Date, time, frequency, band, mode, our callsign, our grid
// and the running activation all come from the device at the moment Log is
// pressed. Anything the Tab5 already knows and asks for anyway is a chance to
// log it wrong.
//
// ⭐ THE REPORTS ARE PRE-FILLED FROM THE MODE. CW defaults to 599 and a phone
// mode to 59, which is what the great majority of contacts actually exchange -
// so the common case is: type the call, press Log. They stay editable, and
// clearing one logs no report rather than a wrong one.
//
// ⭐ IT STAYS OPEN AFTER LOGGING. A run of CW contacts is not one dialog at a
// time: Log writes the record, clears the callsign, keeps the reports, and
// says what it wrote. Closing is a separate, deliberate press.

#include "manual_log_modal.h"
#include "activation_modal.h"
#include "adif/adif_log.h"
#include "cat/cat.h"
#include "storage/settings.h"
#include "ui_theme.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "manual_log";

static lv_obj_t *s_modal    = NULL;
static lv_obj_t *s_panel    = NULL;
static lv_obj_t *s_ta_call  = NULL;
static lv_obj_t *s_ta_sent  = NULL;
static lv_obj_t *s_ta_rcvd  = NULL;
static lv_obj_t *s_hdr_lbl  = NULL;   /* freq / mode / UTC, refreshed on show */
static lv_obj_t *s_note_lbl = NULL;   /* what was just logged, or why not */
static lv_obj_t *s_btn_log  = NULL;
static lv_obj_t *s_keyboard = NULL;
static bool      s_open     = false;

/* ⛔ THE MODE IS THE RADIO'S, NOT A GUESS FROM THE FREQUENCY. cat_get_mode_str()
 * is what the top bar shows, so the logged mode is the one the operator can see
 * while typing. A dead CAT link leaves it empty, and the Log button refuses
 * rather than writing a record with no MODE field - an ADIF record without one
 * is rejected by LoTW and is awkward to repair by hand later. */
static const char *radio_mode(void)
{
    const char *m = cat_get_mode_str();
    return (m && m[0]) ? m : "";
}

/* "599" for CW, "59" for anything else. The QMX's mode strings are "CW",
 * "CW-R", "USB", "LSB", "DiGi" and "AM"; only the CW pair take the readability
 * -strength-tone form. */
static const char *default_rst(void)
{
    const char *m = radio_mode();
    return (m[0] == 'C' || m[0] == 'c') ? "599" : "59";
}

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

/* Trim both ends and upper-case in place - a callsign typed with the keyboard's
 * caps off is the same callsign, and ADIF wants it upper. */
static void clean_call(const char *in, char *out, size_t out_sz)
{
    size_t n = 0;
    if (out_sz == 0) return;
    while (*in && isspace((unsigned char)*in)) in++;
    while (*in && n + 1 < out_sz) {
        unsigned char c = (unsigned char)*in++;
        out[n++] = (char)toupper(c);
    }
    while (n > 0 && isspace((unsigned char)out[n - 1])) n--;
    out[n] = '\0';
}

/* Enabled only once there is something to log. A disabled Log button is the
 * honest answer to an empty field - more honest than logging a blank call and
 * leaving the operator to find it in the log later. */
static void refresh_log_enabled(void)
{
    if (!s_btn_log || !s_ta_call) return;
    char call[32];
    clean_call(lv_textarea_get_text(s_ta_call), call, sizeof(call));
    bool ok = call[0] && radio_mode()[0];
    if (ok) lv_obj_clear_state(s_btn_log, LV_STATE_DISABLED);
    else    lv_obj_add_state(s_btn_log, LV_STATE_DISABLED);
}

static void ta_changed_cb(lv_event_t *e)
{
    (void)e;
    refresh_log_enabled();
}

/* The keyboard follows whichever field was tapped. Same reasoning as
 * js8_text_modal.c: re-aligning and re-showing on every focus is what stops the
 * keyboard disappearing for good after a close. */
static void ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!s_keyboard) return;
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, ta);
    ui_osk_show(s_keyboard);
}

static void header_refresh(void)
{
    if (!s_hdr_lbl) return;
    uint32_t hz = ui_get_dial_freq_hz();
    const char *mode = radio_mode();
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char b[96];
    snprintf(b, sizeof(b), "%.3f MHz   %s   %02d:%02dZ",
             hz / 1e6, mode[0] ? mode : "no CAT", tm.tm_hour, tm.tm_min);
    lv_label_set_text(s_hdr_lbl, b);
}

static void note_set(const char *txt, uint32_t col)
{
    if (!s_note_lbl) return;
    lv_label_set_text(s_note_lbl, txt);
    lv_obj_set_style_text_color(s_note_lbl, lv_color_hex(col), 0);
}

static void log_cb(lv_event_t *e)
{
    (void)e;
    char call[32];
    clean_call(lv_textarea_get_text(s_ta_call), call, sizeof(call));
    if (!call[0]) return;

    const char *mode = radio_mode();
    if (!mode[0]) {
        /* No CAT means no mode and no frequency worth writing. Refusing is the
         * only honest option: a record with the wrong band is worse in a log
         * than a record that was never made. */
        note_set("No CAT - the radio must be connected to log", UI_COLOR_DANGER);
        return;
    }

    qmx_settings_t cfg;
    settings_load_all(&cfg);

    const char *sent = lv_textarea_get_text(s_ta_sent);
    const char *rcvd = lv_textarea_get_text(s_ta_rcvd);

    adif_qso_t qso = {
        .their_call = call,
        .my_call    = cfg.my_callsign,
        .my_grid    = cfg.my_grid,
        .their_grid = NULL,
        .freq_hz    = ui_get_dial_freq_hz(),
        .mode       = mode,
        /* An emptied field means "no report exchanged", which is a real state
         * in CW contesting and in a quick SSB contact. adif_log_record() omits
         * the field rather than writing an empty one. */
        .rst_sent   = (sent && sent[0]) ? sent : NULL,
        .rst_rcvd   = (rcvd && rcvd[0]) ? rcvd : NULL,
        .qso_time   = time(NULL),
    };
    /* OUR activation is NOT set here on purpose - adif_log_record() stamps it
     * from settings, so a hand-typed QSO cannot miss the park the automatic
     * ones carry. See the comment on adif_qso_t. */
    adif_log_record(&qso);
    ESP_LOGI(TAG, "logged %s %s %s/%s at %lu Hz", call, mode,
             qso.rst_sent ? qso.rst_sent : "-", qso.rst_rcvd ? qso.rst_rcvd : "-",
             (unsigned long)qso.freq_hz);

    /* Say what went in, and - while activating - how the count stands. That
     * number is the one an activator is counting in the field, so showing it
     * here saves opening the Activation window to find out. */
    char note[96], ref[24];
    if (activation_is_running() && settings_get_activation_ref(ref, sizeof(ref))) {
        int n = adif_log_count_activation(ref);
        int target = (settings_get_activation_type() == 2) ? 4 : 10;
        snprintf(note, sizeof(note), "Logged %s - %d/%d for %s", call, n, target, ref);
    } else {
        snprintf(note, sizeof(note), "Logged %s - %d in the log", call, adif_log_count());
    }
    note_set(note, UI_COLOR_SUCCESS);

    /* Ready for the next one: the call goes, the reports stay. In a run they
     * are the same report every time, and re-typing 599 for each contact is
     * the thing this window exists to avoid. */
    lv_textarea_set_text(s_ta_call, "");
    header_refresh();
    refresh_log_enabled();
    lv_keyboard_set_textarea(s_keyboard, s_ta_call);
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

/* The keyboard's own tick and cross: READY logs, CANCEL closes - the same
 * meaning they have in every other modal here. */
static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (!lv_obj_has_state(s_btn_log, LV_STATE_DISABLED)) log_cb(NULL);
    } else if (code == LV_EVENT_CANCEL) {
        modal_close();
    }
}

/* One field: label above, textarea below, at x with width w. */
static lv_obj_t *field_create(const char *label, int x, int y, int w, int maxlen)
{
    lv_obj_t *l = lv_label_create(s_panel);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);

    lv_obj_t *ta = lv_textarea_create(s_panel);
    lv_obj_set_size(ta, w, 56);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, x, y + 32);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, maxlen);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(ta);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_COLOR_TEXT_MUTED),
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(ta, ta_focused_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focused_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return ta;
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

    /* Top-aligned for the same reason js8_text_modal is: the on-screen keyboard
     * owns the bottom 280 px, and a centred panel ends up behind it. */
    s_panel = lv_obj_create(s_modal);
    lv_obj_set_size(s_panel, 900, 340);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 20, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Log a contact");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* ⭐ WHAT WILL BE WRITTEN, shown rather than asked for. The operator can
     * see the band and mode the record will carry before pressing Log, which
     * is the only check that catches a dial left on the wrong band. */
    s_hdr_lbl = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_hdr_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_hdr_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(s_hdr_lbl, LV_ALIGN_TOP_RIGHT, 0, 6);
    lv_label_set_text(s_hdr_lbl, "");

    s_ta_call = field_create("Callsign",  0,   50, 420, 16);
    s_ta_sent = field_create("RST sent",  450, 50, 190, 8);
    s_ta_rcvd = field_create("RST rcvd",  670, 50, 190, 8);
    lv_textarea_set_placeholder_text(s_ta_call, "K1ABC");

    s_note_lbl = lv_label_create(s_panel);
    lv_label_set_long_mode(s_note_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_note_lbl, 520);
    lv_obj_align(s_note_lbl, LV_ALIGN_TOP_LEFT, 0, 152);
    lv_obj_set_style_text_font(s_note_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_note_lbl, "");

    struct { const char *lbl; uint32_t col; lv_event_cb_t cb; int dx; } btns[2] = {
        { "Log",   UI_COLOR_SUCCESS, log_cb,   0    },
        { "Close", UI_COLOR_DANGER,  close_cb, -196 },
    };
    lv_obj_t *btn_close = NULL;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(s_panel);
        lv_obj_set_size(b, 180, 64);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, btns[i].dx, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(btns[i].col), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        /* An explicit bg_color survives LV_STATE_DISABLED - without this a
         * greyed-out Log would still be green. */
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_BT_OFF), LV_STATE_DISABLED);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, btns[i].lbl);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_MUTED), LV_STATE_DISABLED);
        lv_obj_center(l);
        if (i == 0) s_btn_log = b;
        else        btn_close = b;
    }
    lv_obj_add_state(s_btn_log, LV_STATE_DISABLED);
    ui_kbd_set_buttons(s_btn_log, btn_close);

    /* The project's keyboard, not LVGL's default - the three calls are copied
     * from js8_text_modal.c rather than reinvented. */
    s_keyboard = lv_keyboard_create(s_modal);
    static lv_style_t style_kb_btn;
    static bool kb_btn_style_inited = false;
    if (!kb_btn_style_inited) {
        lv_style_init(&style_kb_btn);
        lv_style_set_bg_color(&style_kb_btn, lv_color_hex(UI_COLOR_KEY_BG));
        lv_style_set_bg_opa(&style_kb_btn, LV_OPA_COVER);
        lv_style_set_text_color(&style_kb_btn, lv_color_hex(UI_COLOR_TEXT));
        lv_style_set_border_width(&style_kb_btn, 1);
        lv_style_set_border_color(&style_kb_btn, lv_color_hex(UI_COLOR_BORDER));
        kb_btn_style_inited = true;
    }
    lv_obj_add_style(s_keyboard, &style_kb_btn, LV_PART_ITEMS);
    ui_theme_style_keyboard(s_keyboard);
    lv_obj_set_size(s_keyboard, LV_PCT(100), 280);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* Caps, because a callsign is upper-case on the air and in the log. */
    ui_theme_keyboard_attach_caps_cycle_upper(s_keyboard);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
}

void manual_log_modal_init(void)
{
    modal_build();
}

void manual_log_modal_show(void)
{
    modal_build();
    if (s_open) return;

    lv_textarea_set_text(s_ta_call, "");
    /* Re-read on every open: the operator may have changed mode between
     * contacts, and a CW window offering 59 would be re-typed every time. */
    lv_textarea_set_text(s_ta_sent, default_rst());
    lv_textarea_set_text(s_ta_rcvd, default_rst());
    note_set("", UI_COLOR_TEXT_SECONDARY);
    header_refresh();
    refresh_log_enabled();

    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;

    /* ⛔ NO PRE-FOCUS AND NO KEYBOARD UNTIL A FIELD IS TAPPED - the tombstone
     * in ui_theme.h, and the same rule js8_text_modal.c follows. */
}
