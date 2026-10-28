// Scheduled events pane - see sched_modal.h.
//
// ⭐ ONE TIMELINE, NOT TWO LISTS. A contest and an announced summit activation
// compete for the same evening, so sorting them together by start time answers
// the operator's question directly. The filter buttons are there for when he
// has already decided which kind of evening it is.
//
// ⚠ NO KEYBOARD, so this panel may use the full height - the 280 px the
// on-screen keyboard owns in every text-entry modal here is free.
//
// ⛔ SPOTHOLE BEING DOWN IS AN ORDINARY STATE, not an error. Ian M0TRT asked
// for that explicitly when he granted use of the API, and it is the reason
// there is no red banner anywhere below: a failed refresh leaves the last good
// list on screen and the age line says how old it is.

#include "sched_modal.h"
#include "net/spots.h"
#include "wifi.h"
#include "ui_theme.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "sched_modal";

/* Sized like the Activation window for the same reason - the longest row has
 * to fit at the body font, and the screen is 1280 wide. No keyboard here, so
 * the panel runs nearly the full height. */
#define SCHED_PANEL_W   1100
#define SCHED_PANEL_H   650
#define SCHED_PAD       24
/* 8 rows FIT; the list holds twice that and scrolls. 53 of 60 upstream
 * records are in progress at any moment, so a non-scrolling pane showed one
 * screenful of NOW and nothing to plan around - the sort now puts upcoming
 * first, and this is what you scroll into. Not SCHED_MAX: every row is four
 * LVGL labels, and 40 rows would be 160 objects for a pane that is open for
 * a few seconds at a time. */
#define SCHED_ROWS      16
#define SCHED_ROW_H     48
#define SCHED_LIST_H    (8 * SCHED_ROW_H)

/* Column x, relative to the panel's content box. The IN column is right
 * aligned against the content width. */
#define COL_START       0
#define COL_WHAT        166
/* ⛔ SOURCE ENDS BEFORE IN STARTS. At 786 with a 200 px label it ran to 986
 * while IN begins at inner-170 = 882, so the two columns were drawn over
 * each other - "WA7BNM Contest..." and "1h 17m" in the same pixels on the
 * bench. 640 + 200 = 840 leaves a clear gap. Any change to one of these
 * three numbers has to be checked against the other two. */
#define COL_SRC         640
#define COL_SRC_W       200

enum { FILT_ALL = 0, FILT_CONTEST, FILT_ACTIVATION };

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_btn_filt[3];
static lv_obj_t *s_lbl_age = NULL;
static lv_obj_t *s_row_start[SCHED_ROWS];
static lv_obj_t *s_row_what[SCHED_ROWS];
static lv_obj_t *s_row_src[SCHED_ROWS];
static lv_obj_t *s_row_in[SCHED_ROWS];
static lv_obj_t *s_list = NULL;        /* the scrollable row area */
static lv_obj_t *s_lbl_empty = NULL;
static lv_timer_t *s_tick = NULL;
/* LVGL has no lv_timer_get_period(), so the period is tracked here. */
static uint32_t    s_tick_ms = 0;
static int       s_filter = FILT_ALL;
static uint32_t  s_seen_ver = 0;
static bool      s_open = false;

static void style_filt_btn(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? UI_COLOR_PRIMARY : 0x333a42), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(on ? 0xFFFFFF : 0x555555), 0);
    lv_obj_set_style_border_width(b, on ? 3 : 2, 0);
}

/* "2h 10m", "14m", "10h". Deliberately coarse above an hour: the operator is
 * deciding whether to stay up, not catching a train. */
static void fmt_in(int64_t secs, char *out, size_t n)
{
    if (secs < 60)      snprintf(out, n, "now");
    else if (secs < 3600) snprintf(out, n, "%dm", (int)(secs / 60));
    else {
        int h = (int)(secs / 3600), m = (int)((secs % 3600) / 60);
        if (h >= 10 || m == 0) snprintf(out, n, "%dh", h);
        else                   snprintf(out, n, "%dh %dm", h, m);
    }
}

/* ⛔ MOVED TO net/spots.c as sched_src_short(). /api/sched needs the same
 * shortening, and a second copy here would let the browser and the glass
 * disagree about what a source is called. */

static void row_clear(int i)
{
    lv_label_set_text(s_row_start[i], "");
    lv_label_set_text(s_row_what[i],  "");
    lv_label_set_text(s_row_src[i],   "");
    lv_label_set_text(s_row_in[i],    "");
}

static void refresh(void)
{
    for (int i = 0; i < 3; i++) style_filt_btn(s_btn_filt[i], s_filter == i);

    /* Age, or what is happening instead. Three states and they are different
     * facts: never fetched, fetching now, and this is N minutes old. */
    if (s_lbl_age) {
        char b[64];
        int age = sched_events_age_s();
        if (sched_events_busy())      snprintf(b, sizeof(b), "checking...");
        else if (age < 0)             snprintf(b, sizeof(b), "not fetched yet");
        else if (age < 90)            snprintf(b, sizeof(b), "updated just now");
        else                          snprintf(b, sizeof(b), "updated %d min ago", age / 60);
        lv_label_set_text(s_lbl_age, b);
    }

    sched_event_t ev[SCHED_MAX];
    int n = sched_events_get(ev, SCHED_MAX);
    int64_t now = (int64_t)time(NULL);

    int row = 0;
    for (int i = 0; i < n && row < SCHED_ROWS; i++) {
        if (s_filter == FILT_CONTEST    && !ev[i].is_contest) continue;
        if (s_filter == FILT_ACTIVATION &&  ev[i].is_contest) continue;

        bool running = ev[i].start_unix && ev[i].start_unix <= now;

        char when[12] = "--:--";
        if (ev[i].start_unix) {
            time_t t = (time_t)ev[i].start_unix;
            struct tm tmv;
            gmtime_r(&t, &tmv);
            snprintf(when, sizeof(when), "%02d:%02dZ", tmv.tm_hour, tmv.tm_min);
        }
        lv_label_set_text(s_row_start[row], running ? "NOW" : when);
        lv_obj_set_style_text_color(s_row_start[row],
                                    lv_color_hex(running ? 0x60D060 : UI_COLOR_TEXT_SECONDARY), 0);

        /* The reference belongs beside the callsign, not in its own column:
         * it is only present on half the rows, and an empty column reads as
         * missing data rather than as "contests have no reference". */
        char what[72];
        if (ev[i].ref[0]) snprintf(what, sizeof(what), "%s   %s", ev[i].what, ev[i].ref);
        else              snprintf(what, sizeof(what), "%s", ev[i].what);
        lv_label_set_text(s_row_what[row], what);

        /* The feed's names are sentences - "WA7BNM Contest Calendar",
         * "RSGB HF Contests" - and all three ellipsised to "...Contest...",
         * which told the reader nothing and was the widest thing in the
         * column. The distinguishing word is the first one. */
        lv_label_set_text(s_row_src[row], sched_src_short(ev[i].src));
        lv_obj_set_style_text_color(s_row_src[row],
                                    lv_color_hex(ev[i].is_contest ? 0x9AD0FF : 0xA0E0A0), 0);

        /* 24, not 16: gcc sizes each %d at 11 and refuses the smaller
         * buffer under -Werror=format-truncation. */
        char in[24];
        if (running) {
            if (ev[i].end_unix) {
                time_t t = (time_t)ev[i].end_unix;
                struct tm tmv;
                gmtime_r(&t, &tmv);
                snprintf(in, sizeof(in), "to %02d:%02d", tmv.tm_hour, tmv.tm_min);
            } else {
                snprintf(in, sizeof(in), "on air");
            }
        } else {
            fmt_in(ev[i].start_unix ? ev[i].start_unix - now : 0, in, sizeof(in));
        }
        lv_label_set_text(s_row_in[row], in);
        lv_obj_set_style_text_color(s_row_in[row],
            lv_color_hex(running ? 0x60D060
                                 : (ev[i].start_unix - now < 3 * 3600 ? 0xFFC864
                                                                      : UI_COLOR_TEXT_SECONDARY)), 0);
        row++;
    }
    for (int i = row; i < SCHED_ROWS; i++) row_clear(i);

    /* An empty list has three different causes and the operator can act on
     * only one of them, so say which it is rather than printing "nothing". */
    if (s_lbl_empty) {
        const char *msg = NULL;
        if (row == 0) {
            if (!wifi_is_connected())           msg = "No WiFi - the schedule needs a network.";
            else if (sched_events_busy())       msg = "Fetching the schedule...";
            else if (sched_events_version() == 0) msg = "No schedule yet - press Refresh.";
            else                                msg = "Nothing scheduled in this filter.";
        }
        lv_label_set_text(s_lbl_empty, msg ? msg : "");
    }
}

/* 10 s, and only while the pane is open: the "in 2h 10m" column is the only
 * thing that moves, and it moves slowly. */
static void tick_cb(lv_timer_t *t)
{
    if (!s_open) return;
    uint32_t v = sched_events_version();
    bool changed = (v != s_seen_ver);
    s_seen_ver = v;
    refresh();
    if (changed) ESP_LOGI(TAG, "schedule updated (version %" PRIu32 ")", v);
    /* The fast period exists only to catch the fetch that was requested
     * when the pane opened. Once something has landed, or a fetch is no
     * longer in flight, step back to 10 s - only the "in 2h" column
     * moves, and it moves slowly. */
    if (!sched_events_busy() && s_tick_ms < 10000) {
        s_tick_ms = 10000;
        lv_timer_set_period(t, s_tick_ms);
    }
}

static void filt_cb(lv_event_t *e)
{
    s_filter = (int)(intptr_t)lv_event_get_user_data(e);
    /* A new filter is a new list - leaving it scrolled where the last one
     * was shows an apparently empty pane. */
    if (s_list) lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    refresh();
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    if (!s_modal) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
    /* The timer costs nothing while hidden, but a pane nobody is looking at
     * should not be redrawing the LVGL tree either. */
    if (s_tick) lv_timer_pause(s_tick);
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    /* force: the operator pressed it, so the TTL is not the answer. */
    if (!sched_events_request(true)) ui_toast("No WiFi - cannot fetch the schedule");
    refresh();
}

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, int w,
                          const lv_font_t *font, uint32_t colour, bool right)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    if (right) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *text, int w, int h,
                        lv_align_t align, int x, int y, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, align, x, y);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_center(l);
    return b;
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
    lv_obj_set_size(s_panel, SCHED_PANEL_W, SCHED_PANEL_H);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, SCHED_PAD, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    const int inner = SCHED_PANEL_W - 2 * SCHED_PAD;

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Scheduled events");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    static const struct { const char *lbl; int w; int x; } filt[3] = {
        { "All",         150, 0   },
        { "Contests",    190, 158 },
        { "Activations", 230, 356 },
    };
    for (int i = 0; i < 3; i++)
        s_btn_filt[i] = mk_btn(s_panel, filt[i].lbl, filt[i].w, 56,
                               LV_ALIGN_TOP_LEFT, filt[i].x, 46,
                               filt_cb, (void *)(intptr_t)i);

    s_lbl_age = mk_label(s_panel, inner - 320, 62, 320,
                         &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED, true);

    /* Column headings, so "WA7BNM" in the third column reads as a source and
     * not as part of the event's name. */
    lv_label_set_text(mk_label(s_panel, COL_START, 118, 160, &lv_font_montserrat_20, 0x7A7A7A, false), "START");
    lv_label_set_text(mk_label(s_panel, COL_WHAT,  118, 400, &lv_font_montserrat_20, 0x7A7A7A, false), "EVENT");
    lv_label_set_text(mk_label(s_panel, COL_SRC,   118, COL_SRC_W, &lv_font_montserrat_20, 0x7A7A7A, false), "SOURCE");
    lv_label_set_text(mk_label(s_panel, inner - 160, 118, 160, &lv_font_montserrat_20, 0x7A7A7A, true), "IN");

    /* The rows live in their own scrollable box so the ones below the fold are
     * reachable with a finger. Transparent and borderless - it is a viewport,
     * not a panel inside a panel. */
    s_list = lv_obj_create(s_panel);
    lv_obj_set_size(s_list, inner, SCHED_LIST_H);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, 146);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < SCHED_ROWS; i++) {
        int y = i * SCHED_ROW_H;
        s_row_start[i] = mk_label(s_list, COL_START, y, 160,
                                  &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY, false);
        s_row_what[i]  = mk_label(s_list, COL_WHAT,  y, COL_SRC - COL_WHAT - 16,
                                  &lv_font_montserrat_24, UI_COLOR_TEXT, false);
        s_row_src[i]   = mk_label(s_list, COL_SRC,   y, COL_SRC_W,
                                  &lv_font_montserrat_20, 0x9AD0FF, false);
        s_row_in[i]    = mk_label(s_list, inner - 170, y, 170,
                                  &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY, true);
    }

    s_lbl_empty = mk_label(s_panel, COL_START, 200, inner,
                           &lv_font_montserrat_24, UI_COLOR_TEXT_SECONDARY, false);

    lv_obj_t *close = mk_btn(s_panel, "Close", 200, 72, LV_ALIGN_BOTTOM_LEFT, 40, 0,
                             close_cb, NULL);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x3a4149), 0);
    lv_obj_set_style_border_color(close, lv_color_hex(0x5a6169), 0);
    lv_obj_t *rf = mk_btn(s_panel, "Refresh", 240, 72, LV_ALIGN_BOTTOM_RIGHT, -40, 0,
                          refresh_cb, NULL);
    lv_obj_set_style_bg_color(rf, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_color(rf, lv_color_hex(0x5a9fd5), 0);

    s_tick = lv_timer_create(tick_cb, 10000, NULL);
    s_tick_ms = 10000;
    lv_timer_pause(s_tick);
}

void sched_modal_show(void)
{
    modal_build();
    if (s_open) return;

    /* Ask on open, but only if what we hold has gone stale - sched_events_
     * request() owns that rule, so opening the pane twice in a minute is one
     * fetch. The list draws immediately from whatever is already held. */
    sched_events_request(false);

    s_seen_ver = sched_events_version();
    if (s_list) lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    refresh();
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
    if (s_tick) {
        lv_timer_resume(s_tick);
        /* Straight away, not in 10 s: the fetch requested above usually lands
         * within a couple of seconds and the pane should fill, not sit empty. */
        s_tick_ms = 2000;
        lv_timer_set_period(s_tick, s_tick_ms);
    }
}
