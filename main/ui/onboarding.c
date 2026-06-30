// First-boot onboarding flow: a polite "Connect to your WiFi?" prompt, then
// (once) the callsign/grid prompt. Gated by the persisted `onboarded` flag so
// it runs at most once. See onboarding.h.

#include "onboarding.h"
#include "ui_theme.h"
#include "wifi_config.h"
#include "identity_config.h"
#include "settings.h"

#include "esp_log.h"

static const char *TAG = "onboarding";

static lv_obj_t *s_modal = NULL;   // WiFi-prompt backdrop
static bool      s_open  = false;

static void prompt_close(void)
{
    if (s_modal && s_open) {
        lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
        s_open = false;
    }
}

// Step 2: offer callsign + grid if they aren't set yet. Onboarding is marked
// done either way, so a user who declines isn't nagged on the next boot.
static void start_identity_step(void)
{
    qmx_settings_t s;
    settings_load_all(&s);
    if (s.my_callsign[0] == '\0' || s.my_grid[0] == '\0') {
        ESP_LOGI(TAG, "offering callsign/grid setup");
        identity_config_modal_show();
    }
    settings_set_onboarded(true);
}

static void yes_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "WiFi prompt: Yes");
    settings_set_onboarded(true);   // asked once; don't re-prompt next boot
    prompt_close();
    // Open the WiFi modal and run the identity step once it's dismissed.
    wifi_config_modal_show_then(start_identity_step);
}

static void skip_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "WiFi prompt: Don't ask again");
    settings_set_onboarded(true);
    prompt_close();
    start_identity_step();          // still offer callsign/grid the one time
}

static void prompt_build(void)
{
    if (s_modal) return;
    lv_obj_t *scr = lv_screen_active();

    // Dimmed full-screen backdrop (modal).
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

    // Centred dialog panel.
    lv_obj_t *panel = lv_obj_create(s_modal);
    lv_obj_set_size(panel, 760, 320);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 28, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Connect to your WiFi?");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *body = lv_label_create(panel);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 700);
    lv_label_set_text(body,
        "WiFi gives you accurate FT8 clock sync and the web UI. "
        "You can always set it up later from the settings drawer.");
    lv_obj_set_style_text_color(body, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_24, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 64);

    // "Don't ask again" - neutral, bottom-left.
    lv_obj_t *skip = lv_btn_create(panel);
    lv_obj_set_size(skip, 320, 72);
    lv_obj_align(skip, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(skip, lv_color_hex(0x303044), 0);
    lv_obj_set_style_border_width(skip, 0, 0);
    lv_obj_set_style_radius(skip, 8, 0);
    lv_obj_add_event_cb(skip, skip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *skip_lbl = lv_label_create(skip);
    lv_label_set_text(skip_lbl, "Don't ask again");
    lv_obj_set_style_text_color(skip_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(skip_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(skip_lbl);

    // "Yes!" - green primary, bottom-right.
    lv_obj_t *yes = lv_btn_create(panel);
    lv_obj_set_size(yes, 320, 72);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_border_color(yes, lv_color_hex(0x4caf50), 0);
    lv_obj_set_style_border_width(yes, 2, 0);
    lv_obj_set_style_radius(yes, 8, 0);
    lv_obj_add_event_cb(yes, yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yes_lbl = lv_label_create(yes);
    lv_label_set_text(yes_lbl, "Yes!");
    lv_obj_set_style_text_color(yes_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(yes_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(yes_lbl);

    ESP_LOGI(TAG, "prompt built");
}

// Deferred entry (one-shot timer): decide what, if anything, to prompt.
static void onboarding_run(lv_timer_t *t)
{
    lv_timer_delete(t);   // one-shot

    qmx_settings_t s;
    settings_load_all(&s);
    if (s.onboarded) {
        ESP_LOGI(TAG, "already onboarded; nothing to do");
        return;
    }

    // Step 1: WiFi - only prompt if it isn't configured already.
    if (s.wifi_ssid[0] == '\0') {
        prompt_build();
        lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_modal);
        s_open = true;
        ESP_LOGI(TAG, "showing WiFi prompt");
        return;   // flow continues from yes_cb / skip_cb
    }

    // WiFi already configured -> go straight to the identity step.
    ESP_LOGI(TAG, "WiFi already set; skipping prompt");
    start_identity_step();
}

void onboarding_init(void)
{
    prompt_build();   // build now, while internal heap is at its boot maximum
    // Run the flow ~1.5 s after boot so the panadapter renders first.
    lv_timer_create(onboarding_run, 1500, NULL);
}
