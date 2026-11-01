// The web-radio player's control surface.
//
// Structurally js8_macro_modal.c: same scrim, same panel geometry, same
// montserrat_24 body. What is new here is that it has to show something that
// CHANGES while it is open - the ICY title arrives seconds after the stream
// starts and then again every track - so it carries a 1 Hz timer, and that
// timer only runs while the modal is on screen.
//
// ⛔ THE DIRECTORY FETCH IS NOT DONE HERE. radio_browser_fetch() opens a TLS
// connection and parses a few hundred kilobytes of JSON; on taskLVGL that
// would freeze the display for seconds and put a 3.6 KB settings struct on a
// stack that has already crashed this firmware once. It runs in a one-shot
// worker task and this file picks the result up from the timer.
//
// ⛔ CLOSING THE MODAL DOES NOT STOP THE RADIO. The player is a background
// task; the operator closes this to get back to the panadapter and expects
// the music to keep going. Stop is a button, not a side effect of leaving.
#include "webradio_modal.h"
#include "audio/webradio.h"
#include "net/radio_browser.h"
#include "rx_audio.h"
#include "ui_theme.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"    /* the station list is PSRAM, not .bss - see below */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wr_modal";

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_now   = NULL;   /* station + ICY title            */
static lv_obj_t *s_list  = NULL;   /* the scrollable station list    */
static lv_obj_t *s_btn_play = NULL;
static lv_obj_t *s_lbl_play = NULL;
static lv_obj_t *s_slider   = NULL;
static lv_obj_t *s_lbl_vol  = NULL;
static lv_timer_t *s_tick   = NULL;
static bool s_open = false;

/* The list, and the worker's handshake with the timer. `s_fetch_state` is the
 * only thing the two share: 0 idle, 1 running, 2 done, 3 failed. */
/* ⛔ PSRAM, NOT .bss. At RB_MAX_STATIONS 50 this array is 12.2 KB, and as a
 * static it was 12.2 KB of INTERNAL RAM on a board that runs ~30 KB free
 * (#65) - held for the whole session for a pane that is open for seconds.
 * Allocated on first open and kept (one allocation, never grows). */
static rb_station_t *s_st = NULL;
static volatile int s_n_st = 0;
static volatile int s_fetch_state = 0;

/* ⛔ 300 km, AND THE NUMBER WAS MEASURED BEFORE IT WAS CHANGED. The operator
 * asked for 50 stations in the list (2026-11-01). At the old 150 km the
 * directory holds only 54 rows in total for his grid - 46 MP3, 44 unique -
 * so 50 was NOT REACHABLE at any row limit, and raising RB_FETCH_ROWS alone
 * would have looked like a fix and delivered 44. 300 km returns 147 unique
 * MP3 stations, which covers Denmark, southern Sweden and northern Germany.
 * If the list ever has to come back to one country, this is the line. */
#define WR_RADIUS_KM 300

static void fetch_task(void *arg)
{
    (void)arg;
    int n = radio_browser_fetch(s_st, RB_MAX_STATIONS, WR_RADIUS_KM);
    if (n > 0) { s_n_st = n; s_fetch_state = 2; }
    else       { s_n_st = 0; s_fetch_state = 3; }
    ESP_LOGI(TAG, "fetch returned %d", n);
    vTaskDelete(NULL);
}

static void start_fetch(void)
{
    if (s_fetch_state == 1) return;
    /* The worker writes straight into this, so it has to exist before the
     * task starts - not inside it, where a failure would race the timer. */
    if (!s_st) {
        s_st = heap_caps_calloc(RB_MAX_STATIONS, sizeof(rb_station_t),
                                MALLOC_CAP_SPIRAM);
        if (!s_st) {
            s_n_st = 0;
            s_fetch_state = 3;
            ui_toast("No PSRAM for the station list");
            return;
        }
    }
    s_fetch_state = 1;
    lv_obj_clean(s_list);
    lv_obj_t *l = lv_label_create(s_list);
    lv_label_set_text(l, "Looking for stations near your grid...");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    /* 12 KB because radio_browser_fetch() loads settings and runs mbedTLS. */
    xTaskCreatePinnedToCoreWithCaps(fetch_task, "wr_fetch", 12288, NULL,
                                    tskIDLE_PRIORITY + 2, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void station_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_st || idx < 0 || idx >= s_n_st) return;
    ESP_LOGI(TAG, "play '%s'", s_st[idx].name);
    if (!webradio_play(s_st[idx].url, s_st[idx].name)) {
        char m[96];
        webradio_get_error(m, sizeof(m));
        ui_toast(m[0] ? m : "Could not start the player");
    }
}

static void fill_list(void)
{
    lv_obj_clean(s_list);
    if (s_n_st <= 0) {
        lv_obj_t *l = lv_label_create(s_list);
        char m[96];
        snprintf(m, sizeof(m), "%s", radio_browser_last_error());
        lv_label_set_text(l, m[0] ? m : "No stations found");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        return;
    }
    for (int i = 0; i < s_n_st; i++) {
        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, LV_PCT(100), 52);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 6, 0);
        lv_obj_add_event_cb(b, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l = lv_label_create(b);
        /* Distance and bitrate are on the row because both change whether a
         * station is worth tapping: a 32 kbps stream sounds like a telephone,
         * and 140 km away is a different town's local news. */
        char t[96];
        snprintf(t, sizeof(t), "%.47s   %u kbps  %u km",
                 s_st[i].name, (unsigned)s_st[i].bitrate_kbps,
                 (unsigned)s_st[i].distance_km);
        lv_label_set_text(l, t);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_center(l);
    }
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    webradio_state_t st = webradio_state();
    if (st == WEBRADIO_PLAYING || st == WEBRADIO_CONNECTING) webradio_pause();
    else if (webradio_is_paused())                           webradio_resume();
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    webradio_stop();
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    start_fetch();
}

static void vol_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(s);
    webradio_set_volume(v);
    if (s_lbl_vol) {
        char t[16];
        snprintf(t, sizeof(t), "%d", v);
        lv_label_set_text(s_lbl_vol, t);
    }
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    webradio_modal_hide();
}

/* 1 Hz: the ICY title and the player state are the only things that move. */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_open) return;

    if (s_fetch_state == 2 || s_fetch_state == 3) {
        int was = s_fetch_state;
        s_fetch_state = 0;
        fill_list();
        if (was == 3) ui_toast(radio_browser_last_error());
    }

    char name[48] = "", title[96] = "", err[80] = "";
    webradio_get_name(name, sizeof(name));
    webradio_get_title(title, sizeof(title));
    webradio_get_error(err, sizeof(err));

    char line[200];
    switch (webradio_state()) {
    case WEBRADIO_PLAYING:
        /* The station is the constant, the ICY title is what is on now. Two
         * lines, because a track name plus an artist routinely runs past the
         * panel width and shrinking the font is not the answer here. */
        snprintf(line, sizeof(line), "%.47s\n%.95s", name,
                 title[0] ? title : "(no track info)");
        break;
    case WEBRADIO_CONNECTING:
        snprintf(line, sizeof(line), "%.47s\nconnecting...", name);
        break;
    case WEBRADIO_ERROR:
        snprintf(line, sizeof(line), "%.47s\n%.79s", name, err);
        break;
    default:
        snprintf(line, sizeof(line), "%s",
                 webradio_is_paused() ? "Paused" : "Pick a station below");
        break;
    }
    lv_label_set_text(s_now, line);

    if (s_lbl_play) {
        webradio_state_t st = webradio_state();
        lv_label_set_text(s_lbl_play,
                          (st == WEBRADIO_PLAYING || st == WEBRADIO_CONNECTING)
                              ? "Pause" : "Play");
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
    lv_obj_set_size(s_panel, 940, 460);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 20, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Web radio");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_now = lv_label_create(s_panel);
    lv_label_set_long_mode(s_now, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_now, 880);
    lv_obj_align(s_now, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_text_font(s_now, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_now, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_label_set_text(s_now, "Pick a station below");

    /* The list on the left, the transport on the right. */
    s_list = lv_obj_create(s_panel);
    lv_obj_set_size(s_list, 600, 240);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, 112);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_list, 1, 0);
    lv_obj_set_style_radius(s_list, 6, 0);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);

    struct { const char *txt; uint32_t col; lv_event_cb_t cb; int y; lv_obj_t **lbl_out; lv_obj_t **btn_out; }
    B[3] = {
        { "Play",    UI_COLOR_SUCCESS, play_cb,    112, &s_lbl_play, &s_btn_play },
        { "Stop",    UI_COLOR_DANGER,  stop_cb,    184, NULL,        NULL        },
        { "Refresh", UI_COLOR_PRIMARY, refresh_cb, 256, NULL,        NULL        },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(s_panel);
        lv_obj_set_size(b, 260, 64);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 628, B[i].y);
        lv_obj_set_style_bg_color(b, lv_color_hex(B[i].col), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, B[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, B[i].txt);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_center(l);
        if (B[i].lbl_out) *B[i].lbl_out = l;
        if (B[i].btn_out) *B[i].btn_out = b;
    }

    lv_obj_t *vl = lv_label_create(s_panel);
    lv_label_set_text(vl, "Volume");
    lv_obj_set_style_text_font(vl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(vl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(vl, LV_ALIGN_TOP_LEFT, 0, 368);

    s_slider = lv_slider_create(s_panel);
    lv_obj_set_size(s_slider, 600, 24);
    lv_obj_align(s_slider, LV_ALIGN_TOP_LEFT, 120, 372);
    lv_slider_set_range(s_slider, 0, 100);
    lv_obj_add_event_cb(s_slider, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_vol = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_lbl_vol, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(s_lbl_vol, LV_ALIGN_TOP_LEFT, 736, 368);
    lv_label_set_text(s_lbl_vol, "0");

    lv_obj_t *cb = lv_button_create(s_panel);
    lv_obj_set_size(cb, 140, 56);
    lv_obj_align(cb, LV_ALIGN_TOP_LEFT, 772, 356);
    lv_obj_set_style_bg_color(cb, lv_color_hex(UI_COLOR_BT_OFF), 0);
    lv_obj_set_style_radius(cb, 8, 0);
    lv_obj_set_style_border_width(cb, 0, 0);
    lv_obj_add_event_cb(cb, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(cl);

    /* Esc closes; no Enter button, because there is no default station. */
    ui_kbd_set_buttons(NULL, cb);
}

void webradio_modal_init(void) { modal_build(); }

void webradio_modal_hide(void)
{
    if (!s_modal || !s_open) return;
    /* ⛔ THE TIMER GOES WITH THE MODAL. Left running it would repaint labels
     * on a hidden object once a second for the rest of the session - small,
     * but this board's 1 Hz timers are already a measured cost on core 0. */
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

void webradio_modal_show(void)
{
    modal_build();
    if (s_open) return;

    int v = webradio_get_volume();
    lv_slider_set_value(s_slider, v, LV_ANIM_OFF);
    char t[16];
    snprintf(t, sizeof(t), "%d", v);
    lv_label_set_text(s_lbl_vol, t);

    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
    if (!s_tick) s_tick = lv_timer_create(tick_cb, 1000, NULL);
    tick_cb(NULL);

    /* Fetch once per session rather than per open: the list is a directory of
     * transmitters, not live data, and a TLS fetch every time the operator
     * glances at the page would be rude to a free service. Refresh is a
     * button for when it is actually wanted. */
    if (s_n_st == 0 && s_fetch_state == 0) start_fetch();
    else                                   fill_list();
}
