// On-device ADIF log viewer - read-only, newest QSO first. Lets the operator
// check "did I already work this station" / verify a QSO actually logged
// without pulling qso.adi off the device. Field request (Ken KF0AYY): during
// a POTA activation with marginal copy, a QSO can complete on his end while
// his partner never copies the final RR73 and keeps re-sending a report -
// this gives him a quick way to confirm it logged before working them again.
//
// Structurally a copy of wifi_config.c's SSID-picker pattern: a centred,
// content-sized panel holding a title + a scrollable lv_list + a Close
// button. Read-only, so no per-row tap action.

#include "adif_view_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "adif/adif_log.h"
#include "util/dxcc.h"
#include "storage/sd_archive.h"   // "Restore from SD" - card presence + the read
#include "util/psram_task.h"      // the restore runs OFF taskLVGL; see restore_task()

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>   // toupper - the search is case-insensitive
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "adif_view_modal";

// Most recent QSOs shown - the log can grow unbounded over a long
// activation; capping keeps modal build time and memory bounded. Plenty for
// a "did I already work them" field check.
#define ADIF_VIEW_MAX_ROWS 200

// A POTA activation needs 10 QSOs to count - the whole reason for the Today
// filter (Ken KF0AYY field request: "so I know how close to 10 I am"). Once
// today's count reaches this, the title flips green: park is open.
#define POTA_ACTIVATION_QSOS 10
#define COLOR_ACTIVATION_OK  0x4caf50

// LVGL's object pool is a fixed tlsf arena (CONFIG_LV_MEM_SIZE_KILOBYTES,
// PSRAM-backed via lv_pool_shim). Each QSO row is 9 objects (a flex container +
// 8 column labels) costing ~2 KB, so a large enough log builds enough rows to
// exhaust the pool - and LVGL does NOT null-check an allocation failure: it
// dereferences a NULL object and the device panics (load-access fault,
// field-observed 2026-07-19 at only 40 QSOs, because the FT8 screen already
// consumes ~200 KB and only ~58 KB was free when the modal opened). The primary
// fix is sizing the pool (1024 KB) to hold the full ADIF_VIEW_MAX_ROWS cap with
// margin, so the whole log renders as before. This guard is a BACKSTOP: build
// newest-first and stop before the pool runs low (showing a footer for the
// remainder) so we degrade gracefully instead of crashing if the UI's headroom
// is ever squeezed below what the cap needs. In normal operation it never fires.
// The reserve exceeds the worst-case cost of one more row plus room for the
// delete bar and the rest of the live UI.
#define ADIF_VIEW_LVGL_RESERVE (48 * 1024)

static size_t lvgl_free_bytes(void)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    return mon.free_size;
}

static lv_obj_t *s_modal      = NULL;
static lv_obj_t *s_panel      = NULL;
static lv_obj_t *s_title      = NULL;
static lv_obj_t *s_list       = NULL;
static lv_obj_t *s_lbl_filter = NULL;   // label inside the Today/All toggle button
static bool      s_open       = false;

// "Delete test QSOs" (operator request 2026-07-23): FT8 Simulation Mode
// contacts log with no CAT frequency (FREQ 0.000000 - a real contact always
// carries one), which is how they're recognized here. The button only exists
// while such records are present, so operators who never simulate never see
// it. Two-tap confirm: first tap arms ("Sure?"), second within 5 s deletes.
static lv_obj_t *s_btn_del_test  = NULL;
static lv_obj_t *s_lbl_del_test  = NULL;
static int       s_test_count    = 0;      // FREQ==0 records at last render
static int64_t   s_del_test_arm_us = 0;    // 0 = not armed

// "Delete all" (Don WB0LQW, 2026-07-30): clear the whole log from the Tab5 -
// the POTA case, where the operator starts an activation with an empty log so
// the ADIF at the end is exactly the submission, with no PC or WiFi around.
// Same two-tap confirm as the test-delete, but the armed label spells out the
// stakes and the count, since this destroys the entire log with no undo.
static lv_obj_t *s_btn_del_all  = NULL;
static lv_obj_t *s_lbl_del_all  = NULL;
static int64_t   s_del_all_arm_us = 0;     // 0 = not armed

// Show only today's (UTC) QSOs - the POTA-activation view. Defaults to Today
// on open; show() falls back to All when nothing was logged today (reviewing
// an old log at home shouldn't open onto an empty list).
static bool s_today_only = true;

// Search. Gyula HA3HZ: "I like the LOG to tell me if there was a connection and
// on which band and mode. If it turns gray, then I know there was." The grey-out
// in the decode list only answers that when the station happens to be on the air
// at that moment; this asks the same question whenever he likes, and on the Tab5
// rather than only in the browser - the screen he is actually sitting at.
static lv_obj_t *s_search_ta, *s_search_kb;
static char      s_query[24];        // uppercased; empty = show everything

// Panel geometry. The list cap SHRINKS while the keyboard is up, because a
// search you cannot see the results of is not a search - see kb_show().
#define ADIF_LIST_MAX_H      370
// 430 until the search row was added. That row is 48 px plus the panel's
// 12 px pad_row - 60 px the panel did not have, so "Delete all / Restore
// from SD / Close" were pushed off the bottom of the screen. The list pays
// for the row it sits under (about 1.5 QSOs); the panel cap is unchanged,
// because 690 was chosen to keep those buttons on a 720 px display and that
// reasoning still holds. Anything added to this panel has to come out of
// here too.
#define ADIF_PANEL_MAX_H     690
#define ADIF_KB_H            280     // same height every other modal's keyboard uses
#define ADIF_LIST_MAX_H_KB   110     // ~2-3 rows still visible above the keys
// Measured off a bench screenshot: with the keyboard up the panel used ~365 px
// of its 420 px budget while the list was capped at 150 and the search box
// still shared the header. Giving search its own ~56 px row would have pushed
// that to ~421 and clipped the buttons off the bottom, so the list gives the
// row back. Re-measure if any of these three change.
#define ADIF_PANEL_MAX_H_KB  (720 - ADIF_KB_H - 20)

// Counts from the most recent list_render() pass, for show()'s fallback.
static int s_last_total = 0;
static int s_last_today = 0;

// --- Single-record delete (operator request 2026-07-16) --------------------
// Long-press a QSO row -> selection mode (list scroll locks, dragging up/down
// moves the highlight) -> release -> Delete/Cancel bar. Delete removes that
// one record from the ADIF file (duplicates, botched entries). Each row
// carries its 0-based file record index in lv_obj user_data.
static lv_obj_t *s_del_bar     = NULL;   // confirm bar (hidden by default)
static lv_obj_t *s_del_lbl     = NULL;   // "Delete <CALL> <date>?" text
static lv_obj_t *s_sel_row     = NULL;   // currently highlighted row
static int       s_sel_fidx    = -1;     // its file record index
static bool      s_sel_active  = false;  // finger down in selection mode

static void sel_highlight(lv_obj_t *row)
{
    if (s_sel_row == row) return;
    if (s_sel_row) {
        lv_obj_set_style_border_width(s_sel_row, 0, 0);
        lv_obj_set_style_bg_color(s_sel_row, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
        // (bg_opa of odd rows was transparent; harmless to leave the color -
        // cancel/delete re-render the list anyway.)
    }
    s_sel_row  = row;
    s_sel_fidx = row ? (int)(intptr_t)lv_obj_get_user_data(row) : -1;
    if (row) {
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x5a1f1f), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0xff5050), 0);
        lv_obj_set_style_border_width(row, 2, 0);
    }
}

static void del_bar_show(void)
{
    if (!s_del_bar || s_sel_fidx < 0) return;
    char line[512], call[20] = "?", date[9] = "", rcvd[8] = "";
    if (adif_log_get_record(s_sel_fidx, line, sizeof(line))) {
        adif_log_extract_field(line, "CALL",     call, sizeof(call));
        adif_log_extract_field(line, "QSO_DATE", date, sizeof(date));
        adif_log_extract_field(line, "RST_RCVD", rcvd, sizeof(rcvd));
    }
    if (s_del_lbl)
        lv_label_set_text_fmt(s_del_lbl, "Delete %s  %s  rcvd %s ?", call, date, rcvd);
    lv_obj_clear_flag(s_del_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_del_bar);
}

static void list_render(void);   // fwd (sel_reset re-renders)
static void kb_hide(void);       // fwd (modal_close stows the keyboard)

// ---------------------------------------------------------------------------
// "Restore from SD" - put the log back from the card's own mirror.
//
// The auto-archive has always copied qso.adi ON to the card and never back off
// it, so a log lost to a clean reinstall needed a computer, a browser, and
// knowing the file was on the card at all. Gyula HA3HZ had 432 QSOs mirrored on
// the card, inside the device, out of reach - and at a POTA site there is no
// computer to reach them with either. A backup you cannot restore from the
// device is not a backup.
//
// ⛔ It does NOT run on taskLVGL. sd_archive_read_adif() takes the archive lock
// (up to 5 s), reads up to ~100 KB over SPI, and the import then rewrites
// SPIFFS - seconds of work. The QMX terminal already learned this the hard way:
// a 2.4 s blocking open froze taskLVGL. So the button starts a worker and a
// 200 ms poll timer re-renders the list once it has finished.
// A result the operator has to READ gets a window with an OK button, not a
// toast. A toast is right for something you may glance at and may miss - "SD
// card removed" - and wrong for the outcome of an action you just asked for:
// it fades on its own timer, it can be missed entirely while you are looking at
// the log underneath it, and "restored 432 of 432" is the whole answer to the
// question the button was pressed to ask. Dismissing it is the acknowledgement.
static lv_obj_t *s_msg_modal, *s_msg_title, *s_msg_text;

static void msg_ok_cb(lv_event_t *e)
{
    (void)e;
    if (s_msg_modal) lv_obj_add_flag(s_msg_modal, LV_OBJ_FLAG_HIDDEN);
}

// Built lazily and kept, like every other modal here. Two lines of body text at
// montserrat_24 is the design size; the panel hugs whatever it is given.
static void msg_show(const char *title_text, const char *body_text, bool good)
{
    if (!s_msg_modal) {
        lv_obj_t *scr = lv_scr_act();
        s_msg_modal = lv_obj_create(scr);
        lv_obj_set_size(s_msg_modal, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(s_msg_modal, 0, 0);
        lv_obj_set_style_bg_color(s_msg_modal, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_msg_modal, UI_OPA_MODAL_SCRIM, 0);
        lv_obj_set_style_border_width(s_msg_modal, 0, 0);
        lv_obj_set_style_radius(s_msg_modal, 0, 0);
        lv_obj_set_style_pad_all(s_msg_modal, 0, 0);
        lv_obj_clear_flag(s_msg_modal, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *panel = lv_obj_create(s_msg_modal);
        lv_obj_set_size(panel, 760, LV_SIZE_CONTENT);
        lv_obj_center(panel);
        lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(panel, 2, 0);
        lv_obj_set_style_radius(panel, 10, 0);
        lv_obj_set_style_pad_all(panel, 28, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(panel, 20, 0);

        s_msg_title = lv_label_create(panel);
        lv_obj_set_style_text_font(s_msg_title, &lv_font_montserrat_32, 0);

        s_msg_text = lv_label_create(panel);
        lv_label_set_long_mode(s_msg_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_msg_text, LV_PCT(100));
        lv_obj_set_style_text_align(s_msg_text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_msg_text, lv_color_hex(0xdddddd), 0);
        lv_obj_set_style_text_font(s_msg_text, &lv_font_montserrat_24, 0);

        // OK is the ONLY control, and it is neutral: this window reports what
        // already happened, so there is nothing here to accept or refuse.
        lv_obj_t *ok = lv_btn_create(panel);
        lv_obj_set_size(ok, 220, 72);
        lv_obj_set_style_bg_color(ok, lv_color_hex(0x2a3138), 0);
        lv_obj_set_style_border_color(ok, lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_set_style_border_width(ok, 2, 0);
        lv_obj_set_style_radius(ok, 8, 0);
        lv_obj_add_event_cb(ok, msg_ok_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ok_lbl = lv_label_create(ok);
        lv_label_set_text(ok_lbl, "OK");
        lv_obj_set_style_text_color(ok_lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_24, 0);
        lv_obj_center(ok_lbl);
        // Deliberately NOT ui_kbd_set_buttons(): the log viewer already owns
        // Esc -> Close, and nothing here would hand it back on dismissal.
    }

    lv_label_set_text(s_msg_title, title_text);
    lv_obj_set_style_text_color(s_msg_title,
        lv_color_hex(good ? UI_COLOR_SUCCESS_BORDER : 0xffa040), 0);
    lv_label_set_text(s_msg_text, body_text);
    lv_obj_clear_flag(s_msg_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_msg_modal);   // the log viewer is a sibling underneath
}

typedef enum { SDR_IDLE = 0, SDR_RUNNING, SDR_DONE } sdr_state_t;
static volatile sdr_state_t s_sdr_state = SDR_IDLE;
static adif_import_result_t s_sdr_result;
static volatile int         s_sdr_added;
static lv_timer_t          *s_sdr_timer;

static void restore_task(void *arg)
{
    (void)arg;
    adif_import_result_t r;
    int added = adif_log_import_from_sd(&r);
    s_sdr_result = r;
    s_sdr_added  = added;
    s_sdr_state  = SDR_DONE;
    psram_task_park();   // reapable task: never vTaskDelete (leaks the stack)
}

// Runs on taskLVGL, so it may touch LVGL and the list.
static void restore_poll_cb(lv_timer_t *t)
{
    if (s_sdr_state != SDR_DONE) return;

    const adif_import_result_t *r = &s_sdr_result;
    const char *title = "Restore from SD";
    char msg[240];
    bool good = false;
    if (s_sdr_added < 0 && r->found == 0) {
        title = "No log on the card";
        snprintf(msg, sizeof(msg),
                 "Nothing was restored.\n\nCheck the card is in the slot and holds "
                 "qmx-panadapter/qso.adi.");
    } else if (s_sdr_added < 0) {
        title = "Restore failed";
        snprintf(msg, sizeof(msg),
                 "The log could not be written.\n\nThe storage may be full.");
    } else if (r->added) {
        // Say what happened to ALL of them, not just the ones that landed: an
        // import reporting only "added" cannot tell "already logged" from
        // "unreadable", and reporting the first when it was the second is a
        // false statement about someone's log.
        good = true;
        title = "Restored";
        int n = snprintf(msg, sizeof(msg),
                         "%d contact%s restored from the SD card.\n\n"
                         "%d already in the log.",
                         r->added, r->added == 1 ? "" : "s", r->duplicate);
        // Only mentioned when there were any: a permanent "0 could not be read"
        // invites worry about a number that is almost always zero.
        if (r->unreadable > 0 && n > 0 && n < (int)sizeof(msg))
            snprintf(msg + n, sizeof(msg) - n, "\n%d could not be read.", r->unreadable);
    } else if (r->found) {
        good = true;
        title = "Nothing to restore";
        snprintf(msg, sizeof(msg),
                 "All %d contacts on the card are already in the log.", r->duplicate);
    } else {
        title = "Nothing to restore";
        snprintf(msg, sizeof(msg), "No contacts were found in the card's log file.");
    }
    msg_show(title, msg, good);
    ESP_LOGI(TAG, "SD restore: %s - %s", title, msg);

    s_sdr_state = SDR_IDLE;
    lv_timer_del(t);
    s_sdr_timer = NULL;
    list_render();   // the log just changed under the open list
}

static void restore_sd_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_sdr_state != SDR_IDLE) return;   // one at a time
    if (!sd_archive_is_mounted()) {
        msg_show("No SD card", "There is no card in the slot to restore from.", false);
        return;
    }

    // No confirm gesture, deliberately, and unlike "Delete all" next to it:
    // this only ever ADDS contacts, and one already in the log is skipped - so
    // pressing it twice, or by accident, cannot cost anything.
    // No "reading the card..." toast. The read is ~0.1 s measured, so it is
    // gone before it can be read and the result window lands on top of it
    // anyway - two messages for one press, the first of which says nothing.
    s_sdr_state = SDR_RUNNING;
    if (!s_sdr_timer) s_sdr_timer = lv_timer_create(restore_poll_cb, 200, NULL);
    if (!psram_task_create_reapable(restore_task, "adif_sdr", 4096, NULL,
                                    tskIDLE_PRIORITY + 1, tskNO_AFFINITY)) {
        s_sdr_state = SDR_IDLE;
        msg_show("Restore failed", "The restore could not be started.", false);
    }
}


static void sel_reset(bool rerender)
{
    s_sel_row    = NULL;
    s_sel_fidx   = -1;
    s_sel_active = false;
    if (s_del_bar) lv_obj_add_flag(s_del_bar, LV_OBJ_FLAG_HIDDEN);
    if (s_list) lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    if (rerender) list_render();
}

static void row_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *row = lv_event_get_current_target(e);

    if (code == LV_EVENT_LONG_PRESSED) {
        // Enter selection mode: lock the list's own scrolling so the drag
        // moves the highlight instead of the list.
        s_sel_active = true;
        if (s_list) lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
        if (s_del_bar) lv_obj_add_flag(s_del_bar, LV_OBJ_FLAG_HIDDEN);
        sel_highlight(row);
        return;
    }
    if (code == LV_EVENT_PRESSING && s_sel_active) {
        // Move the highlight to whichever row is under the finger. Events
        // keep coming to the originally pressed row, so hit-test siblings.
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev || !s_list) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        uint32_t n = lv_obj_get_child_count(s_list);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *r = lv_obj_get_child(s_list, i);
            if (!lv_obj_has_flag(r, LV_OBJ_FLAG_CLICKABLE)) continue;  // data rows only
            lv_area_t a;
            lv_obj_get_coords(r, &a);
            if (p.y >= a.y1 && p.y <= a.y2) { sel_highlight(r); break; }
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_sel_active) return;
        s_sel_active = false;
        if (s_list) lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
        if (s_sel_row) del_bar_show();   // highlighted row stays; user decides
        return;
    }
}

static void del_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel_fidx >= 0) {
        if (!adif_log_delete_record(s_sel_fidx))
            ESP_LOGW(TAG, "delete record #%d failed", s_sel_fidx);
    }
    sel_reset(true);
}

static void del_cancel_cb(lv_event_t *e)
{
    (void)e;
    sel_reset(true);
}

// Today's UTC date in ADIF QSO_DATE format ("YYYYMMDD"). QSOs are logged
// with UTC dates, so the comparison must be UTC too - not local time.
static void today_utc(char out[9])
{
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    // strftime, not snprintf("%04d...") - GCC's -Werror=format-truncation
    // can't prove tm_year+1900 fits 4 digits and rejects the latter.
    if (strftime(out, 9, "%Y%m%d", &tm_utc) == 0) out[0] = '\0';
}

// 8 columns (Call, Country, Mode, Band, Date, Time, Sent, Rcvd), each a
// weighted flex-grow share of the row's width rather than a fixed px value -
// proportional fonts mean padding a single string with spaces (the old
// approach) never actually lines columns up. Weights (see COL_GROW_* below)
// give Call/Country more room than the narrow fixed-format columns.
#define COL_GAP  10

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    sel_reset(false);   // drop any pending delete selection with the modal
    kb_hide();          // and never leave the keyboard up over the screen behind
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

// One flex-row container per table row (header or data) - children are
// individually-width-set labels, which is what actually makes columns line
// up with a proportional font (string padding with spaces does not).
static lv_obj_t *make_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, COL_GAP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

// flex_grow + width=0 splits the row's width by weight instead of a fixed
// px value - still resizes cleanly if the panel width ever changes again,
// but lets wide-content columns (Call, Country) claim more of it than
// narrow ones (Mode/Band/Date/Time/Sent/Rcvd, all <=5 chars of content).
// Weights mirror typical content length, not character-count exactly.
#define COL_GROW_CALL  2
#define COL_GROW_CTRY  3
#define COL_GROW_NARROW 1
// "US-1241" / "G/LD-049" - wider than a report, narrower than a country.
#define COL_GROW_REF   2

static void add_col(lv_obj_t *row, const char *text, int grow,
                     const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);   // ellipsize, never wrap a column
    lv_obj_set_width(lbl, 0);
    lv_obj_set_flex_grow(lbl, grow);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
}

static void add_header_row(lv_obj_t *parent)
{
    lv_obj_t *row = make_row(parent);
    lv_obj_set_style_pad_bottom(row, 6, 0);
    add_col(row, "Call",    COL_GROW_CALL,   &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Country", COL_GROW_CTRY,   &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Mode",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Band",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Date",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Time",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Sent",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    add_col(row, "Rcvd",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    // Their Maidenhead grid. Same reason the Ref column was added: the search
    // matches it, so it has to be visible - a column you can search and cannot
    // see is a promise the screen does not keep.
    add_col(row, "Grid",    COL_GROW_NARROW, &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
    // The park or summit THEY were activating (ADIF SIG_INFO). Added because the
    // search box offered to find it while the list never showed it - a column you
    // can search and cannot see is a promise the screen does not keep.
    add_col(row, "Ref",     COL_GROW_REF,    &lv_font_montserrat_20, UI_COLOR_TEXT_MUTED);
}

// Build one QSO's row from a raw ADIF record line - callsign, DXCC country
// (looked up from the callsign, not stored in the ADIF file), mode (FT8/FT4),
// band, date, time, and the sent/received signal reports as two separate
// columns. even_row alternates the row background (zebra striping) so long
// lists stay easy to read across.
static void build_qso_row(lv_obj_t *parent, const char *line, bool even_row,
                          int file_idx)
{
    // Reports show "-" when the record has no RST field: an FT8 exchange that
    // never carried a numeric report writes none at all (see adif_log.c), and
    // showing a number the log doesn't contain would just recreate the "am I
    // really being sent 599?" confusion in the viewer instead of the file.
    char call[20] = "?", date[9] = "", time_on[7] = "", band[8] = "",
         mode[8] = "FT8", rst_sent[8] = "-", rst_rcvd[8] = "-";

    adif_log_extract_field(line, "CALL",     call,     sizeof(call));
    adif_log_extract_field(line, "QSO_DATE", date,     sizeof(date));
    adif_log_extract_field(line, "TIME_ON",  time_on,  sizeof(time_on));
    adif_log_extract_field(line, "BAND",     band,     sizeof(band));
    adif_log_extract_field(line, "MODE",     mode,     sizeof(mode));
    // FT4 is stored the way ADIF requires - MODE=MFSK with SUBMODE=FT4 - so the
    // submode is what the operator recognises and the column must show. "MFSK"
    // alone would name a family of modes rather than the one worked.
    {
        char submode[8] = "";
        if (adif_log_extract_field(line, "SUBMODE", submode, sizeof(submode)) && submode[0])
            snprintf(mode, sizeof(mode), "%s", submode);
    }
    adif_log_extract_field(line, "RST_SENT", rst_sent, sizeof(rst_sent));
    adif_log_extract_field(line, "RST_RCVD", rst_rcvd, sizeof(rst_rcvd));
    char sig_info[20] = "";
    adif_log_extract_field(line, "SIG_INFO", sig_info, sizeof(sig_info));
    char grid[12] = "";
    adif_log_extract_field(line, "GRIDSQUARE", grid, sizeof(grid));

    // date is "YYYYMMDD", time_on is "HHMMSS" (or "HHMM") - slice down to
    // "MM-DD" / "HH:MM" for a compact column.
    char mmdd[6] = "--/--", hhmm[6] = "--:--";
    if (strlen(date) >= 8) snprintf(mmdd, sizeof(mmdd), "%.2s-%.2s", date + 4, date + 6);
    if (strlen(time_on) >= 4) snprintf(hhmm, sizeof(hhmm), "%.2s:%.2s", time_on, time_on + 2);

    const char *country = dxcc_lookup(call);

    lv_obj_t *row = make_row(parent);
    if (even_row) {
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
        lv_obj_set_style_pad_top(row, 4, 0);
        lv_obj_set_style_pad_bottom(row, 4, 0);
    }
    // Single-record delete: long-press selects, drag moves, release confirms.
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void *)(intptr_t)file_idx);
    lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_PRESS_LOST, NULL);
    add_col(row, call,                    COL_GROW_CALL,   &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, country ? country : "-", COL_GROW_CTRY,   &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, mode,                    COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, band[0] ? band : "--",   COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, mmdd,                    COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, hhmm,                    COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, rst_sent,                COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    add_col(row, rst_rcvd,                COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    // Legitimately empty on plenty of contacts - a station answering with a
    // report rather than a grid never sends one - so "-" like the reports.
    add_col(row, grid[0] ? grid : "-",    COL_GROW_NARROW, &lv_font_montserrat_24, UI_COLOR_TEXT);
    // Most QSOs are not park-to-park, so an empty cell is the normal case and
    // gets the same "-" the reports use rather than a blank that reads as a
    // rendering fault.
    add_col(row, sig_info[0] ? sig_info : "-",
                                          COL_GROW_REF,    &lv_font_montserrat_24, UI_COLOR_TEXT);
}

// Rebuild the list from the live ADIF log, newest record first, honouring
// the Today/All filter. Also refreshes the title counts and the toggle
// button's label. The button reads as an ACTION, not a state: it shows the
// view you'll switch TO by pressing it (operator feedback 2026-07-16 -
// "you press what you get"); the title text carries the current state.
// Does one raw ADIF record match the search box?
//
// Matched against the fields that IDENTIFY a contact - call, mode, band, date,
// grid, reference - rather than the whole line, so a stray digit inside a
// report or a timestamp cannot produce a hit the operator cannot explain.
// Space-separated terms must ALL match, which is what makes "ha3 20m" mean what
// it looks like.
static bool record_matches_query(const char *raw)
{
    if (!s_query[0]) return true;

    char hay[160];
    int  n = 0;
    static const char *FIELDS[] = { "CALL", "SUBMODE", "MODE", "BAND",
                                    "QSO_DATE", "GRIDSQUARE", "SIG_INFO", "SIG" };
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        char v[24];
        if (!adif_log_extract_field(raw, FIELDS[i], v, sizeof(v))) continue;
        for (char *c = v; *c && n < (int)sizeof(hay) - 2; c++)
            hay[n++] = (char)toupper((unsigned char)*c);
        if (n < (int)sizeof(hay) - 1) hay[n++] = ' ';
    }

    // The country too, even though no ADIF field carries it.
    //
    // It is DERIVED from the callsign at render time, which is why the first
    // version could not search it - and it is the one column sitting on screen
    // that the box appeared to ignore, so searching "Spain" found nothing while
    // the word was right there in the list. Same dxcc_lookup() the row itself
    // uses, so what is displayed and what is searched cannot disagree.
    {
        char call[24];
        if (adif_log_extract_field(raw, "CALL", call, sizeof(call))) {
            const char *country = dxcc_lookup(call);
            if (country) {
                for (const char *c = country; *c && n < (int)sizeof(hay) - 2; c++)
                    hay[n++] = (char)toupper((unsigned char)*c);
                if (n < (int)sizeof(hay) - 1) hay[n++] = ' ';
            }
        }
    }
    hay[n] = '\0';

    // Every term must appear somewhere in the record.
    const char *t = s_query;
    while (*t) {
        while (*t == ' ') t++;
        if (!*t) break;
        const char *e = t;
        while (*e && *e != ' ') e++;
        size_t len = (size_t)(e - t);
        char term[24];
        if (len >= sizeof(term)) len = sizeof(term) - 1;
        memcpy(term, t, len);
        term[len] = '\0';
        if (!strstr(hay, term)) return false;
        t = e;
    }
    return true;
}

static void list_render(void)
{
    int64_t t_start = esp_timer_get_time();

    int total = adif_log_count();
    char today[9];
    today_utc(today);

    if (s_lbl_filter) lv_label_set_text(s_lbl_filter, s_today_only ? "All" : "Today");
    if (!s_list) return;
    // The clean below destroys any selected row object - drop the reference
    // (the confirm bar, if open, keys off the file index, which stays valid
    // until an actual delete re-renders through sel_reset()).
    s_sel_row    = NULL;
    s_sel_active = false;
    lv_obj_clean(s_list);

    if (total == 0) {
        s_last_total = 0;
        s_last_today = 0;
        s_test_count = 0;
        if (s_btn_del_test) lv_obj_add_flag(s_btn_del_test, LV_OBJ_FLAG_HIDDEN);
        s_del_all_arm_us = 0;
        if (s_btn_del_all) lv_obj_add_flag(s_btn_del_all, LV_OBJ_FLAG_HIDDEN);
        if (s_title) {
            lv_label_set_text(s_title, "ADIF Log - 0 QSOs");
            lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COLOR_TEXT), 0);
        }
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "No QSOs logged yet");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        return;
    }

    // Ring buffer holding the newest ring_cap filter-matching records - the
    // filter means we can't know which file offsets we'll display until the
    // whole file is scanned, so keep overwriting the oldest slot and the last
    // ring_cap matches survive. Still one fopen/fgets pass (the single-pass
    // O(N) shape that replaced the old O(N^2) per-row re-scan - keep it that
    // way). PSRAM: 200 rows * 1024 B = 200 KB max, trivial.
    int ring_cap = (total < ADIF_VIEW_MAX_ROWS) ? total : ADIF_VIEW_MAX_ROWS;
    char *lines = heap_caps_malloc((size_t)ring_cap * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int  *fidxs = heap_caps_malloc((size_t)ring_cap * sizeof(int), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lines || !fidxs) {
        ESP_LOGE(TAG, "OOM allocating %d-row ADIF buffer", ring_cap);
        if (lines) heap_caps_free(lines);
        if (fidxs) heap_caps_free(fidxs);
        return;
    }

    int64_t t_read_start = esp_timer_get_time();
    FILE *f = fopen(adif_log_file_path(), "r");
    int matched = 0;      // records passing the current filter (all, when All)
    int today_count = 0;  // records dated today UTC, counted regardless of filter
    if (f) {
        char raw[1024];
        bool header_skipped = false;
        int  rec = 0;   // 0-based file record index (counts ALL records)
        s_test_count = 0;
        while (fgets(raw, sizeof(raw), f)) {
            if (!header_skipped) { header_skipped = true; continue; }   // <EOH> line
            int this_rec = rec++;
            char date[9] = "";
            adif_log_extract_field(raw, "QSO_DATE", date, sizeof(date));
            bool is_today = (strcmp(date, today) == 0);
            if (is_today) today_count++;
            // Simulation-mode records (FREQ 0) - counted regardless of the
            // Today/All filter, drives the "Delete test QSOs" button.
            char freq_s[16] = "";
            if (adif_log_extract_field(raw, "FREQ", freq_s, sizeof(freq_s)) &&
                atof(freq_s) < 0.001) s_test_count++;
            if (s_today_only && !is_today) continue;
            if (!record_matches_query(raw)) continue;
            char *slot = lines + (size_t)(matched % ring_cap) * 1024;
            strncpy(slot, raw, 1023);
            slot[1023] = '\0';
            fidxs[matched % ring_cap] = this_rec;   // file index rides along
            matched++;
        }
        fclose(f);
    } else {
        ESP_LOGW(TAG, "open %s failed", adif_log_file_path());
    }
    int64_t t_read_done = esp_timer_get_time();

    s_last_total = total;
    s_last_today = today_count;

    if (s_title) {
        char t[64];
        if (s_today_only) snprintf(t, sizeof(t), "ADIF Log - Today: %d  (%d total)", today_count, total);
        else              snprintf(t, sizeof(t), "ADIF Log - %d QSOs  (%d today)", total, today_count);
        lv_label_set_text(s_title, t);
        // Park is open: 10+ QSOs today = a valid POTA activation.
        lv_obj_set_style_text_color(s_title,
            lv_color_hex(today_count >= POTA_ACTIVATION_QSOS ? COLOR_ACTIVATION_OK : UI_COLOR_TEXT), 0);
    }

    // "Delete all" exists whenever the log has records; disarm on re-render
    // so a stale "Sure?" never survives a filter toggle or a single delete.
    if (s_btn_del_all) {
        s_del_all_arm_us = 0;
        if (s_lbl_del_all) lv_label_set_text(s_lbl_del_all, "Delete all");
        lv_obj_clear_flag(s_btn_del_all, LV_OBJ_FLAG_HIDDEN);
    }

    // "Delete test QSOs" only exists while sim-mode records are in the log -
    // operators who never simulate never see it.
    if (s_btn_del_test) {
        s_del_test_arm_us = 0;
        if (s_test_count > 0) {
            lv_obj_clear_flag(s_btn_del_test, LV_OBJ_FLAG_HIDDEN);
            if (s_lbl_del_test)
                lv_label_set_text_fmt(s_lbl_del_test, "Del %d test", s_test_count);
        } else {
            lv_obj_add_flag(s_btn_del_test, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (matched == 0) {
        lv_obj_t *lbl = lv_label_create(s_list);
        // A search that finds nothing is an ANSWER, not an empty screen - it is
        // the whole reason the box is here, so say what it means.
        if (s_query[0])
            lv_label_set_text_fmt(lbl, "Nothing matches \"%s\"%s\n- so this one has not been worked.",
                                  s_query, s_today_only ? " today" : "");
        else
            lv_label_set_text(lbl, s_today_only ? "No QSOs today yet" : "No QSOs logged");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        heap_caps_free(lines);
        heap_caps_free(fidxs);
        return;
    }

    // Newest-first display: the newest match sits at (matched-1) % ring_cap,
    // walk backward from there.
    int shown = (matched < ring_cap) ? matched : ring_cap;
    int built = 0;
    size_t lv_free_start = lvgl_free_bytes();   // LVGL pool headroom before rows
    for (int k = 0; k < shown; k++) {
        // Budget guard (see ADIF_VIEW_LVGL_RESERVE): stop before the LVGL object
        // pool runs low. Checked BEFORE building, since a row is 9 objects plus
        // layout work and LVGL faults instead of failing gracefully. Newest-
        // first, so what we drop is the oldest QSOs (still logged / downloadable).
        if (lvgl_free_bytes() < ADIF_VIEW_LVGL_RESERVE) {
            ESP_LOGW(TAG, "LVGL pool low (%u B free) - stopping ADIF render at %d of %d rows",
                     (unsigned)lvgl_free_bytes(), built, shown);
            break;
        }
        int idx = (matched - 1 - k) % ring_cap;
        bool even_row = (k % 2) == 1;
        build_qso_row(s_list, lines + (size_t)idx * 1024, even_row, fidxs[idx]);
        built++;
    }
    heap_caps_free(lines);
    heap_caps_free(fidxs);

    // Couldn't show them all (memory budget or the ring cap) - say so plainly.
    // The full log is always available via the web ADIF download; this on-device
    // viewer is just a quick "did I work them / how close to 10" check.
    if (built < matched) {
        lv_obj_t *more = lv_label_create(s_list);
        lv_label_set_text_fmt(more,
            "Showing newest %d of %d - download the ADIF (web UI) for the full log",
            built, matched);
        lv_obj_set_style_text_font(more, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(more, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_style_pad_top(more, 8, 0);
        lv_obj_set_style_pad_bottom(more, 8, 0);
    }

    int64_t t_done = esp_timer_get_time();
    ESP_LOGI(TAG, "ADIF viewer: showing %d of %d logged QSOs (filter=%s today=%d, LVGL pool %uKB->%uKB free, read=%lld ms, rows=%lld ms, total=%lld ms)",
             built, total, s_today_only ? "today" : "all", today_count,
             (unsigned)(lv_free_start / 1024), (unsigned)(lvgl_free_bytes() / 1024),
             (long long)((t_read_done - t_read_start) / 1000),
             (long long)((t_done - t_read_done) / 1000),
             (long long)((t_done - t_start) / 1000));
}

// Delete every FREQ==0 (simulation-mode) record. Two-tap confirm: the first
// tap arms the button ("Sure?"), a second tap within 5 s deletes. Indices are
// collected fresh at delete time (the list may have changed since render) and
// removed HIGHEST-FIRST, since adif_log_delete_record() shifts every record
// after the deleted one down by one slot.
static void del_test_btn_cb(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_del_test_arm_us == 0 || (now - s_del_test_arm_us) > 5000000) {
        s_del_test_arm_us = now;
        if (s_lbl_del_test) lv_label_set_text(s_lbl_del_test, "Sure?");
        return;
    }
    s_del_test_arm_us = 0;

    // Collect the file indices of all FREQ==0 records.
    int cap = adif_log_count();
    int *idxs = cap > 0 ? heap_caps_malloc((size_t)cap * sizeof(int),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    int n_test = 0;
    if (idxs) {
        FILE *f = fopen(adif_log_file_path(), "r");
        if (f) {
            char raw[1024];
            bool header_skipped = false;
            int  rec = 0;
            while (fgets(raw, sizeof(raw), f) && n_test < cap) {
                if (!header_skipped) { header_skipped = true; continue; }
                int this_rec = rec++;
                char freq_s[16] = "";
                if (adif_log_extract_field(raw, "FREQ", freq_s, sizeof(freq_s)) &&
                    atof(freq_s) < 0.001) idxs[n_test++] = this_rec;
            }
            fclose(f);
        }
    }

    int deleted = 0;
    for (int i = n_test - 1; i >= 0; i--) {           // highest-first
        if (adif_log_delete_record(idxs[i])) deleted++;
    }
    if (idxs) heap_caps_free(idxs);

    ESP_LOGI(TAG, "deleted %d test (sim-mode, FREQ=0) QSO record(s)", deleted);
    char msg[96];
    snprintf(msg, sizeof(msg), "%d test contact%s removed from the log.",
             deleted, deleted == 1 ? "" : "s");
    msg_show("Deleted", msg, true);
    sel_reset(true);   // clears any row selection + re-renders (button hides itself)
}

// Delete EVERY record. Two-tap confirm like the test-delete, but the armed
// label carries the count and the word "ALL" - this one has no undo and no
// scope limit. adif_log_clear() also resets the QRZ/eQSL/LoTW upload cursors
// (see adif_log.c), so QSOs logged after the clear upload normally.
static void del_all_btn_cb(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_del_all_arm_us == 0 || (now - s_del_all_arm_us) > 5000000) {
        s_del_all_arm_us = now;
        if (s_lbl_del_all)
            lv_label_set_text_fmt(s_lbl_del_all, "ALL %d?", adif_log_count());
        return;
    }
    s_del_all_arm_us = 0;

    int n = adif_log_count();
    adif_log_clear();
    ESP_LOGI(TAG, "deleted ALL %d QSO record(s) (operator, ADIF viewer)", n);
    char msg[128];
    // The one message here that must not be missable: it cannot be undone, and
    // the count is the only record of what was lost.
    snprintf(msg, sizeof(msg), "All %d contact%s deleted.\n\nThis cannot be undone.",
             n, n == 1 ? "" : "s");
    msg_show("Log cleared", msg, false);
    sel_reset(true);   // clears any row selection + re-renders (button hides itself)
}

// Give the list back its height, and the panel its place.
static void kb_hide(void)
{
    if (s_search_kb) lv_obj_add_flag(s_search_kb, LV_OBJ_FLAG_HIDDEN);
    if (s_list)  lv_obj_set_style_max_height(s_list,  ADIF_LIST_MAX_H, 0);
    if (s_panel) {
        lv_obj_set_style_max_height(s_panel, ADIF_PANEL_MAX_H, 0);
        lv_obj_set_align(s_panel, LV_ALIGN_CENTER);
    }
}

// ⛔ The keyboard must not cover the rows. A 280 px keyboard over a centred
// 690 px panel on a 720 px screen would bury the list, the buttons, and the
// matches - so the operator would be typing a search blind, which is the one
// thing this box exists to avoid. The panel moves to the TOP and the list cap
// drops to ~3 rows, which is enough to answer "have I worked this one" while
// still showing the field being typed into.
static void kb_show(void)
{
    if (!s_search_kb) return;
    lv_keyboard_set_textarea(s_search_kb, s_search_ta);
    // ui_osk_show(), not a bare flag clear: it declines to show the on-screen
    // keyboard when a Bluetooth one is connected and already doing the job
    // (#273), and its own comment asks callers to keep that decision in one
    // place. Clearing the flag by hand would put half a screen of keys in front
    // of an operator who is typing on a real keyboard.
    ui_osk_show(s_search_kb);

    // ...so only give up the list's height if a keyboard actually appeared.
    // With a Bluetooth keyboard there is nothing covering the rows and no
    // reason to shrink anything.
    if (lv_obj_has_flag(s_search_kb, LV_OBJ_FLAG_HIDDEN)) return;
    if (s_list)  lv_obj_set_style_max_height(s_list,  ADIF_LIST_MAX_H_KB, 0);
    if (s_panel) {
        lv_obj_set_style_max_height(s_panel, ADIF_PANEL_MAX_H_KB, 0);
        lv_obj_set_align(s_panel, LV_ALIGN_TOP_MID);
    }
}

// Re-filter on every keystroke. The whole log is re-read per render, which is
// the same work the Today/All toggle already does and is measured in single-
// digit milliseconds for a few hundred records - and typing is slow.
static void search_ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) { kb_show(); return; }
    if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY) { kb_hide(); return; }
    if (code != LV_EVENT_VALUE_CHANGED) return;

    const char *txt = lv_textarea_get_text(s_search_ta);
    size_t i = 0;
    for (; txt && txt[i] && i < sizeof(s_query) - 1; i++)
        s_query[i] = (char)toupper((unsigned char)txt[i]);
    s_query[i] = '\0';
    list_render();
}

static void filter_btn_cb(lv_event_t *e)
{
    (void)e;
    s_today_only = !s_today_only;
    list_render();
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
    // 900 until the Ref column arrived. The display is 1280 wide, so this keeps a
    // 50 px margin each side and gives the new column its room without squeezing
    // Country, which is the widest text in the table.
    lv_obj_set_width(s_panel, 1180);
    lv_obj_set_height(s_panel, LV_SIZE_CONTENT);
    // Chrome (title header + search row + column header + bottom buttons +
    // paddings) is ~290 px; plus ADIF_LIST_MAX_H that leaves ~660 and keeps the
    // buttons on-screen (the panel is centred in a 720 px-tall display).
    // ⛔ These three numbers are ONE budget: ADIF_LIST_MAX_H, this cap, and
    // whatever rows the panel holds. Adding a row without taking it out of the
    // list pushes the buttons off the bottom - which is exactly what the search
    // row did. If any of them changes, re-measure on the device.
    lv_obj_set_style_max_height(s_panel, ADIF_PANEL_MAX_H, 0);
    lv_obj_set_align(s_panel, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 16, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 12, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Title + Today/All toggle share one horizontal header strip so the
    // toggle doesn't cost the list any vertical space.
    lv_obj_t *hdr = lv_obj_create(s_panel);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(hdr);
    lv_label_set_text(s_title, "ADIF Log");
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);

    // "Delete test QSOs" - only shown while sim-mode (FREQ 0) records exist;
    // list_render() manages visibility + the count in the label. Sits between
    // the title and the Today/All toggle in the same header strip.
    s_btn_del_test = lv_btn_create(hdr);
    lv_obj_set_size(s_btn_del_test, 190, 56);
    lv_obj_set_style_bg_color(s_btn_del_test, lv_color_hex(0x7a4a10), 0);   // muted amber: caution
    lv_obj_set_style_border_color(s_btn_del_test, lv_color_hex(0xb07020), 0);
    lv_obj_set_style_border_width(s_btn_del_test, 2, 0);
    lv_obj_set_style_radius(s_btn_del_test, 8, 0);
    lv_obj_add_flag(s_btn_del_test, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_btn_del_test, del_test_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_del_test = lv_label_create(s_btn_del_test);
    lv_label_set_text(s_lbl_del_test, "Del test");
    lv_obj_set_style_text_color(s_lbl_del_test, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_del_test, &lv_font_montserrat_24, 0);
    lv_obj_center(s_lbl_del_test);

    // Today/All filter toggle - the label is the ACTION (the view pressing
    // it switches to); the title above shows the current view.
    lv_obj_t *btn_filter = lv_btn_create(hdr);
    lv_obj_set_size(btn_filter, 160, 56);
    lv_obj_set_style_bg_color(btn_filter, lv_color_hex(0x2a2f37), 0);
    lv_obj_set_style_border_color(btn_filter, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(btn_filter, 2, 0);
    lv_obj_set_style_radius(btn_filter, 8, 0);
    lv_obj_add_event_cb(btn_filter, filter_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_filter = lv_label_create(btn_filter);
    lv_label_set_text(s_lbl_filter, "All");   // action label; list_render() keeps it current
    lv_obj_set_style_text_color(s_lbl_filter, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_filter, &lv_font_montserrat_24, 0);
    lv_obj_center(s_lbl_filter);

    // Search gets its OWN row under the header, full width.
    //
    // It was first put in the header strip beside the title, to cost the list no
    // height - and that squeezed the title into it ("ADIF Log - 25 QSOs (0 toda"
    // running straight into the box). The header already carries a title whose
    // width changes with the counts, plus up to two buttons; there is no room
    // for a third flexible item, and a row of its own is ~56 px the list can
    // spare.
    s_search_ta = lv_textarea_create(s_panel);
    lv_textarea_set_one_line(s_search_ta, true);
    // Names what it searches in the operator's words. "reference" was ours -
    // it means the POTA park or SOTA summit, and nobody had to guess that.
    lv_textarea_set_placeholder_text(s_search_ta,
        "Search a callsign, country, band, mode, date, grid or park/summit reference");
    lv_textarea_set_max_length(s_search_ta, sizeof(s_query) - 1);
    lv_obj_set_width(s_search_ta, LV_PCT(100));
    lv_obj_set_style_text_font(s_search_ta, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_search_ta);
    lv_obj_add_event_cb(s_search_ta, search_ta_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_search_ta, search_ta_cb, LV_EVENT_FOCUSED,       NULL);
    lv_obj_add_event_cb(s_search_ta, search_ta_cb, LV_EVENT_DEFOCUSED,     NULL);
    lv_obj_add_event_cb(s_search_ta, search_ta_cb, LV_EVENT_READY,         NULL);

    // Header row sits outside the scrollable list (a sibling, not a child) so
    // column titles stay pinned in place while the QSO rows scroll under it.
    add_header_row(s_panel);

    // Plain flex-column container, not lv_list - lv_list_add_button() forces
    // a single icon+label per row, which is exactly what items #1/#2 of this
    // change removed (no per-row layout control, and a checkmark icon that
    // implied an action when this view is read-only).
    s_list = lv_obj_create(s_panel);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    // ~11 rows visible, scroll for the rest. Deliberately bounded (not "as tall
    // as it can be") so the panel - header + this list + the Close button - fits
    // on-screen with Close fully visible. Must stay in step with the panel's
    // max_height below (chrome ~230 px + this = the panel height).
    lv_obj_set_style_max_height(s_list, 430, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    // Bottom strip: "Delete all" (left) + Close (right). Deliberately NOT in
    // the top header - the header is where the harmless Today/All toggle
    // lives, and the panel's width can't fit a fourth button beside a long
    // title anyway. Being across the panel from everything tapped routinely
    // is part of the confirm gesture.
    lv_obj_t *bot = lv_obj_create(s_panel);
    lv_obj_set_width(bot, LV_PCT(100));
    lv_obj_set_height(bot, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 0, 0);
    lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

    s_btn_del_all = lv_btn_create(bot);
    lv_obj_set_size(s_btn_del_all, 240, 72);
    lv_obj_set_style_bg_color(s_btn_del_all, lv_color_hex(0x5a1f1f), 0);   // dark red: danger
    lv_obj_set_style_border_color(s_btn_del_all, lv_color_hex(0xff5050), 0);
    lv_obj_set_style_border_width(s_btn_del_all, 2, 0);
    lv_obj_set_style_radius(s_btn_del_all, 8, 0);
    lv_obj_add_flag(s_btn_del_all, LV_OBJ_FLAG_HIDDEN);   // list_render() shows it with records
    lv_obj_add_event_cb(s_btn_del_all, del_all_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_del_all = lv_label_create(s_btn_del_all);
    lv_label_set_text(s_lbl_del_all, "Delete all");
    lv_obj_set_style_text_color(s_lbl_del_all, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_del_all, &lv_font_montserrat_24, 0);
    lv_obj_center(s_lbl_del_all);

    // "Restore from SD", between the two. Neutral colour on purpose: the note
    // below says red in this panel means exactly one thing, and this is the
    // opposite of that - it can only add contacts back. Shown only with a card
    // in, since with no card the button could do nothing but explain itself.
    if (sd_archive_is_mounted()) {
        lv_obj_t *sd_btn = lv_btn_create(bot);
        lv_obj_set_size(sd_btn, 260, 72);
        lv_obj_set_style_bg_color(sd_btn, lv_color_hex(0x2a3138), 0);
        lv_obj_set_style_border_color(sd_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_set_style_border_width(sd_btn, 2, 0);
        lv_obj_set_style_radius(sd_btn, 8, 0);
        lv_obj_add_event_cb(sd_btn, restore_sd_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *sd_lbl = lv_label_create(sd_btn);
        lv_label_set_text(sd_lbl, "Restore from SD");
        lv_obj_set_style_text_color(sd_lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(sd_lbl, &lv_font_montserrat_24, 0);
        lv_obj_center(sd_lbl);
    }

    // ⭐ Close is NEUTRAL here, deliberately breaking the house Cancel colour.
    //
    // 0x962020 is what Cancel uses in every other modal, and that is fine where
    // the worst outcome is "nothing happened". This is the ONE window with a
    // destructive button in it, and there the convention actively misleads:
    // Gyula HA3HZ opened the log, found Close "bright red as well as the Delete
    // all button", and hesitated. He was being more careful than the UI
    // deserved - Close was 0x962020 and Delete all 0x5a1f1f, so the SAFE button
    // was the brighter red of the two.
    //
    // In this panel red means exactly one thing: it deletes your log. Anything
    // else added here must stay off red.
    lv_obj_t *close_btn = lv_btn_create(bot);
    lv_obj_set_size(close_btn, 240, 72);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2a3138), 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_width(close_btn, 2, 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(close_lbl);
    ui_kbd_set_buttons(NULL, close_btn);   // physical keyboard Esc -> Close

    // On the modal, not the panel: it has to be able to sit BELOW the panel
    // rather than inside it. Hidden until the search field is touched.
    s_search_kb = lv_keyboard_create(s_modal);
    // ui_theme_style_keyboard() alone styles the keyboard's own background but
    // NOT its keys, so on its own it leaves LVGL's default white keys - which
    // is what shipped to the bench and looked nothing like the rest of the app.
    // Every other modal here adds this per-key style as well; same block, same
    // values, so this keyboard matches the ones beside it.
    static lv_style_t style_kb_btn;
    static bool kb_btn_style_inited = false;
    if (!kb_btn_style_inited) {
        lv_style_init(&style_kb_btn);
        lv_style_set_bg_color(&style_kb_btn, lv_color_hex(UI_COLOR_KEY_BG));
        lv_style_set_bg_opa(&style_kb_btn, LV_OPA_COVER);
        lv_style_set_text_color(&style_kb_btn, lv_color_white());
        lv_style_set_border_width(&style_kb_btn, 1);
        lv_style_set_border_color(&style_kb_btn, lv_color_hex(0x505050));
        kb_btn_style_inited = true;
    }
    lv_obj_add_style(s_search_kb, &style_kb_btn, LV_PART_ITEMS);
    ui_theme_style_keyboard(s_search_kb);
    lv_obj_set_size(s_search_kb, LV_PCT(100), ADIF_KB_H);
    lv_obj_align(s_search_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_search_kb, LV_KEYBOARD_MODE_TEXT_UPPER);   // callsigns
    ui_theme_keyboard_attach_caps_cycle_upper(s_search_kb);
    // The key LABELS. Missing this is what still made it look like a different
    // keyboard after the per-key colours were fixed - the letters came out at
    // LVGL's default size against montserrat_28 everywhere else, and that is
    // the difference the eye actually catches.
    lv_obj_set_style_text_font(s_search_kb, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_search_kb, LV_OBJ_FLAG_HIDDEN);

    // Single-record delete confirm bar: floating overlay across the bottom of
    // the panel (FLOATING = ignored by the flex layout, so nothing reflows),
    // hidden until a row is long-press-selected and released.
    s_del_bar = lv_obj_create(s_panel);
    lv_obj_add_flag(s_del_bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_del_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_del_bar, LV_PCT(96), 92);
    lv_obj_align(s_del_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(s_del_bar, lv_color_hex(0x30181a), 0);
    lv_obj_set_style_bg_opa(s_del_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_del_bar, lv_color_hex(0xff5050), 0);
    lv_obj_set_style_border_width(s_del_bar, 2, 0);
    lv_obj_set_style_radius(s_del_bar, 8, 0);
    lv_obj_set_style_pad_all(s_del_bar, 10, 0);
    lv_obj_clear_flag(s_del_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_del_lbl = lv_label_create(s_del_bar);
    lv_label_set_text(s_del_lbl, "");
    lv_obj_set_style_text_font(s_del_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_del_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_align(s_del_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *b_cancel = lv_btn_create(s_del_bar);
    lv_obj_set_size(b_cancel, 170, 60);
    lv_obj_align(b_cancel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(b_cancel, lv_color_hex(0x2a2f37), 0);
    lv_obj_set_style_border_color(b_cancel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(b_cancel, 2, 0);
    lv_obj_set_style_radius(b_cancel, 8, 0);
    lv_obj_add_event_cb(b_cancel, del_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(b_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_set_style_text_color(lc, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24, 0);
    lv_obj_center(lc);

    lv_obj_t *b_del = lv_btn_create(s_del_bar);
    lv_obj_set_size(b_del, 170, 60);
    lv_obj_align(b_del, LV_ALIGN_RIGHT_MID, -186, 0);
    lv_obj_set_style_bg_color(b_del, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(b_del, 8, 0);
    lv_obj_set_style_border_width(b_del, 0, 0);
    lv_obj_add_event_cb(b_del, del_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ld = lv_label_create(b_del);
    lv_label_set_text(ld, "Delete");
    lv_obj_set_style_text_color(ld, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ld, &lv_font_montserrat_24, 0);
    lv_obj_center(ld);
}

void adif_view_modal_init(void)
{
    modal_build();
}

void adif_view_modal_show(void)
{
    modal_build();
    // A fresh open starts from no search: a query left over from last time
    // would silently hide records, and the box holding the reason for it is
    // easy to miss beside the title.
    s_query[0] = 0;
    if (s_search_ta) lv_textarea_set_text(s_search_ta, "");
    kb_hide();
    // Open on the Today view (the POTA-activation use case) - but fall back
    // to All when today is empty and older QSOs exist, so reviewing the log
    // at home doesn't open onto a blank list. An unsynced clock degrades the
    // same way: bogus "today" matches nothing -> All. A manual toggle back
    // to an empty Today is respected (this fallback runs only on open).
    s_today_only = true;
    list_render();
    if (s_last_today == 0 && s_last_total > 0) {
        s_today_only = false;
        list_render();
    }
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
}
