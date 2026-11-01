    /* ⛔ NO HINT LINE. It read "tap to play, hold to store" at 14 px, which
     * he could not read and did not need - long press IS the standard way to
     * save a memory on a radio. It cost a row of height the buttons wanted. */
// The web-radio player's control surface.
//
// Structurally js8_macro_modal.c: same scrim, same panel geometry. What is
// new here is that it has to show something that CHANGES while it is open -
// the ICY title arrives seconds after the stream starts and then again every
// track - so it carries a 1 Hz timer, and that timer only runs while the
// modal is on screen.
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
//
// ⭐ THE TEXT FONTS ARE font_ui_latin_*, NOT THE BUILT-IN MONTSERRAT. Station
// names and ICY programme text are Danish, Swedish and German, and LVGL's
// built-ins stop at ASCII - the operator photographed a panel full of squares
// where æøå should have been. See font_ui_latin_24.c. The header and the
// transport buttons stay on a built-in, because their text is ASCII and the
// built-ins are already linked.
#include "webradio_modal.h"
#include "audio/webradio.h"
#include "net/radio_browser.h"
#include "rx_audio.h"
#include "ui_theme.h"
#include "ui.h"
#include "font_ui_latin.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"    /* the station list is PSRAM, not .bss - see below */
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "util/psram_task.h"   /* reapable: wr_fetch runs once per Refresh */
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "wr_modal";

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_station = NULL; /* the station name, big and gold       */
static lv_obj_t *s_now   = NULL;   /* the ICY programme text, wrapped      */
static lv_obj_t *s_list  = NULL;   /* the scrollable station list          */
static lv_obj_t *s_btn_play = NULL;
static lv_obj_t *s_lbl_play = NULL;
static lv_obj_t *s_lbl_vol  = NULL;
static lv_obj_t *s_search_ta = NULL;
static lv_obj_t *s_search_kb = NULL;
static lv_timer_t *s_tick   = NULL;
static bool s_open = false;

/* ⛔ PSRAM, NOT .bss. At RB_MAX_STATIONS 150 this array is 39 KB, and as a
 * static it would be 39 KB of INTERNAL RAM on a board that runs ~30 KB free
 * (#65) - held for the whole session for a pane that is open for seconds.
 * Allocated on first open and kept (one allocation, never grows). */
static rb_station_t *s_st = NULL;
static volatile int s_n_st = 0;
static volatile int s_fetch_state = 0;

/* ⛔ 450 km, AND THE NUMBER WAS MEASURED BEFORE IT WAS CHANGED, twice.
 *
 * MEASURED from his grid against the same query the device sends:
 *     150 km ->  44 unique MP3 stations
 *     300 km -> 147
 *     450 km -> 189   (250 rows, 304 KB of JSON)
 *
 * He asked for 50, then for 150. Neither was reachable by raising the row
 * limit - the RADIUS binds, and at 150 km the directory holds only 54 rows in
 * total no matter what is asked for. Raising RB_FETCH_ROWS alone would have
 * looked like a fix and quietly delivered 44.
 *
 * 450 km reaches southern Norway, southern Sweden and northern Germany. The
 * search field is what makes that usable rather than noisy. If the list ever
 * has to come back to one country, this is the line. */
#define WR_RADIUS_KM 450

/* ---- panel geometry -----------------------------------------------------
 *
 * 1180 x 660 at y=30 on a 1280x720 panel, so it DOES cover the status bar
 * while open. That is deliberate and it is what the extra height buys: the
 * operator asked for the whole ICY programme text to be visible, and a DR
 * blurb runs to three lines at a readable size. Widening the panel rather
 * than shrinking the font is the standing rule on this display. */
#define WR_PANEL_W   1180
#define WR_PANEL_H    660
#define WR_PAD         20
#define WR_INNER_W   (WR_PANEL_W - 2 * WR_PAD)   /* 1140 */
#define WR_COL_L      740                        /* list / search column    */
#define WR_COL_R_X    790
#define WR_COL_R_W    350
#define WR_MEM_N        6
#define WR_MEM_W      180
#define WR_MEM_GAP     12                        /* 6*180 + 5*12 = 1140     */
/* Reserved on the right of a list row for "192 kbps  386 km" at 24 px, plus
 * the row's own padding. The name gets the rest and dots at that edge. */
#define WR_META_W     260

/* ---- station memories ---------------------------------------------------
 *
 * Six slots. Tap plays, LONG PRESS stores whatever is playing right now.
 *
 * ⛔ IN NVS, NOT IN RAM. A memory that dies at the next reboot is not a
 * memory, and this board reboots for every flash. Its own namespace rather
 * than the settings struct: that struct is already 3.6 KB with a dirty
 * bitmap per field and has overflowed a task stack once (69de714c), and six
 * station URLs have no business in it. */
#define WR_MEM_NS   "wrmem"
typedef struct {
    char name[RB_NAME_LEN];
    char url[RB_URL_LEN];
} wr_mem_t;
static wr_mem_t  s_mem[WR_MEM_N];
static lv_obj_t *s_mem_btn[WR_MEM_N];
static lv_obj_t *s_mem_lbl[WR_MEM_N];

static void mem_load(void)
{
    nvs_handle_t h;
    if (nvs_open(WR_MEM_NS, NVS_READONLY, &h) != ESP_OK) return;
    for (int i = 0; i < WR_MEM_N; i++) {
        char key[8];
        snprintf(key, sizeof(key), "m%d", i);
        size_t len = sizeof(s_mem[i]);
        if (nvs_get_blob(h, key, &s_mem[i], &len) != ESP_OK || len != sizeof(s_mem[i]))
            memset(&s_mem[i], 0, sizeof(s_mem[i]));
        /* A blob from an older build could be unterminated; never trust it. */
        s_mem[i].name[sizeof(s_mem[i].name) - 1] = '\0';
        s_mem[i].url[sizeof(s_mem[i].url) - 1]   = '\0';
    }
    nvs_close(h);
}

static void mem_save(int i)
{
    nvs_handle_t h;
    if (nvs_open(WR_MEM_NS, NVS_READWRITE, &h) != ESP_OK) {
        ui_toast("Could not open the memory store");
        return;
    }
    char key[8];
    snprintf(key, sizeof(key), "m%d", i);
    esp_err_t e = nvs_set_blob(h, key, &s_mem[i], sizeof(s_mem[i]));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ui_toast("Could not save the memory");
}

static void mem_paint(int i)
{
    if (!s_mem_lbl[i]) return;
    bool full = s_mem[i].url[0] != '\0';
    lv_label_set_text(s_mem_lbl[i], full ? s_mem[i].name : "empty");
    lv_obj_set_style_text_color(s_mem_lbl[i],
        lv_color_hex(full ? 0xffffff : UI_COLOR_TEXT_SECONDARY), 0);
}

static void mem_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= WR_MEM_N) return;
    if (!s_mem[i].url[0]) {
        ui_toast("Empty - hold this button to store what is playing");
        return;
    }
    ESP_LOGI(TAG, "memory %d -> '%s'", i + 1, s_mem[i].name);
    if (!webradio_play(s_mem[i].url, s_mem[i].name)) {
        char m[96];
        webradio_get_error(m, sizeof(m));
        ui_toast(m[0] ? m : "Could not start the player");
    }
}

static void mem_long_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= WR_MEM_N) return;

    /* ⛔ THE PLAYER IS THE SOURCE, NOT THE LIST SELECTION. "the station you
     * hear" is what he asked for, and after a memory recall or a search that
     * is not the row anybody last tapped. webradio_get_name() is the only
     * thing that always describes what is actually coming out of the
     * speaker. */
    char name[RB_NAME_LEN] = "";
    webradio_get_name(name, sizeof(name));
    const char *url = webradio_current_url();
    if (!url || !url[0]) {
        ui_toast("Nothing is playing to store");
        return;
    }
    snprintf(s_mem[i].name, sizeof(s_mem[i].name), "%s", name[0] ? name : "Station");
    snprintf(s_mem[i].url,  sizeof(s_mem[i].url),  "%s", url);
    mem_save(i);
    mem_paint(i);

    char t[96];
    snprintf(t, sizeof(t), "Stored in M%d: %.48s", i + 1, s_mem[i].name);
    ui_toast(t);
}

/* ---- search -------------------------------------------------------------
 *
 * Filters as the operator types, over the list already fetched - no network,
 * no re-sort. With 150 stations from four countries in the pane, this is what
 * makes the wider radius usable rather than noisy. */
static char s_query[32] = "";

/* ---- genre -------------------------------------------------------------
 *
 * ⭐ THE TAGS WERE ALREADY IN THE REPLY. Every directory row carries a
 * comma-separated tag list that this file used to discard; rb_station_t now
 * keeps it, so the picker is a local substring test over a list already
 * fetched - no second query, no wait, and it composes with the text search.
 *
 * ⚠ THE LIST IS NOT INVENTED. These are the tags that actually occur within
 * 450 km of his grid, counted from the real reply on 2026-11-01: pop 44,
 * news 28, culture 24, rock 23, information 21, oldies 12, techno 12, talk
 * 11, chillout 10, house 10, dance 9, electronic 9, 80s 8, 90s 8. A genre
 * nobody broadcasts is a line that only ever shows an empty list.
 *
 * Several needles per entry because the directory is folksonomy, not a
 * taxonomy: "news", "information" and "public service" are one thing to an
 * operator looking for the news. */
typedef struct { const char *label; const char *needles[6]; } wr_genre_t;
static const wr_genre_t WR_GENRES[] = {
    { "All genres",    { NULL } },
    { "News & talk",   { "news", "talk", "information", "public service", "spoken", NULL } },
    { "Pop",           { "pop", "contemporary", "chart", NULL } },
    { "Rock",          { "rock", "metal", "alternative", NULL } },
    { "Dance & club",  { "dance", "techno", "house", "electronic", "trance", "edm" } },
    { "Jazz & blues",  { "jazz", "blues", "soul", NULL } },
    { "Classical",     { "classical", "klassik", "classic music", "opera", NULL } },
    { "Oldies",        { "oldies", "60s", "70s", "80s", "90s", NULL } },
    { "Chill & easy",  { "chillout", "lounge", "easy", "ambient", "relax", NULL } },
    { "Culture",       { "culture", "kultur", "documentary", NULL } },
};
#define WR_GENRE_N ((int)(sizeof(WR_GENRES) / sizeof(WR_GENRES[0])))
static int s_genre = 0;            /* index into WR_GENRES; 0 = all */

/* ---- stations that did not play ----------------------------------------
 *
 * ⛔ THE DIRECTORY CANNOT TELL US THIS, which is why the list is kept here.
 * MEASURED on the 450 km reply 2026-11-01: all 198 MP3 rows have
 * lastcheckok=1, every one was checked OK within 5 days (median: today), and
 * ssl_error is 0 for all of them. `hidebroken=true` is already set. There is
 * no field left to filter on - by the directory's own account every station
 * in the pane works.
 *
 * So the only honest signal is OUR OWN attempt. A station that fails to start
 * drops out of the list for the rest of the session; Refresh brings them all
 * back, because most of these failures are transient and a permanent hide
 * built on one bad evening would be worse than the problem.
 *
 * ⚠ The large majority of what he saw was not the stations at all - it was
 * the Location header never being read, so every 3xx died. That is fixed. */
#define WR_BAD_MAX 64
static uint32_t s_bad[WR_BAD_MAX];
static int      s_bad_n = 0;

static uint32_t url_hash(const char *s)
{
    uint32_t h = 2166136261u;                       /* FNV-1a */
    for (; s && *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static bool url_is_bad(const char *u)
{
    uint32_t h = url_hash(u);
    for (int i = 0; i < s_bad_n; i++) if (s_bad[i] == h) return true;
    return false;
}

/* Returns true if this is the first time we have seen it fail. */
static bool mark_bad(const char *u)
{
    if (!u || !u[0] || url_is_bad(u)) return false;
    if (s_bad_n >= WR_BAD_MAX) return false;
    s_bad[s_bad_n++] = url_hash(u);
    return true;
}

static bool genre_matches(const rb_station_t *st)
{
    if (s_genre <= 0 || s_genre >= WR_GENRE_N) return true;
    const char *const *n = WR_GENRES[s_genre].needles;
    for (int i = 0; i < 6 && n[i]; i++)
        if (strstr(st->genre, n[i])) return true;
    return false;
}

/* A key both sides of the comparison are folded into: lower case, and every
 * Latin-1 accented letter reduced to its base ASCII letter. So "koben" finds
 * "København" and "munch" finds "München" - typing the accents on a search
 * field nobody has an æ key for is not a reasonable thing to ask. */
static void search_fold(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; ) {
        uint32_t cp = *p;
        if (cp < 0x80) { p++; }
        else if ((cp & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
        } else { p++; cp = '?'; }

        char c;
        if      (cp >= 0xC0 && cp <= 0xC5) c = 'a';
        else if (cp >= 0xE0 && cp <= 0xE5) c = 'a';
        else if (cp == 0xC6 || cp == 0xE6) c = 'a';          /* Æ æ */
        else if (cp == 0xC7 || cp == 0xE7) c = 'c';
        else if ((cp >= 0xC8 && cp <= 0xCB) || (cp >= 0xE8 && cp <= 0xEB)) c = 'e';
        else if ((cp >= 0xCC && cp <= 0xCF) || (cp >= 0xEC && cp <= 0xEF)) c = 'i';
        else if (cp == 0xD1 || cp == 0xF1) c = 'n';
        else if ((cp >= 0xD2 && cp <= 0xD6) || (cp >= 0xF2 && cp <= 0xF6)) c = 'o';
        else if (cp == 0xD8 || cp == 0xF8) c = 'o';          /* Ø ø */
        else if ((cp >= 0xD9 && cp <= 0xDC) || (cp >= 0xF9 && cp <= 0xFC)) c = 'u';
        else if (cp == 0xDD || cp == 0xFD || cp == 0xFF) c = 'y';
        else if (cp == 0xDF) c = 's';                        /* ß */
        else if (cp < 0x80) c = (char)tolower((int)cp);
        else continue;                                       /* drop the rest */
        out[o++] = c;
    }
    out[o] = '\0';
}

static bool station_matches(const rb_station_t *st, const char *qfold)
{
    if (!qfold[0]) return true;
    char nf[RB_NAME_LEN * 2];
    search_fold(st->name, nf, sizeof(nf));
    return strstr(nf, qfold) != NULL;
}

static void fetch_task(void *arg)
{
    (void)arg;
    int n = radio_browser_fetch(s_st, RB_MAX_STATIONS, WR_RADIUS_KM);
    if (n > 0) { s_n_st = n; s_fetch_state = 2; }
    else       { s_n_st = 0; s_fetch_state = 3; }
    ESP_LOGI(TAG, "fetch returned %d", n);
    /* ⛔ PARK, NOT DELETE. This task is created again on every Refresh, and
     * vTaskDelete() on a static stack+TCB frees neither - the same leak that
     * took the internal heap to 739 B on 2026-11-01. See psram_task.h. */
    psram_task_park();
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
    /* Refresh is the "try them all again" button, so it clears the hides. */
    s_bad_n = 0;
    s_fetch_state = 1;
    lv_obj_clean(s_list);
    lv_obj_t *l = lv_label_create(s_list);
    lv_label_set_text(l, "Looking for stations near your grid...");
    lv_obj_set_style_text_font(l, &font_ui_latin_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    /* 12 KB because radio_browser_fetch() loads settings and runs mbedTLS. */
    psram_task_reap();
    if (!psram_task_create_reapable(fetch_task, "wr_fetch", 12288, NULL,
                                    tskIDLE_PRIORITY + 2, 1)) {
        s_fetch_state = 3;
        ui_toast("Could not start the directory fetch");
    }
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
        lv_obj_set_style_text_font(l, &font_ui_latin_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        return;
    }

    char qf[sizeof(s_query) * 2];
    search_fold(s_query, qf, sizeof(qf));

    int shown = 0, hidden = 0;
    for (int i = 0; i < s_n_st; i++) {
        if (!genre_matches(&s_st[i])) continue;
        if (!station_matches(&s_st[i], qf)) continue;
        if (url_is_bad(s_st[i].url)) { hidden++; continue; }
        shown++;

        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, LV_PCT(100), 52);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 6, 0);
        lv_obj_add_event_cb(b, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* ⭐ TWO LABELS, NOT ONE STRING. The station is what the eye should
         * land on; the bitrate and distance are the small print that decides
         * whether it is worth tapping - a 32 kbps stream sounds like a
         * telephone and 140 km away is a different town's local news. One
         * label can only have one colour, so the row is split: white name on
         * the left, dimmed figures on the right.
         *
         * ⛔ ONE LINE, AND THE HEIGHT IS WHAT MAKES IT ONE. LV_LABEL_LONG_DOT
         * fills the available HEIGHT before it starts dotting, so a label
         * with a fixed width and an automatic height wraps first. Pinning the
         * height to a single line is the whole fix. */
        lv_obj_t *meta = lv_label_create(b);
        char mt[32];
        snprintf(mt, sizeof(mt), "%u kbps  %u km",
                 (unsigned)s_st[i].bitrate_kbps, (unsigned)s_st[i].distance_km);
        lv_label_set_text(meta, mt);
        lv_obj_set_style_text_font(meta, &font_ui_latin_24, 0);
        lv_obj_set_style_text_color(meta, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_align(meta, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, s_st[i].name);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        /* Leaves room for the figures; the name dots rather than runs under. */
        lv_obj_set_size(l, WR_COL_L - WR_META_W, 36);
        lv_obj_set_style_text_font(l, &font_ui_latin_28, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    }

    if (!shown) {
        lv_obj_t *l = lv_label_create(s_list);
        char m[128];
        /* Name BOTH filters: with a genre set, an empty list from a typo and
         * an empty list from "no jazz within 450 km" look identical. */
        if (s_query[0] && s_genre > 0)
            snprintf(m, sizeof(m), "No %s station matches \"%.20s\"",
                     WR_GENRES[s_genre].label, s_query);
        else if (s_genre > 0)
            snprintf(m, sizeof(m), "No %s stations in range", WR_GENRES[s_genre].label);
        else
            snprintf(m, sizeof(m), "Nothing matches \"%.24s\"", s_query);
        lv_label_set_text(l, m);
        lv_obj_set_style_text_font(l, &font_ui_latin_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    }

    /* ⛔ SAY SO RATHER THAN JUST HIDING. A station that silently vanishes
     * from a list the operator was reading is a worse fault than one that
     * would not play, and this line is also where he learns Refresh brings
     * them back. Dim and not a button, so it cannot be tapped. */
    if (hidden) {
        lv_obj_t *l = lv_label_create(s_list);
        char m[96];
        snprintf(m, sizeof(m), "%d hidden - would not play. Refresh to try again.",
                 hidden);
        lv_label_set_text(l, m);
        lv_obj_set_style_text_font(l, &font_ui_latin_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    }
}

static void search_cb(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_search_ta);
    snprintf(s_query, sizeof(s_query), "%s", txt ? txt : "");
    fill_list();
}

static void genre_cb(lv_event_t *e)
{
    s_genre = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    fill_list();
}

static void search_focus_cb(lv_event_t *e)
{
    (void)e;
    if (!s_search_kb) return;
    lv_keyboard_set_textarea(s_search_kb, s_search_ta);
    ui_osk_show(s_search_kb);
}

static void search_done_cb(lv_event_t *e)
{
    (void)e;
    if (s_search_kb) lv_obj_add_flag(s_search_kb, LV_OBJ_FLAG_HIDDEN);
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

static void vol_step_cb(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    int v = webradio_get_volume() + step;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
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

    char name[RB_NAME_LEN] = "", title[224] = "", err[80] = "";
    webradio_get_name(name, sizeof(name));
    webradio_get_title(title, sizeof(title));
    webradio_get_error(err, sizeof(err));

    switch (webradio_state()) {
    case WEBRADIO_PLAYING:
        lv_label_set_text(s_station, name);
        lv_label_set_text(s_now, title[0] ? title : "(no programme info)");
        break;
    case WEBRADIO_CONNECTING:
        lv_label_set_text(s_station, name);
        lv_label_set_text(s_now, "connecting...");
        break;
    case WEBRADIO_ERROR:
        lv_label_set_text(s_station, name);
        lv_label_set_text(s_now, err);
        /* First time this URL has failed: take it out of the list. The
         * player is the source of the URL, not the row that was tapped -
         * after a memory recall those are different stations. */
        if (mark_bad(webradio_current_url())) {
            ESP_LOGW(TAG, "hiding '%s' - %s", name, err);
            fill_list();
        }
        break;
    default:
        lv_label_set_text(s_station, webradio_is_paused() ? "Paused" : "");
        lv_label_set_text(s_now, webradio_is_paused() ? ""
                                                      : "Pick a station below");
        break;
    }

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

    mem_load();

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
    lv_obj_set_size(s_panel, WR_PANEL_W, WR_PANEL_H);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, WR_PAD, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Header. ASCII, so a built-in font, and the biggest one that is already
     * linked - 32. */
    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Web radio");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* ⭐ AN X IN THE CORNER, not a Close button in the row of controls. It
     * was sitting next to Stop, which is a destructive-looking place for the
     * one button that does nothing to the radio. */
    lv_obj_t *xb = lv_button_create(s_panel);
    lv_obj_set_size(xb, 60, 60);
    lv_obj_align(xb, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_radius(xb, 8, 0);
    lv_obj_set_style_border_width(xb, 0, 0);
    lv_obj_add_event_cb(xb, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_center(xl);

    /* Now playing: the station is the constant, the programme text is what is
     * on right now. Two labels rather than one with a newline, because they
     * want different sizes and the title has to WRAP - the operator asked for
     * the whole text, and a DR blurb is three lines wide. */
    s_station = lv_label_create(s_panel);
    lv_label_set_long_mode(s_station, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_station, WR_INNER_W - 70);
    lv_obj_align(s_station, LV_ALIGN_TOP_LEFT, 0, 66);
    lv_obj_set_style_text_font(s_station, &font_ui_latin_28, 0);
    lv_obj_set_style_text_color(s_station, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_label_set_text(s_station, "");

    s_now = lv_label_create(s_panel);
    lv_label_set_long_mode(s_now, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_now, WR_INNER_W);
    /* ⛔ TWO LINES, NOT THREE. Three was his first ask and his second was to
     * take one back: a DR blurb is longer than any panel, so the third line
     * bought a word and cost 30 px that the list and the memory row needed
     * more. Dotted at the end rather than wrapped forever. */
    lv_obj_set_height(s_now, 62);
    lv_obj_align(s_now, LV_ALIGN_TOP_LEFT, 0, 104);
    lv_obj_set_style_text_font(s_now, &font_ui_latin_24, 0);
    lv_obj_set_style_text_color(s_now, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_label_set_text(s_now, "Pick a station below");

    /* Search. The same pattern adif_view_modal.c uses, so the snap-on and
     * Bluetooth keyboards drive it for free through ui.c's keyboard bridge
     * and ui_osk_show() decides whether the on-screen one is needed. */
    s_search_ta = lv_textarea_create(s_panel);
    lv_textarea_set_one_line(s_search_ta, true);
    lv_textarea_set_placeholder_text(s_search_ta, "Type to filter stations");
    lv_textarea_set_max_length(s_search_ta, sizeof(s_query) - 1);
    lv_obj_set_size(s_search_ta, WR_COL_L / 2 - 6, 56);
    lv_obj_align(s_search_ta, LV_ALIGN_TOP_LEFT, 0, 176);
    lv_obj_set_style_text_font(s_search_ta, &font_ui_latin_28, 0);
    ui_theme_style_textarea(s_search_ta);
    lv_obj_add_event_cb(s_search_ta, search_cb,       LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_search_ta, search_focus_cb, LV_EVENT_FOCUSED,       NULL);
    lv_obj_add_event_cb(s_search_ta, search_done_cb,  LV_EVENT_DEFOCUSED,     NULL);

    {
        /* lv_dropdown wants one string with newline separators, and building
         * it from WR_GENRES means the list and the matcher can never drift. */
        char opts[WR_GENRE_N * 20];
        int o = 0;
        for (int i = 0; i < WR_GENRE_N && o < (int)sizeof(opts) - 1; i++)
            o += snprintf(opts + o, sizeof(opts) - (size_t)o, "%s%s",
                          i ? "\n" : "", WR_GENRES[i].label);
        lv_obj_t *dd = lv_dropdown_create(s_panel);
        lv_dropdown_set_options(dd, opts);
        lv_obj_set_size(dd, WR_COL_L / 2 - 6, 56);
        lv_obj_align(dd, LV_ALIGN_TOP_LEFT, WR_COL_L / 2 + 6, 176);
        lv_obj_set_style_text_font(dd, &lv_font_montserrat_28, 0);
        lv_obj_set_style_bg_color(dd, lv_color_hex(UI_COLOR_KEY_BG), 0);
        lv_obj_set_style_border_color(dd, lv_color_hex(UI_COLOR_BORDER), 0);
        lv_obj_set_style_text_color(dd, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_add_event_cb(dd, genre_cb, LV_EVENT_VALUE_CHANGED, NULL);
        /* The open list is taller than the panel's remaining room, so it has
         * to be allowed to draw over everything below it. */
        lv_obj_t *dl = lv_dropdown_get_list(dd);
        if (dl) {
            lv_obj_set_style_text_font(dl, &lv_font_montserrat_28, 0);
            lv_obj_set_style_bg_color(dl, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
            lv_obj_set_style_text_color(dl, lv_color_hex(UI_COLOR_TEXT), 0);
        }
    }

    s_list = lv_obj_create(s_panel);
    lv_obj_set_size(s_list, WR_COL_L, 270);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, 244);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_list, 1, 0);
    lv_obj_set_style_radius(s_list, 6, 0);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    ui_kbd_add_scrollable(s_list);

    struct { const char *txt; uint32_t col; lv_event_cb_t cb; int y; lv_obj_t **lbl_out; lv_obj_t **btn_out; }
    B[3] = {
        { "Play",    UI_COLOR_SUCCESS, play_cb,    176, &s_lbl_play, &s_btn_play },
        { "Stop",    UI_COLOR_DANGER,  stop_cb,    252, NULL,        NULL        },
        { "Refresh", UI_COLOR_PRIMARY, refresh_cb, 328, NULL,        NULL        },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(s_panel);
        lv_obj_set_size(b, WR_COL_R_W, 68);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, WR_COL_R_X, B[i].y);
        lv_obj_set_style_bg_color(b, lv_color_hex(B[i].col), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, B[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, B[i].txt);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_center(l);
        if (B[i].lbl_out) *B[i].lbl_out = l;
        if (B[i].btn_out) *B[i].btn_out = b;
    }

    /* ⛔ BUTTONS, NOT A SLIDER, AND NOT ACROSS THE BOTTOM. The slider ran the
     * width of the list and was 20 px tall - easy to miss on a touch screen,
     * and the operator said so. A step pair under Refresh keeps every control
     * that changes the sound in one column, and each target is 100x68. */
    {
        /* ⭐ 72 px GLYPHS, FROM A TWO-CHARACTER FONT. "At least double as big"
         * (operator) and the largest built-in montserrat here is 48 - 1.5x,
         * not 2x. A full 72 px face costs a few hundred KB; + and - alone
         * cost 7.5 KB, and a stepper needs exactly two characters.
         *
         * 120 x 110 targets, so the pair plus the reading fill the 350 px
         * column exactly: 120 + 110 + 120. */
        struct { const char *txt; int dx; int step; } V[2] = {
            { "-", 0,   -5 },
            { "+", 230, +5 },
        };
        for (int i = 0; i < 2; i++) {
            lv_obj_t *b = lv_button_create(s_panel);
            lv_obj_set_size(b, 120, 110);
            lv_obj_align(b, LV_ALIGN_TOP_LEFT, WR_COL_R_X + V[i].dx, 404);
            lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(UI_COLOR_BORDER), 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_add_event_cb(b, vol_step_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)V[i].step);
            /* Held down, it repeats - 0 to 100 in fives is 20 taps otherwise. */
            lv_obj_add_event_cb(b, vol_step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                                (void *)(intptr_t)V[i].step);
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, V[i].txt);
            lv_obj_set_style_text_font(l, &font_ui_pm_72, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
            lv_obj_center(l);
        }
        /* The reading at the header's size, so the number you are changing is
         * as readable as the panel's own title. */
        s_lbl_vol = lv_label_create(s_panel);
        lv_obj_set_width(s_lbl_vol, 110);
        lv_obj_set_style_text_align(s_lbl_vol, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_lbl_vol, LV_ALIGN_TOP_LEFT, WR_COL_R_X + 120, 432);
        lv_obj_set_style_text_font(s_lbl_vol, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_label_set_text(s_lbl_vol, "0");
    }

    /* ⛔ NO HINT LINE ABOVE THE MEMORY ROW. It read "tap to play, hold to
     * store" at 14 px - unreadable, and unnecessary because long press IS the
     * standard way to save a memory on a radio. Removed on his second asking:
     * the first attempt deleted the comment and left the label, so it carried
     * on showing through between and above the buttons. */

    for (int i = 0; i < WR_MEM_N; i++) {
        lv_obj_t *b = lv_button_create(s_panel);
        /* The ADIF-log button's look, from ft8_screen_view.c: the darker blue
         * fill with the primary border at 2 px is this app's "a real control
         * lives here" treatment, and six of them read as a row of presets
         * rather than six empty boxes. He asked for the looks only. */
        lv_obj_set_size(b, WR_MEM_W, 86);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, i * (WR_MEM_W + WR_MEM_GAP), 526);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x163d5e), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_pad_all(b, 6, 0);
        /* ⛔ SHORT_CLICKED, NOT CLICKED - a real defect, not a style note.
         * LVGL sends CLICKED on release even when the press was long, so
         * holding a button to STORE a memory also fired the play path: it
         * stored the station and then immediately started it. The ADIF button
         * splits them the same way and says so. */
        lv_obj_add_event_cb(b, mem_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, mem_long_cb,  LV_EVENT_LONG_PRESSED,  (void *)(intptr_t)i);
        s_mem_btn[i] = b;

        /* ⛔ NO "M1" ROW. It was a slot number nobody needs next to the only
         * thing that identifies the button - the station - and it stole half
         * the height from the text that matters. The operator said so. */
        /* ⛔ DOT-TRUNCATED AND WIDTH-BOUND. "(((EBM-Radio)))" and "DR P4
         * København" both overflow 180 px at any readable size, and an
         * unbounded label in LVGL does not clip - it draws straight over its
         * neighbour. 20 px is the floor here: the rule on this display is
         * widen the panel rather than shrink the font, and six buttons across
         * 1140 px is already the widest this row can be. */
        /* TWO lines, then dots. "DR P4 København" and "SmoothJazz.com" both
         * need the second one, and a fixed two-line height is what gives it -
         * same mechanism as the list rows above, used the other way round. */
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_size(l, WR_MEM_W - 20, 64);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        lv_obj_set_style_text_font(l, &font_ui_latin_24, 0);
        s_mem_lbl[i] = l;
        mem_paint(i);
    }

    /* Built last so it draws over the panel, and hidden until the search
     * field is focused. */
    s_search_kb = lv_keyboard_create(s_modal);
    /* ⛔⛔ THE HOUSE KEYBOARD, NOT LVGL'S. ui_theme_style_keyboard() styles the
     * keyboard's BACKGROUND and nothing else, so on its own it leaves LVGL's
     * default white keys at LVGL's default label size - which is a different
     * keyboard from every other one in this app, and the operator has had to
     * say so more than once. adif_view_modal.c carries the same three parts
     * and the same warning; this is a copy of it, deliberately.
     *
     * The per-key style is what makes the keys dark, and the explicit
     * montserrat_28 on the keyboard is what makes the LETTERS match - that
     * second one is the difference the eye actually catches. */
    static lv_style_t style_kb_btn;
    static bool kb_btn_style_inited = false;
    if (!kb_btn_style_inited) {
        lv_style_init(&style_kb_btn);
        lv_style_set_bg_color(&style_kb_btn, lv_color_hex(0x303030));
        lv_style_set_text_color(&style_kb_btn, lv_color_white());
        lv_style_set_border_width(&style_kb_btn, 1);
        lv_style_set_border_color(&style_kb_btn, lv_color_hex(0x505050));
        kb_btn_style_inited = true;
    }
    lv_obj_add_style(s_search_kb, &style_kb_btn, LV_PART_ITEMS);
    ui_theme_style_keyboard(s_search_kb);
    lv_obj_set_size(s_search_kb, LV_PCT(100), 280);
    lv_obj_align(s_search_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* Lower case: these are station names, not callsigns. */
    lv_keyboard_set_mode(s_search_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    ui_theme_keyboard_attach_caps_cycle_upper(s_search_kb);
    lv_obj_set_style_text_font(s_search_kb, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_search_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_search_kb, search_done_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_search_kb, search_done_cb, LV_EVENT_CANCEL, NULL);

    /* Esc closes; no Enter button, because there is no default station. */
    ui_kbd_set_buttons(NULL, xb);
}

void webradio_modal_init(void) { modal_build(); }

void webradio_modal_hide(void)
{
    if (!s_modal || !s_open) return;
    /* ⛔ THE TIMER GOES WITH THE MODAL. Left running it would repaint labels
     * on a hidden object once a second for the rest of the session - small,
     * but this board's 1 Hz timers are already a measured cost on core 0. */
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_search_kb) lv_obj_add_flag(s_search_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

void webradio_modal_show(void)
{
    modal_build();
    if (s_open) return;

    int v = webradio_get_volume();
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
