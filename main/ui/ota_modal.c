// See ota_modal.h.

#include "ota_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "display.h"
#include "net/ota_update.h"
#include "net/update_check.h"
#include "util/status.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota_modal";

static lv_obj_t   *s_modal   = NULL;
static lv_obj_t   *s_panel   = NULL;
static lv_obj_t   *s_title   = NULL;
static lv_obj_t   *s_body    = NULL;
static lv_obj_t   *s_bar     = NULL;   // progress, shown only while downloading
static lv_obj_t   *s_ok_btn  = NULL;
static lv_obj_t   *s_ok_lbl  = NULL;
static lv_obj_t   *s_no_btn  = NULL;
static lv_obj_t   *s_no_lbl  = NULL;
static lv_timer_t *s_timer   = NULL;
static bool        s_open    = false;

// What the primary button will do when pressed. Derived from the live OTA
// state on every refresh rather than remembered, so a download that finishes
// while the window is open turns "Downloading..." into "Restart now" by
// itself - the operator does not have to close and reopen to find the button.
typedef enum {
    ACT_NONE = 0,    // nothing to do (mid-download); primary hidden
    ACT_DOWNLOAD,
    ACT_RESTART,
    ACT_RETRY,
    ACT_CHECK,
} ota_action_t;

static ota_action_t s_action = ACT_NONE;

static void close_modal(void)
{
    s_open = false;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    ui_kbd_set_buttons(NULL, NULL);
}

static void later_btn_cb(lv_event_t *e)
{
    (void)e;
    // "Later" does NOT cancel a download in flight. This is a window, not a
    // switch; closing it would have to either abort the transfer (throwing away
    // bandwidth the operator already spent) or silently keep going, and only
    // one of those is honest. The bottom-bar line keeps saying where things
    // stand either way, and tapping it reopens this window.
    ESP_LOGI(TAG, "closed by the operator");
    status_ota_ready_ack();   // seen it - the bar can stop breathing
    close_modal();
}

static void do_restart(void)
{
    ESP_LOGW(TAG, "operator confirmed restart into the new firmware");
    if (s_title) lv_label_set_text(s_title, "Restarting");
    if (s_body)  lv_label_set_text(s_body,  "");
    if (s_ok_btn) lv_obj_add_flag(s_ok_btn, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);                   // paint it before we stop painting anything

    // GO DARK BEFORE RESTARTING. display_init() sets the backlight to 0 so an
    // uninitialised panel is never shown, but that is ~2-3 s into boot; across
    // esp_restart() the backlight simply stays on from the previous run and
    // lights up a panel with nothing in it. The operator saw that as "a clear
    // cyan screen for 2-3 seconds" and rightly called it intrusive.
    vTaskDelay(pdMS_TO_TICKS(400));
    display_set_brightness(0);
    vTaskDelay(pdMS_TO_TICKS(80));
    esp_restart();
}

static void ok_btn_cb(lv_event_t *e)
{
    (void)e;
    switch (s_action) {
    case ACT_RESTART:
        do_restart();
        break;
    case ACT_DOWNLOAD:
    case ACT_RETRY: {
        char url[192], err[96];
        update_check_get_asset_url(url, sizeof(url));
        if (!url[0]) {
            if (s_body) lv_label_set_text(s_body, "No download address for that release.");
            return;
        }
        if (!ota_update_start(url, err, sizeof(err))) {
            // The refusal reason matters more than the failure - "transmitting"
            // is something the operator can act on, so it is shown here rather
            // than only logged.
            ESP_LOGW(TAG, "update refused: %s", err);
            if (s_body) lv_label_set_text(s_body, err);
            return;
        }
        break;
    }
    case ACT_CHECK:
        update_check_now();
        if (s_body) lv_label_set_text(s_body, "Checking...");
        break;
    default:
        break;
    }
}

// Re-read the world and re-render. Runs on the modal's own timer AND on open,
// so there is exactly one place that decides what this window says.
static void refresh(void)
{
    if (!s_open || !s_modal) return;

    int  pct = 0;
    char msg[128] = {0};
    ota_state_t st = ota_update_get_state(&pct, msg, sizeof(msg));

    char latest[32] = {0};
    update_check_get_latest(latest, sizeof(latest));
    const esp_app_desc_t *app = esp_app_get_description();
    const char *running = (app && app->version[0]) ? app->version : "?";

    char title[64], body[192];
    uint32_t ok_col = UI_COLOR_PRIMARY;
    const char *ok_text = NULL;

    if (st == OTA_DONE) {
        snprintf(title, sizeof(title), "%s is ready", latest[0] ? latest : "A new version");
        snprintf(body, sizeof(body),
                 "Downloaded and verified.\nThe radio restarts to finish - about 20 seconds.");
        ok_text  = "Restart now";
        ok_col   = 0x2E7D46;                       // green: this completes it
        s_action = ACT_RESTART;
    } else if (st == OTA_RUNNING) {
        snprintf(title, sizeof(title), "Downloading  %d%%", pct);
        snprintf(body, sizeof(body),
                 "%s is downloading in the background.\nYou can close this - nothing is interrupted.",
                 latest[0] ? latest : "The update");
        s_action = ACT_NONE;
    } else if (st == OTA_FAILED) {
        snprintf(title, sizeof(title), "Download failed");
        snprintf(body, sizeof(body), "%s", msg[0] ? msg : "The download could not be completed.");
        ok_text  = "Try again";
        ok_col   = UI_COLOR_PRIMARY;
        s_action = ACT_RETRY;
    } else if (update_check_in_progress()) {
        // Held here until the answer actually lands. This branch sits ABOVE the
        // "up to date" one deliberately: that verdict is the LAST one, and
        // showing it while a check is in flight is how this window told people
        // they were current about a release that already existed.
        snprintf(title, sizeof(title), "Checking...");
        snprintf(body, sizeof(body),
                 "Asking GitHub whether anything is newer than %s.\n"
                 "This takes a few seconds.", running);
        s_action = ACT_NONE;
    } else if (update_check_available() && latest[0]) {
        snprintf(title, sizeof(title), "%s is available", latest);
        snprintf(body, sizeof(body), "You are running %s.", running);
        ok_text  = "Download now";
        ok_col   = UI_COLOR_PRIMARY;
        s_action = ACT_DOWNLOAD;
    } else {
        snprintf(title, sizeof(title), "Up to date");
        snprintf(body, sizeof(body), "You are running %s.", running);
        ok_text  = "Check now";
        ok_col   = UI_COLOR_PRIMARY;
        s_action = ACT_CHECK;
    }

    lv_label_set_text(s_title, title);
    lv_label_set_text(s_body,  body);

    if (st == OTA_RUNNING) {
        lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);   // no animation: ~13 fps, see CLAUDE.md
    } else {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }

    if (ok_text) {
        lv_label_set_text(s_ok_lbl, ok_text);
        lv_obj_set_style_bg_color(s_ok_btn, lv_color_hex(ok_col), 0);
        lv_obj_clear_flag(s_ok_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ok_btn, LV_OBJ_FLAG_HIDDEN);
    }

    // The dismiss button says what dismissing means in this state.
    lv_label_set_text(s_no_lbl, (st == OTA_RUNNING) ? "Close" : "Later");
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
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
    lv_obj_add_flag(s_modal, UI_FLAG_NOT_HOT);   // a scrim you dismiss, not a control
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    s_panel = lv_obj_create(s_modal);
    lv_obj_set_size(s_panel, 760, 400);
    // Centred, but nudged DOWN. The top-bar Band/Mode/BW/Zoom hit zones are
    // direct children of the screen and are foregrounded above every overlay -
    // the reverse-creation-order trap documented in ui.c, which already cost
    // the Reader its own header buttons. Keeping the whole panel below them
    // means nothing here can be swallowed.
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_panel);
    lv_label_set_text(s_title, "");
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_48, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 0);

    s_body = lv_label_create(s_panel);
    lv_label_set_text(s_body, "");
    lv_obj_set_style_text_color(s_body, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_body, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_body, 700);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 80);

    s_bar = lv_bar_create(s_panel);
    lv_obj_set_size(s_bar, 640, 18);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 176);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xFFC060), LV_PART_INDICATOR);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    s_ok_btn = lv_btn_create(s_panel);
    lv_obj_set_size(s_ok_btn, 360, 84);
    lv_obj_align(s_ok_btn, LV_ALIGN_BOTTOM_MID, 0, -92);
    lv_obj_set_style_radius(s_ok_btn, 8, 0);
    lv_obj_add_event_cb(s_ok_btn, ok_btn_cb, LV_EVENT_CLICKED, NULL);
    s_ok_lbl = lv_label_create(s_ok_btn);
    lv_label_set_text(s_ok_lbl, "");
    lv_obj_set_style_text_color(s_ok_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_ok_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(s_ok_lbl);

    s_no_btn = lv_btn_create(s_panel);
    lv_obj_set_size(s_no_btn, 240, 72);
    lv_obj_align(s_no_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_no_btn, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_color(s_no_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_no_btn, 2, 0);
    lv_obj_set_style_radius(s_no_btn, 8, 0);
    lv_obj_add_event_cb(s_no_btn, later_btn_cb, LV_EVENT_CLICKED, NULL);
    s_no_lbl = lv_label_create(s_no_btn);
    lv_label_set_text(s_no_lbl, "Later");
    lv_obj_set_style_text_color(s_no_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_no_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_no_lbl);

    // Enter/Esc from the snap-on keyboard, same as every other modal here.
    ui_kbd_set_buttons(s_ok_btn, s_no_btn);
}

void ota_modal_show(void)
{
    modal_build();
    if (s_open) return;
    s_open = true;
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    ui_kbd_set_buttons(s_ok_btn, s_no_btn);   // Enter = the action, Esc = Later
    refresh();
    // 500 ms: fast enough that a percentage looks live, slow enough to be
    // free. Only exists while the window is open.
    if (!s_timer) s_timer = lv_timer_create(timer_cb, 500, NULL);
    ESP_LOGI(TAG, "opened");
}

bool ota_modal_is_open(void) { return s_open; }
