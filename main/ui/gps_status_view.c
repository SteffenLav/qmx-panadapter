#include "gps_status_view.h"
#include "gps_page.h"
#include "ui.h"
#include "ui_theme.h"
#include "qmx_term_view.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <time.h>

#include "cat/cat.h"
#include "time_sync/time_sync.h"
#include "unit_gps/unit_gps.h"

static const char *TAG = "gps_view";

LV_FONT_DECLARE(qmx_mono_25);

/* Same geometry as qmx_term_view, deliberately: the page it shows is a copy of
 * the radio's own GPS viewer, and the two must not read as two designs. See
 * the note at the top of font_qmx_mono_25.c - CELL_W has to match the font's
 * advance width exactly or every column drifts along its row. */
#define CELL_W     15
#define ROW_H      27
#define GRID_W     (GPS_PAGE_COLS * CELL_W)   /* 1200 */
#define GRID_H     (GPS_PAGE_ROWS * ROW_H)    /* 648  */
#define HEADER_H   62
#define REFRESH_MS 1000

static lv_obj_t  *s_overlay;
static lv_obj_t  *s_rows[GPS_PAGE_ROWS];
static lv_obj_t  *s_marks[GPS_PAGE_MAX_MARKERS];
static lv_obj_t  *s_title;
/* SCRATCH IN PSRAM, NOT .bss, AND ONLY WHILE THE PAGE IS OPEN.
 *
 * These four buffers are about 4.3 kB together. As statics they sat in
 * internal .bss - and internal .bss comes straight out of the DMA pool, so the
 * SD card then would not mount at all: "SDFAIL[mount] err=0x101 | DMA
 * free=1235 lblk=1024", measured on the bench 2026-10-08. The card was fine;
 * this code took its memory. See project_internal_bss_root_cause.
 *
 * They cannot go on the stack either: repaint() runs on the HTTPD task for the
 * dev action, and 4 kB there is the stack protection fault that crashed it
 * earlier the same evening. PSRAM is the only place left, and a modal that is
 * open for a minute has no business holding internal RAM anyway. */
typedef struct {
    unit_gps_info_t   info;
    char              lines[GPS_PAGE_ROWS][GPS_PAGE_COLS + 1];
    gps_page_marker_t marks[GPS_PAGE_MAX_MARKERS];
    gps_page_satrow_t satrows[UNIT_GPS_MAX_SATS];
} gps_view_scratch_t;

static gps_view_scratch_t *s_scratch;                  /* PSRAM, open..close */
static lv_obj_t  *s_sat_list;                          /* scrollable table pane */
static lv_obj_t  *s_sat_rows[UNIT_GPS_MAX_SATS];
static lv_timer_t *s_timer;
static bool       s_open;

bool gps_status_view_is_open(void) { return s_open; }

/* Which receiver is disciplining the clock, or neither.
 *
 * ⛔ NTP, RTC, FT8 and manual all return "neither". The tap then does nothing:
 * a page of dashes is worse than no page, because it reads as a receiver that
 * has failed rather than as no receiver at all. */
bool gps_status_source(gps_page_src_t *out)
{
    switch (time_sync_get_effective_source()) {
    case TIME_SOURCE_UNIT_GPS:
        if (out) *out = GPS_PAGE_SRC_MODULE;
        return true;
    case TIME_SOURCE_QMX:
        if (!time_sync_qmx_gps_confirmed()) return false;
        if (out) *out = GPS_PAGE_SRC_QMX;
        return true;
    default:
        return false;
    }
}

bool gps_status_view_available(void) { return gps_status_source(NULL); }

/* The QMX reports position and time over CAT and nothing else - no satellite
 * count, no SNR, no elevation or azimuth. Those rows stay on the page and read
 * "-" (see gps_page.c), so it is visibly the same page with the radio not
 * reporting, rather than a second page. */
static void fill_from_qmx(unit_gps_info_t *in)
{
    memset(in, 0, sizeof(*in));
    in->avg_snr = -1;
    in->hdop    = -1.0f;

    double lat, lon;
    if (cat_qmx_gps_position(&lat, &lon)) {
        in->has_pos = true;
        in->lat_deg = lat;
        in->lon_deg = lon;
        in->valid   = true;
    }

    /* The time shown is the SYSTEM clock, which this source is what disciplines
     * - not a separate reading. Taking a second reading here would invite the
     * page and the bottom bar to disagree by a second and look like a fault. */
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    if (tm_utc.tm_year > 100) {
        in->has_time = true;
        in->year = tm_utc.tm_year + 1900;
        in->mon  = tm_utc.tm_mon + 1;
        in->mday = tm_utc.tm_mday;
        in->hour = tm_utc.tm_hour;
        in->min  = tm_utc.tm_min;
        in->sec  = tm_utc.tm_sec;
    }
}

static void repaint(void)
{
    gps_page_src_t src;
    if (!gps_status_source(&src)) { gps_status_view_close(); return; }

    /* Scratch lives in s_scratch (PSRAM) - see its declaration for why it is
     * neither a static nor a local. Shared safely because this view is a
     * singleton and every caller holds the display lock. */
    if (!s_scratch) return;
    unit_gps_info_t   *info  = &s_scratch->info;
    gps_page_marker_t *marks = s_scratch->marks;
    char             (*lines)[GPS_PAGE_COLS + 1] = s_scratch->lines;
    int n_marks = 0;

    /* Only the module reaches here - the QMX source is handed to the radio's
     * own viewer at open time. fill_from_qmx() stays for the GPS_PAGE_SRC_QMX
     * layout the harness still checks, and for the day CAT grows a satellite
     * command. */
    if (src == GPS_PAGE_SRC_MODULE) unit_gps_get_info(info);
    else                            fill_from_qmx(info);
    /* compose, not render: the shared copy of the two blanking loops that used
     * to live below. See gps_page_compose(). */
    gps_page_compose(info, src, lines, marks, &n_marks);

    /* The pin names the port in the title. Read live rather than cached: the
     * driver can be rebound to the other port at runtime (unit_gps_start_on),
     * and a title describing the port it used to be on would be exactly the
     * mislabel this replaced. */
    if (s_title) lv_label_set_text(s_title, gps_page_title(src, unit_gps_rx_gpio()));

    /* gps_page_compose() has already blanked the marker cells and the
     * satellite-table region: each marker is drawn below as its own coloured
     * label, and the table by the scrollable pane. The row keeps the plot's
     * dots; only the satellites are coloured. Same approach qmx_term_view uses
     * for the radio's colour runs.
     * ⛔ The two blanking loops that stood here were copied into the web page
     * and are now shared - do not bring them back. */

    for (int r = 0; r < GPS_PAGE_ROWS; r++) {
        int e = GPS_PAGE_COLS;
        while (e > 0 && lines[r][e - 1] == ' ') e--;
        lines[r][e] = '\0';
        if (s_rows[r]) lv_label_set_text(s_rows[r], lines[r]);
    }

    /* The table, coloured by SNR exactly as the sky plot is - one satellite
     * must not be green in the plot and amber in the list. */
    gps_page_satrow_t *satrows = s_scratch->satrows;
    int n_rows = (src == GPS_PAGE_SRC_MODULE)
               ? gps_page_sat_rows(info, satrows, UNIT_GPS_MAX_SATS) : 0;
    for (int i = 0; i < UNIT_GPS_MAX_SATS; i++) {
        if (!s_sat_rows[i]) continue;
        if (i < n_rows) {
            lv_label_set_text(s_sat_rows[i], satrows[i].text);
            lv_obj_set_style_text_color(s_sat_rows[i],
                                        lv_color_hex(gps_page_snr_colour(satrows[i].snr_db)), 0);
            lv_obj_clear_flag(s_sat_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_sat_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* ⛔ DO NOT size the pane to its content here.
     *
     * The first version called lv_obj_set_content_height(s_sat_list,
     * n_rows * ROW_H), which does the opposite of what it sounds like: it
     * GROWS THE OBJECT so its content area is that tall. The pane became 20
     * rows high, overflowed the grid, and scrolled nothing - reported from the
     * glass, 2026-10-08, after I had called it done without looking.
     *
     * The pane keeps the fixed height it was built with, and LVGL derives the
     * scrollable extent from the visible children. Hidden children are left
     * out of that calculation, which is exactly what is wanted: only the
     * satellites actually in view scroll. */

    for (int i = 0; i < GPS_PAGE_MAX_MARKERS; i++) {
        if (!s_marks[i]) continue;
        if (i < n_marks) {
            lv_label_set_text(s_marks[i], marks[i].text);
            /* Compass labels blue, satellites by SNR - the radio's own plot
             * reads that way, and the blue is what stops 0/90/180/270 being
             * mistaken for a satellite with a two-digit PRN. */
            lv_obj_set_style_text_color(
                s_marks[i],
                lv_color_hex(marks[i].compass ? 0x5B8DEF
                                              : gps_page_snr_colour(marks[i].snr_db)), 0);
            lv_obj_set_pos(s_marks[i], marks[i].col * CELL_W, marks[i].row * ROW_H);
            lv_obj_clear_flag(s_marks[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_marks[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void tick_cb(lv_timer_t *t) { (void)t; repaint(); }

static void close_cb(lv_event_t *e) { (void)e; gps_status_view_close(); }

void gps_status_view_open(void)
{
    if (s_open) return;
    if (!gps_status_view_available()) {
        /* Nothing to show. Silent rather than a toast: the operator tapped a
         * clock, not a button, and a complaint for a tap they may not have
         * meant is noise. */
        return;
    }

    /* ⭐ The QMX source is the RADIO'S OWN GPS VIEWER, not a page of ours.
     *
     * Over CAT the radio gives position and time and nothing else - no
     * satellites, no SNR, no fix type - so a page drawn here would be three
     * rows of data and eight of dashes. Its own viewer has all of it, and the
     * Tab5 can already render that screen. So this hands over to the terminal
     * view, titled "GPS - QMX internal receiver".
     *
     * ⚠ That takes the radio into its menus and stops the panadapter while it
     * is open. It is the same trip the operator would make by hand, and the
     * only data path there is. */
    {
        gps_page_src_t which;
        if (gps_status_source(&which) && which == GPS_PAGE_SRC_QMX) {
            qmx_term_view_open_gps();
            return;
        }
    }

    s_scratch = heap_caps_calloc(1, sizeof(*s_scratch), MALLOC_CAP_SPIRAM);
    if (!s_scratch) {
        ESP_LOGE(TAG, "no PSRAM for the GPS page scratch (%u B) - not opening",
                 (unsigned)sizeof(*s_scratch));
        return;
    }

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps below */

    lv_obj_t *hdr = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_HOR_RES, HEADER_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(hdr);
    lv_label_set_text(s_title, "GPS");
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *cb = lv_btn_create(hdr);
    lv_obj_set_size(cb, 92, 48);
    lv_obj_align(cb, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x3a2222), 0);
    lv_obj_set_style_border_color(cb, lv_color_hex(0xB05050), 0);
    lv_obj_set_style_border_width(cb, 2, 0);
    lv_obj_add_event_cb(cb, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cl);

    lv_obj_t *grid = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, GRID_W, GRID_H);
    lv_obj_set_pos(grid, (LV_HOR_RES - GRID_W) / 2, HEADER_H + 4);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int r = 0; r < GPS_PAGE_ROWS; r++) {
        s_rows[r] = lv_label_create(grid);
        lv_obj_set_style_text_font(s_rows[r], &qmx_mono_25, 0);
        lv_obj_set_style_text_color(s_rows[r], lv_color_hex(0xD8D8D8), 0);
        lv_label_set_long_mode(s_rows[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_rows[r], GRID_W);
        lv_obj_set_pos(s_rows[r], 0, r * ROW_H);
        lv_label_set_text(s_rows[r], "");
    }
    /* ⭐ The satellite table is a SCROLLABLE pane over the grid's table region.
     *
     * The page is 24 rows and the table starts at row 12, so the grid itself
     * can show twelve satellites; the bench sees twenty. Truncating would hide
     * exactly the weak ones an operator opens this page to look at.
     *
     * It covers only the table's columns, so the sky plot to its right stays
     * part of the grid and does not scroll with it. */
    s_sat_list = lv_obj_create(grid);
    lv_obj_remove_style_all(s_sat_list);
    lv_obj_set_size(s_sat_list, GPS_PAGE_SAT_COLS * CELL_W,
                    (GPS_PAGE_ROWS - GPS_PAGE_TABLE_ROW0) * ROW_H);
    lv_obj_set_pos(s_sat_list, 0, GPS_PAGE_TABLE_ROW0 * ROW_H);
    lv_obj_set_style_pad_all(s_sat_list, 0, 0);
    lv_obj_set_style_bg_opa(s_sat_list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_sat_list, LV_OBJ_FLAG_SCROLLABLE);   /* remove_style_all left it, be explicit */
    lv_obj_set_scroll_dir(s_sat_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_sat_list, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_bg_color(s_sat_list, lv_color_hex(0x606060), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_sat_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_sat_list, 6, LV_PART_SCROLLBAR);

    for (int i = 0; i < UNIT_GPS_MAX_SATS; i++) {
        s_sat_rows[i] = lv_label_create(s_sat_list);
        lv_obj_set_style_text_font(s_sat_rows[i], &qmx_mono_25, 0);
        lv_label_set_long_mode(s_sat_rows[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_sat_rows[i], GPS_PAGE_SAT_COLS * CELL_W);
        lv_obj_set_pos(s_sat_rows[i], 0, i * ROW_H);
        lv_label_set_text(s_sat_rows[i], "");
        lv_obj_add_flag(s_sat_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Created once and hidden when unused, rather than created and destroyed
     * every second: a per-second churn of LVGL objects is how the FT8 screen
     * once fragmented its heap. */
    for (int i = 0; i < GPS_PAGE_MAX_MARKERS; i++) {
        s_marks[i] = lv_label_create(grid);
        lv_obj_set_style_text_font(s_marks[i], &qmx_mono_25, 0);
        lv_label_set_long_mode(s_marks[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_marks[i], "");
        lv_obj_add_flag(s_marks[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_open = true;
    repaint();
    s_timer = lv_timer_create(tick_cb, REFRESH_MS, NULL);
    ui_help_overlay_changed();
    ESP_LOGI(TAG, "GPS page opened");
}

void gps_status_view_close(void)
{
    if (!s_open) return;
    s_open = false;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    if (s_scratch) { heap_caps_free(s_scratch); s_scratch = NULL; }
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_marks, 0, sizeof(s_marks));
    memset(s_sat_rows, 0, sizeof(s_sat_rows));
    s_sat_list = NULL;
    s_title = NULL;
    ui_help_overlay_changed();
    ESP_LOGI(TAG, "GPS page closed");
}
