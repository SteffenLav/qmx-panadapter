#include "ft8_screen_view.h"
#include "ft8_screen.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cat/cat.h"
#include "storage/settings.h"
#include "util/maidenhead.h"
#include "util/dxcc.h"

static const char *TAG = "ft8_view";

// Geometry. Display is 1280x720 landscape (sw_rotate). Top bar 60,
// bottom bar 36; middle band = 624 px.
#define TOP_BAR_H        60
#define BOTTOM_BAR_H     36
#define MID_Y            TOP_BAR_H
#define MID_H            (720 - TOP_BAR_H - BOTTOM_BAR_H)
#define MID_W            1280
#define LEFT_W           320
#define RIGHT_W          (MID_W - LEFT_W)

// Column x-offsets / widths within the row.
// Layout: CALL | MESSAGE | COUNTRY | SNR | KM | BRG | HRD
#define COL_CALL_X      12
#define COL_TEXT_X      150
#define COL_COUNTRY_X   430
#define COL_SNR_X       580
#define COL_KM_X        670
#define COL_BRG_X       770
#define COL_HEARD_X     880
#define COL_RIGHT_EDGE  960
#define ROW_H           36

#define COL_CALL_W      (COL_TEXT_X    - COL_CALL_X    - 8)
#define COL_MSG_W       (COL_COUNTRY_X - COL_TEXT_X    - 8)
#define COL_COUNTRY_W   (COL_SNR_X     - COL_COUNTRY_X - 8)
#define COL_SNR_W       (COL_KM_X      - COL_SNR_X     - 8)
#define COL_KM_W        (COL_BRG_X     - COL_KM_X      - 8)
#define COL_BRG_W       (COL_HEARD_X   - COL_BRG_X     - 8)
#define COL_HEARD_W     (COL_RIGHT_EDGE - COL_HEARD_X  - 16)

// Pool size: pre-allocated row container/label objects in BSS.
// Combined with shared lv_style_t (below), per-row local styles
// drop from ~42 to 1 (SNR colour), so 20 rows is comfortable.
#define MAX_ROWS        20

// Shared styles. These live in BSS, not on the heap, so the dozens
// of label objects can share them via lv_obj_add_style() without
// triggering per-object local-style allocations. This was the root
// of the burger-press crash at higher row counts.
static lv_style_t s_style_row;          // row container
static lv_style_t s_style_col_call;     // CALL column (amber, left)
static lv_style_t s_style_col_msg;      // MESSAGE column (white, left)
static lv_style_t s_style_col_country;  // COUNTRY (dim, left)
static lv_style_t s_style_col_snr;      // SNR base (font/pos/align), colour set per-row
static lv_style_t s_style_col_km;       // KM (dim, right)
static lv_style_t s_style_col_brg;      // BRG (dim, right)
static lv_style_t s_style_col_heard;    // HRD (dim, right)
static lv_style_t s_style_header;       // column header row
static lv_style_t s_style_header_label; // column header text
static bool s_styles_inited = false;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *l_call;
    lv_obj_t *l_msg;
    lv_obj_t *l_country;
    lv_obj_t *l_snr;
    lv_obj_t *l_km;
    lv_obj_t *l_brg;
    lv_obj_t *l_heard;
    // Dirty-tracking cache: skip lv_label_set_text when unchanged.
    char prev_call[16];
    char prev_msg[40];
    char prev_country[24];
    char prev_snr[12];
    char prev_km[12];
    char prev_brg[12];
    char prev_heard[12];
    int16_t prev_snr_db;
} row_widgets_t;

static lv_obj_t *s_container   = NULL;
static lv_obj_t *s_left_pane   = NULL;
static lv_obj_t *s_right_pane  = NULL;

static lv_obj_t *s_lbl_mode     = NULL;
static lv_obj_t *s_lbl_freq     = NULL;
static lv_obj_t *s_lbl_utc      = NULL;
static lv_obj_t *s_lbl_count    = NULL;
static lv_obj_t *s_lbl_heard    = NULL;
static lv_obj_t *s_lbl_me       = NULL;

static lv_obj_t *s_list         = NULL;
static row_widgets_t s_rows[MAX_ROWS];

static lv_timer_t *s_t_refresh  = NULL;
static lv_timer_t *s_t_clock    = NULL;

static volatile bool s_refresh_pending = false;

static double s_user_lat = 0.0;
static double s_user_lon = 0.0;
static bool   s_user_loc_valid = false;

// ---------------- shared style init ----------------

static void styles_init(void)
{
    if (s_styles_inited) return;
    s_styles_inited = true;

    // Row container: black bg, thin bottom border, no radius/padding.
    lv_style_init(&s_style_row);
    lv_style_set_bg_color    (&s_style_row, lv_color_hex(0x000000));
    lv_style_set_bg_opa      (&s_style_row, LV_OPA_COVER);
    lv_style_set_radius      (&s_style_row, 0);
    lv_style_set_pad_all     (&s_style_row, 0);
    lv_style_set_border_color(&s_style_row, lv_color_hex(0x303030));
    lv_style_set_border_width(&s_style_row, 1);
    lv_style_set_border_side (&s_style_row, LV_BORDER_SIDE_BOTTOM);

    // Per-column styles. Each owns: font, text colour, text align,
    // width, x offset, y offset. Sharing these saves ~7 local style
    // entries per label.
    #define INIT_COL(s, x, w, align, font, color) \
        lv_style_init(&(s)); \
        lv_style_set_text_font (&(s), (font)); \
        lv_style_set_text_color(&(s), lv_color_hex(color)); \
        lv_style_set_text_align(&(s), (align)); \
        lv_style_set_width     (&(s), (w)); \
        lv_style_set_x         (&(s), (x)); \
        lv_style_set_y         (&(s), 6);

    INIT_COL(s_style_col_call,    COL_CALL_X,    COL_CALL_W,    LV_TEXT_ALIGN_LEFT,  &lv_font_montserrat_24, 0xFFD700);
    INIT_COL(s_style_col_msg,     COL_TEXT_X,    COL_MSG_W,     LV_TEXT_ALIGN_LEFT,  &lv_font_montserrat_24, 0xFFFFFF);
    INIT_COL(s_style_col_country, COL_COUNTRY_X, COL_COUNTRY_W, LV_TEXT_ALIGN_LEFT,  &lv_font_montserrat_24, 0xC0C0C0);
    // SNR base: no colour (per-row), font/pos/align/width are shared.
    lv_style_init(&s_style_col_snr);
    lv_style_set_text_font (&s_style_col_snr, &lv_font_montserrat_24);
    lv_style_set_text_align(&s_style_col_snr, LV_TEXT_ALIGN_RIGHT);
    lv_style_set_width     (&s_style_col_snr, COL_SNR_W);
    lv_style_set_x         (&s_style_col_snr, COL_SNR_X);
    lv_style_set_y         (&s_style_col_snr, 6);

    INIT_COL(s_style_col_km,      COL_KM_X,      COL_KM_W,      LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, 0xC0C0C0);
    INIT_COL(s_style_col_brg,     COL_BRG_X,     COL_BRG_W,     LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, 0xC0C0C0);
    INIT_COL(s_style_col_heard,   COL_HEARD_X,   COL_HEARD_W,   LV_TEXT_ALIGN_RIGHT, &lv_font_montserrat_24, 0xC0C0C0);
    #undef INIT_COL

    // Column header bar.
    lv_style_init(&s_style_header);
    lv_style_set_bg_color(&s_style_header, lv_color_hex(0x202028));
    lv_style_set_bg_opa  (&s_style_header, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_header, 0);
    lv_style_set_radius  (&s_style_header, 0);
    lv_style_set_pad_all (&s_style_header, 0);

    // Header label: smaller font, dim grey, top padding.
    lv_style_init(&s_style_header_label);
    lv_style_set_text_font (&s_style_header_label, &lv_font_montserrat_18);
    lv_style_set_text_color(&s_style_header_label, lv_color_hex(0x808080));
    lv_style_set_y         (&s_style_header_label, 5);
}

// ---------------- helpers ----------------

static int cmp_by_utc_desc(const void *a, const void *b)
{
    const ft8_call_t *ca = (const ft8_call_t *)a;
    const ft8_call_t *cb = (const ft8_call_t *)b;
    if (cb->last_utc < ca->last_utc) return -1;
    if (cb->last_utc > ca->last_utc) return  1;
    return 0;
}

// Set label text only if it differs from cache (avoid redundant invalidate).
static void set_text_if_changed(lv_obj_t *lbl, char *cache, size_t cap, const char *txt)
{
    if (!lbl || !cache || !txt) return;
    if (strncmp(cache, txt, cap) == 0) return;
    strncpy(cache, txt, cap - 1);
    cache[cap - 1] = '\0';
    lv_label_set_text(lbl, txt);
}

static lv_color_t snr_color(int snr)
{
    if (snr >=  0)  return lv_color_hex(0x80FF80);
    if (snr >= -5)  return lv_color_hex(0xFFFFFF);
    if (snr >= -15) return lv_color_hex(0xFFA040);
    return lv_color_hex(0x707070);
}

// Create one label, attach a shared style. No local styles needed.
// Width/position/colour/font/align are all carried by the style.
static lv_obj_t *make_label_styled(lv_obj_t *row, const lv_style_t *style)
{
    lv_obj_t *lbl = lv_label_create(row);
    if (!lbl) return NULL;
    lv_obj_add_style(lbl, (lv_style_t *)style, 0);
    lv_label_set_text(lbl, "");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    return lbl;
}

static void build_row(int i)
{
    row_widgets_t *r = &s_rows[i];

    r->row = lv_obj_create(s_list);
    lv_obj_set_size(r->row, RIGHT_W, ROW_H);
    lv_obj_set_pos(r->row, 0, i * ROW_H);
    lv_obj_add_style(r->row, &s_style_row, 0);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);

    r->l_call    = make_label_styled(r->row, &s_style_col_call);
    r->l_msg     = make_label_styled(r->row, &s_style_col_msg);
    r->l_country = make_label_styled(r->row, &s_style_col_country);
    r->l_snr     = make_label_styled(r->row, &s_style_col_snr);
    r->l_km      = make_label_styled(r->row, &s_style_col_km);
    r->l_brg     = make_label_styled(r->row, &s_style_col_brg);
    r->l_heard   = make_label_styled(r->row, &s_style_col_heard);

    // SNR colour starts white; per-row local override on update.
    if (r->l_snr) {
        lv_obj_set_style_text_color(r->l_snr, lv_color_hex(0xFFFFFF), 0);
    }

    r->prev_call[0]    = '\0';
    r->prev_msg[0]     = '\0';
    r->prev_country[0] = '\0';
    r->prev_snr[0]     = '\0';
    r->prev_km[0]      = '\0';
    r->prev_brg[0]     = '\0';
    r->prev_heard[0]   = '\0';
    r->prev_snr_db     = -127;
}

static void update_row(int i, const ft8_call_t *src)
{
    row_widgets_t *r = &s_rows[i];
    if (!r->row) return;

    const char *country = dxcc_lookup(src->call);
    if (!country) country = "--";

    int snr = (int)src->last_snr_db;
    char b_snr[12], b_km[12], b_brg[12], b_heard[12];
    snprintf(b_snr,   sizeof(b_snr),   "%+d dB", snr);
    snprintf(b_heard, sizeof(b_heard), "(%u)",  (unsigned)src->heard_count);

    if (s_user_loc_valid && src->last_grid[0]) {
        double rlat = 0.0, rlon = 0.0;
        if (maidenhead_to_latlon(src->last_grid, &rlat, &rlon)) {
            double km  = haversine_km(s_user_lat, s_user_lon, rlat, rlon);
            double brg = bearing_deg (s_user_lat, s_user_lon, rlat, rlon);
            snprintf(b_km,  sizeof(b_km),  "%d",   (int)(km + 0.5));
            snprintf(b_brg, sizeof(b_brg), "%d°", (int)(brg + 0.5));
        } else {
            snprintf(b_km,  sizeof(b_km),  "--");
            snprintf(b_brg, sizeof(b_brg), "--");
        }
    } else {
        snprintf(b_km,  sizeof(b_km),  "--");
        snprintf(b_brg, sizeof(b_brg), "--");
    }

    set_text_if_changed(r->l_call,    r->prev_call,    sizeof(r->prev_call),    src->call);
    set_text_if_changed(r->l_msg,     r->prev_msg,     sizeof(r->prev_msg),     src->last_text);
    set_text_if_changed(r->l_country, r->prev_country, sizeof(r->prev_country), country);
    set_text_if_changed(r->l_snr,     r->prev_snr,     sizeof(r->prev_snr),     b_snr);
    set_text_if_changed(r->l_km,      r->prev_km,      sizeof(r->prev_km),      b_km);
    set_text_if_changed(r->l_brg,     r->prev_brg,     sizeof(r->prev_brg),     b_brg);
    set_text_if_changed(r->l_heard,   r->prev_heard,   sizeof(r->prev_heard),   b_heard);

    if (src->last_snr_db != r->prev_snr_db) {
        r->prev_snr_db = src->last_snr_db;
        lv_obj_set_style_text_color(r->l_snr, snr_color(snr), 0);
    }

    if (lv_obj_has_flag(r->row, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_row(int i)
{
    row_widgets_t *r = &s_rows[i];
    if (!r->row) return;
    if (!lv_obj_has_flag(r->row, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rebuild_list(void)
{
    static ft8_call_t snap[FT8_CALL_TABLE_SIZE];
    int n = 0;
    ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
    qsort(snap, n, sizeof(ft8_call_t), cmp_by_utc_desc);

    int shown = n < MAX_ROWS ? n : MAX_ROWS;
    for (int i = 0; i < shown; i++) {
        update_row(i, &snap[i]);
    }
    for (int i = shown; i < MAX_ROWS; i++) {
        hide_row(i);
    }

    if (s_lbl_heard) {
        char b[32];
        snprintf(b, sizeof(b), "Heard: %d", n);
        lv_label_set_text(s_lbl_heard, b);
    }
}

static void t_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_refresh_pending) return;
    s_refresh_pending = false;
    rebuild_list();
}

static void t_clock_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;

    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);

    if (s_lbl_utc) {
        char b[16];
        snprintf(b, sizeof(b), "%02d:%02d:%02d UTC",
                 utc.tm_hour, utc.tm_min, utc.tm_sec);
        lv_label_set_text(s_lbl_utc, b);
    }
    if (s_lbl_count) {
        int sec_in_slot = (int)(now % 15);
        int remain = 15 - sec_in_slot;
        char b[16];
        snprintf(b, sizeof(b), "Slot: %2d s", remain);
        lv_label_set_text(s_lbl_count, b);
    }
    if (s_lbl_freq) {
        uint32_t hz = cat_get_frequency();
        char b[32];
        uint32_t mhz = hz / 1000000;
        uint32_t khz = (hz / 1000) % 1000;
        uint32_t rem = hz % 1000;
        snprintf(b, sizeof(b), "%lu.%03lu.%03lu",
                 (unsigned long)mhz, (unsigned long)khz, (unsigned long)rem);
        lv_label_set_text(s_lbl_freq, b);
    }
}

// ---------------- public API ----------------

void ft8_screen_view_init(lv_obj_t *parent)
{
    styles_init();

    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, MID_W, MID_H);
    lv_obj_set_pos(s_container, 0, MID_Y);
    lv_obj_set_style_bg_color(s_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_radius(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);

    s_left_pane = lv_obj_create(s_container);
    lv_obj_set_size(s_left_pane, LEFT_W, MID_H);
    lv_obj_set_pos(s_left_pane, 0, 0);
    lv_obj_set_style_bg_color(s_left_pane, lv_color_hex(0x101018), 0);
    lv_obj_set_style_border_width(s_left_pane, 0, 0);
    lv_obj_set_style_radius(s_left_pane, 0, 0);
    lv_obj_set_style_pad_all(s_left_pane, 16, 0);
    lv_obj_clear_flag(s_left_pane, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_mode = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_mode, "MODE: FT8");
    lv_obj_set_style_text_color(s_lbl_mode, lv_color_hex(0xFFD700), 0);
    lv_obj_set_style_text_font(s_lbl_mode, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(s_lbl_mode, 0, 0);

    s_lbl_freq = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_freq, "--.---.---");
    lv_obj_set_style_text_color(s_lbl_freq, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_freq, &lv_font_montserrat_32, 0);
    lv_obj_set_pos(s_lbl_freq, 0, 80);

    s_lbl_utc = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_utc, "--:--:-- UTC");
    lv_obj_set_style_text_color(s_lbl_utc, lv_color_hex(0xA0FFA0), 0);
    lv_obj_set_style_text_font(s_lbl_utc, &lv_font_montserrat_32, 0);
    lv_obj_set_pos(s_lbl_utc, 0, 160);

    s_lbl_count = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_count, "Slot: -- s");
    lv_obj_set_style_text_color(s_lbl_count, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_lbl_count, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_count, 0, 240);

    s_lbl_heard = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_heard, "Heard: 0");
    lv_obj_set_style_text_color(s_lbl_heard, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_lbl_heard, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_heard, 0, 300);

    s_lbl_me = lv_label_create(s_left_pane);
    lv_label_set_text(s_lbl_me, "");
    lv_obj_set_style_text_color(s_lbl_me, lv_color_hex(0xA0FFA0), 0);
    lv_obj_set_style_text_font(s_lbl_me, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_lbl_me, 0, 360);
    lv_obj_add_flag(s_lbl_me, LV_OBJ_FLAG_HIDDEN);

    // Right pane
    s_right_pane = lv_obj_create(s_container);
    lv_obj_set_size(s_right_pane, RIGHT_W, MID_H);
    lv_obj_set_pos(s_right_pane, LEFT_W, 0);
    lv_obj_set_style_bg_color(s_right_pane, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_right_pane, 0, 0);
    lv_obj_set_style_radius(s_right_pane, 0, 0);
    lv_obj_set_style_pad_all(s_right_pane, 0, 0);
    lv_obj_clear_flag(s_right_pane, LV_OBJ_FLAG_SCROLLABLE);

    // Column header (shared styles)
    lv_obj_t *hdr = lv_obj_create(s_right_pane);
    lv_obj_set_size(hdr, RIGHT_W, 30);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_add_style(hdr, &s_style_header, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    struct { const char *t; int x; int w; lv_text_align_t a; } cols[7] = {
        { "CALL",    COL_CALL_X,    COL_CALL_W,    LV_TEXT_ALIGN_LEFT  },
        { "MESSAGE", COL_TEXT_X,    COL_MSG_W,     LV_TEXT_ALIGN_LEFT  },
        { "COUNTRY", COL_COUNTRY_X, COL_COUNTRY_W, LV_TEXT_ALIGN_LEFT  },
        { "SNR",     COL_SNR_X,     COL_SNR_W,     LV_TEXT_ALIGN_RIGHT },
        { "KM",      COL_KM_X,      COL_KM_W,      LV_TEXT_ALIGN_RIGHT },
        { "BRG",     COL_BRG_X,     COL_BRG_W,     LV_TEXT_ALIGN_RIGHT },
        { "HRD",     COL_HEARD_X,   COL_HEARD_W,   LV_TEXT_ALIGN_RIGHT },
    };
    for (int i = 0; i < 7; i++) {
        lv_obj_t *lbl = lv_label_create(hdr);
        lv_obj_add_style(lbl, &s_style_header_label, 0);
        lv_label_set_text(lbl, cols[i].t);
        lv_obj_set_width(lbl, cols[i].w);
        lv_obj_set_x(lbl, cols[i].x);
        lv_obj_set_style_text_align(lbl, cols[i].a, 0);
    }

    // Scrolling list container
    s_list = lv_obj_create(s_right_pane);
    lv_obj_set_size(s_list, RIGHT_W, MID_H - 30);
    lv_obj_set_pos(s_list, 0, 30);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    // Pre-allocate the row pool using shared styles.
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    memset(s_rows, 0, sizeof(s_rows));
    for (int i = 0; i < MAX_ROWS; i++) {
        build_row(i);
    }
    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "row pool built (%d rows, heap_i %u -> %u, delta %d KB)",
             MAX_ROWS, (unsigned)heap_before, (unsigned)heap_after,
             ((int)heap_after - (int)heap_before) / 1024);

    s_t_refresh = lv_timer_create(t_refresh_cb, 500, NULL);
    s_t_clock   = lv_timer_create(t_clock_cb,  1000, NULL);

    ESP_LOGI(TAG, "FT8 view built (container %dx%d at y=%d, hidden)",
             MID_W, MID_H, MID_Y);
}

void ft8_screen_view_show(void)
{
    if (!s_container) return;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_refresh_pending = true;

    if (s_lbl_me) {
        qmx_settings_t s;
        settings_load_all(&s);
        if (s.my_callsign[0]) {
            char buf[40];
            if (s.my_grid[0]) {
                snprintf(buf, sizeof(buf), "ME: %s %s", s.my_callsign, s.my_grid);
            } else {
                snprintf(buf, sizeof(buf), "ME: %s", s.my_callsign);
            }
            lv_label_set_text(s_lbl_me, buf);
            lv_obj_clear_flag(s_lbl_me, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_me, LV_OBJ_FLAG_HIDDEN);
        }
        s_user_loc_valid = false;
        if (s.my_grid[0]) {
            s_user_loc_valid = maidenhead_to_latlon(s.my_grid,
                                                    &s_user_lat,
                                                    &s_user_lon);
        }
    }
    ESP_LOGI(TAG, "show");
}

void ft8_screen_view_hide(void)
{
    if (!s_container) return;
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "hide");
}

void ft8_screen_view_request_refresh(void)
{
    s_refresh_pending = true;
}
