// Antenna Tune modal - full-screen overlay for QMX SWR Tune mode (MD8;,
// 1_04+ firmware only). Moved out of the settings drawer (2026-07-04) into
// its own window: a big live SWR/power readout, the Start/Stop Tune toggle,
// and a red Cancel that also safely exits an active tune before closing -
// this keys the radio for real, so it gets the same "can't dismiss without
// stopping the transmission" treatment as the rest of this file's safety
// logic. See docs/qmx-1_04-cat-comparison.md for the CAT-level design.

#include "tune_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "cat.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tune_modal";

#define TUNE_TIMEOUT_MS 60000  // auto-exit if nobody stops Tune manually - don't leave the radio keyed indefinitely

static lv_obj_t   *s_modal      = NULL;
static lv_obj_t   *s_panel      = NULL;
static lv_obj_t   *s_status_lbl = NULL;   // big live SWR/power readout
static lv_obj_t   *s_action_btn = NULL;
static lv_obj_t   *s_action_lbl = NULL;
static lv_timer_t *s_timer      = NULL;   // live SWR/timeout poll while active
static bool        s_active     = false; // ground truth for "are we in a Tune session" -
                                          // NOT derived from the QMX's own MD; echo. Field-
                                          // tested 2026-07-03: the QMX kept echoing the
                                          // PRE-Tune mode digit via MD; the entire time it
                                          // was genuinely transmitting a tune carrier, so
                                          // trusting that echo as ground truth silently
                                          // detached our UI from an actually-still-
                                          // transmitting radio. This flag is set/cleared
                                          // only by our own button/timeout/Cancel logic.
static char        s_prior_mode[8] = "USB";  // mode to restore to on exit
static uint32_t    s_enter_ms   = 0;         // esp_timer_get_time()/1000 at entry, for the timeout

static void status_reset_label(void)
{
    if (s_status_lbl) lv_label_set_text(s_status_lbl, "SWR --    -- W");
}

// Stops the Tune session's UI/poll state. Does NOT send any CAT command -
// callers that need the radio to actually exit Tune must cat_request_mode()
// the prior mode themselves before calling this (see action_btn_cb/
// cancel_btn_cb/status_timer_cb below).
static void do_stop(const char *toast_msg)
{
    s_active = false;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_action_lbl) lv_label_set_text(s_action_lbl, "Start Tune");
    if (s_action_btn) lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    status_reset_label();
    cat_tune_poll_set_active(false);
    if (toast_msg) ui_toast(toast_msg);
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    // No radio-side auto-exit detection: the QMX's MD; Get never actually
    // echoes mode digit 8 back (see s_active's comment above), so there is
    // no reliable CAT-only signal for "the radio left Tune on its own".
    // The safety timeout below is the only automatic exit.
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s_enter_ms > TUNE_TIMEOUT_MS) {
        cat_request_mode(s_prior_mode);
        do_stop(LV_SYMBOL_WARNING " Tune timed out - exited automatically");
        return;
    }
    float pw = -1.0f, swr = -1.0f;
    cat_pwr_swr_async_read(&pw, &swr);
    if (s_status_lbl) {
        char buf[48];
        if (swr >= 0.0f) {
            snprintf(buf, sizeof(buf), "SWR %.2f   %.1f W", (double)swr,
                     (double)(pw >= 0.0f ? pw : 0.0f));
        } else {
            snprintf(buf, sizeof(buf), "Tuning...");
        }
        lv_label_set_text(s_status_lbl, buf);
    }
}

static void action_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_active) {
        cat_request_mode(s_prior_mode);
        do_stop("Tune exited");
        return;
    }
    const char *cur = cat_get_mode_str();
    strncpy(s_prior_mode, (cur && cur[0]) ? cur : "USB", sizeof(s_prior_mode) - 1);
    s_prior_mode[sizeof(s_prior_mode) - 1] = '\0';
    s_enter_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_active = true;
    cat_request_mode("TUNE");
    cat_tune_poll_set_active(true);
    if (s_action_lbl) lv_label_set_text(s_action_lbl, "Stop Tune");
    if (s_action_btn) lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(0xB03020), 0);
    ui_toast(LV_SYMBOL_WARNING " QMX transmitting - Tune mode");
    if (!s_timer) s_timer = lv_timer_create(status_timer_cb, 500, NULL);
}

// Cancel always leaves the radio in a known-safe (not transmitting) state
// before closing - never just hides the window out from under an active
// tune with no way back to the Stop button.
static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_active) {
        cat_request_mode(s_prior_mode);
        do_stop(NULL);  // no toast - the window is closing anyway
    }
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_size(s_panel, 620, 460);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Antenna Tune");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *warn = lv_label_create(s_panel);
    lv_label_set_text(warn, LV_SYMBOL_WARNING " Transmits a tune carrier while active");
    lv_obj_set_style_text_color(warn, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_24, 0);
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 48);

    s_status_lbl = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 130);
    status_reset_label();

    s_action_btn = lv_btn_create(s_panel);
    lv_obj_set_size(s_action_btn, 320, 80);
    lv_obj_align(s_action_btn, LV_ALIGN_TOP_MID, 0, 210);
    lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_radius(s_action_btn, 8, 0);
    lv_obj_add_event_cb(s_action_btn, action_btn_cb, LV_EVENT_CLICKED, NULL);
    s_action_lbl = lv_label_create(s_action_btn);
    lv_label_set_text(s_action_lbl, "Start Tune");
    lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_action_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(s_action_lbl);

    // Red Cancel - bottom, matching the rest of this app's destructive/dismiss
    // button styling (wifi_config.c, ft8_tx_modal.c, etc.)
    lv_obj_t *cancel_btn = lv_btn_create(s_panel);
    lv_obj_set_size(cancel_btn, 240, 72);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0xc04040), 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cancel_lbl);

    ESP_LOGI(TAG, "Tune modal built");
}

void tune_modal_init(void)
{
    modal_build();
}

void tune_modal_show(void)
{
    modal_build();  // no-op if already built (idempotent via s_modal guard)
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    ESP_LOGI(TAG, "Tune modal shown");
}
