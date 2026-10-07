// "Is today's date right?" - see date_confirm_modal.h for why this exists.
//
// One window, no keyboard: the date is shown large with a day either side of it
// (- day / + day), because the realistic error is "the day I last used it", a
// few days back, and two taps beat typing a date on glass at a park table. The
// only way out is to say the date is right, which is the whole point - but a
// later SNTP sync closes the AUTOMATIC question without asking, since the
// internet then knows. A window the operator opened themselves is never closed
// for them - see s_operator_opened in the .c (royord, #18).

#include "date_confirm_modal.h"
#include "ui_theme.h"
#include "time_sync/time_sync.h"

#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "date_confirm";

/* Long enough for a unit with WiFi to get SNTP first (normally ~15 s after
 * boot), so an at-home boot never sees the question at all. */
#define ASK_AFTER_UPTIME_S 60

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_lbl_date = NULL;
static lv_obj_t *s_lbl_day = NULL;
static int       s_offset_days = 0;   // what the operator has stepped to, relative to the clock
static bool      s_asked_this_boot = false;

/* ⛔ WHO OPENED IT DECIDES WHETHER IT MAY CLOSE ITSELF (royord, #18).
 *
 * The automatic nag is a question nobody asked for, so SNTP arriving answers it
 * and it goes away - that part was right. But the same self-close ran for a
 * window the OPERATOR had just opened from the Set-the-Clock date line, and it
 * does not take a slow SNTP to kill that one: with WiFi already up
 * time_sync_date_verified() is ALREADY true, so the very next 1 Hz tick shut it.
 *
 * Reported as "it flashes up the menu and then almost immediately clears it",
 * and he was right - manual date entry existed and could not be held open. His
 * repro: flash the M5 UserDemo (which leaves the RTC at a nonsense date), flash
 * this firmware, FT8 > Options > Set and Sync the Clock, open the date line.
 *
 * So the flag is about PROVENANCE, not state. Do not replace it with a test on
 * date_verified or on which screen is up - those are exactly the facts that are
 * the same in both cases. */
static bool      s_operator_opened = false;

bool date_confirm_modal_is_open(void)
{
    return s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void refresh(void)
{
    time_t t = time(NULL) + (time_t)s_offset_days * 86400;
    struct tm tm;
    gmtime_r(&t, &tm);
    static const char *const wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    lv_label_set_text_fmt(s_lbl_date, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    lv_label_set_text_fmt(s_lbl_day, "%s  (UTC)", wd[tm.tm_wday % 7]);
}

static void close_modal(void)
{
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_operator_opened = false;
}

static void minus_cb(lv_event_t *e) { (void)e; s_offset_days--; refresh(); }
static void plus_cb(lv_event_t *e)  { (void)e; s_offset_days++; refresh(); }
/* ⛔ ONE DAY AT A TIME IS NOT ENOUGH, and a real user proved it.
 *
 * This window was built for the realistic error - "the day I last used it", a
 * few days back - and ± a day is right for that. John Dusek, 2026-09-26: a
 * fresh install that had never been online showed **2023**, and stepping a day
 * at a time to reach today is roughly a thousand taps. "THAT's a problem", and
 * he is right.
 *
 * Month and year steps rather than a typed date: the window has no keyboard by
 * design (two taps beat typing a date on glass at a park table), and three
 * step sizes reach any plausible error in a handful of presses while keeping
 * that property. 30 and 365 days are deliberately approximate - this sets a
 * date the operator then CONFIRMS by reading it, so landing near and nudging
 * with ± day is the intended way to use it.
 *
 * ⚠ The real fix for John is not this window at all: the clock is a SYMPTOM of
 * never having been online, and it corrects itself from SNTP or a GPS-equipped
 * QMX the moment the network works. This only stops the manual path being
 * unusable meanwhile. */
static void minus_mon_cb(lv_event_t *e) { (void)e; s_offset_days -= 30;  refresh(); }
static void plus_mon_cb(lv_event_t *e)  { (void)e; s_offset_days += 30;  refresh(); }
static void minus_yr_cb(lv_event_t *e)  { (void)e; s_offset_days -= 365; refresh(); }
static void plus_yr_cb(lv_event_t *e)   { (void)e; s_offset_days += 365; refresh(); }

static void ok_cb(lv_event_t *e)
{
    (void)e;
    if (s_offset_days == 0) {
        time_sync_confirm_date();
    } else {
        time_t t = time(NULL) + (time_t)s_offset_days * 86400;
        struct tm tm;
        gmtime_r(&t, &tm);
        if (!time_sync_set_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday)) return;   // keep asking
    }
    close_modal();
}

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
    /* Clickable so a tap beside the panel lands here and does NOTHING: dismissing
     * by accident would leave the wrong date in the log, which is the fault. */
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_modal, swallow_cb, LV_EVENT_CLICKED, NULL);

    /* ⛔ GEOMETRY IS LOAD-BEARING HERE, and it was wrong until 2026-10-07.
     *
     * At 760x470 with pad 24 the content box is 712x422, centre y 211. The
     * "why" paragraph wrapped to two lines ending at y~105 and the year
     * buttons started at y 63 - so the explanation was printed UNDER the
     * buttons and the operator read half a sentence in muted grey. His words:
     * "the feature is shoe-horned in with weak letters".
     *
     * The rows below are laid out with explicit gaps and the arithmetic is
     * written down, because the previous version had none and nothing caught
     * the overlap. Content box is now 864 x 564, centre y 282:
     *     title   0..40      (font 32)
     *     why    48..106     (font 24, two lines)
     *     year  132..196     (centre -118, h 64)
     *     mth   205..269     (centre  -45, h 64)
     *     day   274..370     (centre  +40, h 96)
     *     date  252..312     (centre   0,  font 48 - horizontally between the
     *                         button columns, 504 px of clear space)
     *     OK    480..564     (bottom, h 84)
     * If you change a font or a height, redo the sums. */
    lv_obj_t *p = lv_obj_create(s_modal);
    lv_obj_set_size(p, 920, 620);
    lv_obj_center(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_pad_all(p, 28, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, "Is today's date right?");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *why = lv_label_create(p);
    lv_label_set_text(why, "The Tab5 was off too long to keep its clock, and there is no "
                           "internet to check the date. QSOs are logged with this date.");
    lv_label_set_long_mode(why, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(why, 800);
    lv_obj_set_style_text_align(why, LV_TEXT_ALIGN_CENTER, 0);
    /* Not UI_COLOR_TEXT_MUTED: this paragraph is the reason the window exists,
     * and muted grey on a dark panel read as decoration. */
    lv_obj_set_style_text_color(why, lv_color_hex(0xB8C4D0), 0);
    lv_obj_set_style_text_font(why, &lv_font_montserrat_24, 0);
    lv_obj_align(why, LV_ALIGN_TOP_MID, 0, 48);

    lv_obj_t *minus = make_btn(p, "- day", 0x2a3a4a, 180, 96, minus_cb);
    lv_obj_align(minus, LV_ALIGN_LEFT_MID, 0, 40);
    /* Coarse steps above the day buttons - see minus_mon_cb(). */
    lv_obj_t *minus_mo = make_btn(p, "- mth", 0x24303c, 180, 64, minus_mon_cb);
    lv_obj_align(minus_mo, LV_ALIGN_LEFT_MID, 0, -45);
    lv_obj_t *minus_yr = make_btn(p, "- year", 0x24303c, 180, 64, minus_yr_cb);
    lv_obj_align(minus_yr, LV_ALIGN_LEFT_MID, 0, -118);
    lv_obj_t *plus = make_btn(p, "+ day", 0x2a3a4a, 180, 96, plus_cb);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 40);
    lv_obj_t *plus_mo = make_btn(p, "+ mth", 0x24303c, 180, 64, plus_mon_cb);
    lv_obj_align(plus_mo, LV_ALIGN_RIGHT_MID, 0, -45);
    lv_obj_t *plus_yr = make_btn(p, "+ year", 0x24303c, 180, 64, plus_yr_cb);
    lv_obj_align(plus_yr, LV_ALIGN_RIGHT_MID, 0, -118);

    s_lbl_date = lv_label_create(p);
    lv_obj_set_style_text_color(s_lbl_date, lv_color_hex(0xFFC864), 0);
    lv_obj_set_style_text_font(s_lbl_date, &lv_font_montserrat_48, 0);
    lv_obj_align(s_lbl_date, LV_ALIGN_CENTER, 0, 0);
    s_lbl_day = lv_label_create(p);
    lv_obj_set_style_text_color(s_lbl_day, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_lbl_day, &lv_font_montserrat_28, 0);
    lv_obj_align(s_lbl_day, LV_ALIGN_CENTER, 0, 52);

    lv_obj_t *ok = make_btn(p, LV_SYMBOL_OK "  This date is right", 0x1e6028, 520, 84, ok_cb);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui_kbd_set_buttons(ok, NULL);   // Enter = confirm
}

static void show_modal(bool by_operator)
{
    build();
    s_offset_days = 0;
    refresh();
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_operator_opened = by_operator;
    /* Kept for BOTH paths, as it always was: having shown the window once is
     * what the once-per-boot nag counts, however it got on screen. */
    s_asked_this_boot = true;
    if (by_operator) {
        ESP_LOGI(TAG, "date window opened by the operator - it stays until dismissed");
    } else {
        ESP_LOGW(TAG, "date not verified - asking the operator");
    }
}

void date_confirm_modal_show(void)
{
    /* Every caller of this is an operator action - the Set-the-Clock date line
     * (ft8_time_modal.c) and the web UI. The automatic nag goes through
     * show_modal(false) from the tick below. */
    show_modal(true);
}

void date_confirm_modal_tick(void)
{
    if (date_confirm_modal_is_open()) {
        /* The internet arrived while OUR OWN question was up: it knows the date,
         * so stop asking. A window the operator opened is theirs - it closes
         * when they say the date is right, and not before. */
        if (time_sync_date_verified() && !s_operator_opened) {
            ESP_LOGI(TAG, "date verified by SNTP while asking - closing");
            close_modal();
        } else {
            refresh();   // keeps the shown day right across midnight
        }
        return;
    }
    if (s_asked_this_boot || time_sync_date_verified()) return;
    if (esp_timer_get_time() < (int64_t)ASK_AFTER_UPTIME_S * 1000000) return;
    if (time(NULL) < 1700000000) return;   // no date at all yet - nothing to confirm
    show_modal(false);
}
