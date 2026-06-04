#include "ui.h"
#include "render.h"
#include "render_waterfall.h"
#include "dsp.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "cat.h"
#include "screenshot.h"
#include "settings.h"
#include "wifi_config.h"
#include "iq_balance.h"
#include "ui_mode.h"
#include "ft8_screen.h"
#include "ft8_screen_view.h"
#include "ft8_test.h"

static const char *TAG = "ui";
static lv_obj_t *s_mode_btn_lbl = NULL;

// Layout constants (1280x720)
#define TOP_BAR_H       60
#define DRAWER_W        520  /* Phase 5.10D Stage 2: settings drawer width (520 in v0.8.x to fit WiFi button + smoothing slider) */
#define BOTTOM_BAR_H    36
#define SPECTRUM_H      200
#define LABEL_BAR_H     32  /* Phase 5.10C: room for Montserrat 18 labels under tick marks */
// Phase 5.10E: QMX I/Q has a 12 kHz IF offset -- the signal at the QMX's
// tuned frequency lands at +12 kHz in the baseband. We compensate by
// shifting the displayed spectrum left by 12 kHz so the tuned signal
// appears at the visual center. Touch-to-tune math is unchanged because
// s_last_qmx_freq_hz is the dial reading, not the LO.
#define IF_OFFSET_HZ    12000
#define WATERFALL_H     (DISPLAY_V_RES - TOP_BAR_H - SPECTRUM_H - LABEL_BAR_H - BOTTOM_BAR_H)

// Forward declarations (Phase 6.1 - touch-to-tune)
static void touch_event_cb(lv_event_t *e);
static void settings_button_cb(lv_event_t *e);  // Phase 5.10D
static uint32_t s_last_qmx_freq_hz = 0;  // updated by ui_update_frequency
static char s_current_mode[8] = "USB";  // Phase 5.10F: latest CAT mode for snap-aware tuning
static char s_current_band[8] = "---";  // Phase 9 (v0.9.5): cached band string for web JSON
static uint32_t s_passband_width_hz = 0;  // Phase 5.10G: 0 = use mode default; else from CAT FW
static uint16_t s_cw_pitch_hz = 700;  // CW sidetone offset (Hz); applied to touch-tune in CW modes

// Touch-target cursor state (Phase 6.1)
static int s_target_x = -1;
static uint64_t s_target_until_us = 0;
#define TARGET_DISPLAY_MS  600
// (s_last_qmx_freq_hz declared at top of file)

// Widget handles
static lv_obj_t *s_freq_label = NULL;
static lv_obj_t *s_smeter_label = NULL;
static lv_obj_t *s_band_label = NULL;   // Phase 5.10D: dedicated band slot
static lv_obj_t *s_mode_label = NULL;
static lv_obj_t *s_spectrum_obj = NULL;
static lv_obj_t *s_waterfall_obj = NULL;
static lv_obj_t *s_label_bar = NULL;
static lv_obj_t *s_status_label = NULL;  // legacy: single label, kept for compatibility (unused after Phase 5.13)
static lv_obj_t *s_bot_left   = NULL;
static lv_obj_t *s_bot_center = NULL;
static lv_obj_t *s_bot_right  = NULL;
static lv_obj_t *s_burger_btn = NULL;  // Phase 5.10I: kept for foreground move after all UI built
static lv_obj_t *s_switch_iq  = NULL;  // Phase B: IQ balance toggle in settings drawer
static lv_obj_t *s_switch_flat = NULL; // Phase 5.12: flat-spectrum toggle in settings drawer

// Phase 5.10D Stage 2: settings drawer state
static lv_obj_t *s_drawer = NULL;
static bool s_drawer_open = false;
// Phase 5.10D Stage 2b: drawer widgets we need to keep handles to
static lv_obj_t *s_slider_db_min = NULL;
static lv_obj_t *s_slider_db_max = NULL;
static lv_obj_t *s_slider_alpha = NULL;
static lv_obj_t *s_lbl_db_min = NULL;
static lv_obj_t *s_lbl_db_max = NULL;
static lv_obj_t *s_lbl_alpha = NULL;
static lv_obj_t *s_slider_cwpitch = NULL;
static lv_obj_t *s_lbl_cwpitch = NULL;
static lv_obj_t *s_dropdown_cmap = NULL;
static void drawer_preset_normal_cb(lv_event_t *e);
static void drawer_preset_dx_cb(lv_event_t *e);
static void drawer_preset_strong_cb(lv_event_t *e);
static void drawer_wifi_btn_cb(lv_event_t *e);
static void drawer_mode_btn_cb(lv_event_t *e);
static void ui_refresh_mode_button_label(void);
static void drawer_slider_db_min_cb(lv_event_t *e);
static void drawer_slider_db_max_cb(lv_event_t *e);
static void drawer_slider_alpha_cb(lv_event_t *e);
static void drawer_slider_cwpitch_cb(lv_event_t *e);
static void drawer_dropdown_cmap_cb(lv_event_t *e);
static void drawer_switch_flat_cb(lv_event_t *e);
bool ui_get_flat_mode(void);
void ui_set_flat_mode(bool on);
static void drawer_apply_preset(int db_min, int db_max, float alpha);
static void drawer_build(void);
static void drawer_open(void);
static void drawer_close(void);
static void drawer_anim_x_cb(void *obj, int32_t v);
static void drawer_close_button_cb(lv_event_t *e);
static void iq_balance_toggle_cb(lv_event_t *e);

// Phase 5.5: static defaults -- manual Ref/Range, user-controlled later
// (internal arbitrary dB scale; ~80=noise floor, ~125=strong signal on test rig)
static float DB_MIN_DISPLAY = -130.0f;  /* dBm, calibrated scale */
static float DB_MAX_DISPLAY = -30.0f;  /* dBm, headroom for S9+40 */

// Forward decl so build_spectrum can call this

static lv_obj_t *s_wf_canvas = NULL;
static uint8_t *s_wf_canvas_buf = NULL;

// Spectrum dB labels (Phase 5.4)
static lv_obj_t *s_db_max_label = NULL;
static lv_obj_t *s_db_min_label = NULL;

// Spectrum canvas (Phase 5.1)
static lv_obj_t *s_spec_canvas = NULL;
static uint8_t *s_spec_canvas_buf = NULL;

// ==== Top bar ====
static void build_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, DISPLAY_H_RES, TOP_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Phase 5.10D: top-bar layout -- Band | Mode | [center: Freq] | S-meter
    s_band_label = lv_label_create(bar);
    lv_label_set_text(s_band_label, "Band: ---");
    lv_obj_set_style_text_color(s_band_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_band_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_band_label, LV_ALIGN_LEFT_MID, 8, 0);

    s_mode_label = lv_label_create(bar);
    lv_label_set_text(s_mode_label, "Mode: USB");
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_LEFT_MID, 200, 0);

    s_freq_label = lv_label_create(bar);
    lv_label_set_text(s_freq_label, "Center Freq: 14.074.000 Hz");
    lv_obj_set_style_text_color(s_freq_label, lv_color_hex(0xFFD76B), 0);
    lv_obj_set_style_text_font(s_freq_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_freq_label, LV_ALIGN_CENTER, 0, 0);

    s_smeter_label = lv_label_create(bar);
    lv_label_set_text(s_smeter_label, "Signal: S0");
    lv_obj_set_style_text_color(s_smeter_label, lv_color_hex(0x00FF00), 0);  // Phase 5.10D: match spectrum trace green
    lv_obj_set_style_text_font(s_smeter_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_smeter_label, LV_ALIGN_CENTER, 320, 0);  // Phase 5.10D: centered in right half

    // Phase 5.10I: 80x80 burger, overflows downward into the spectrum.
    // Top bar stays at 60 px; clip content disabled so button can be larger.
    // Phase 5.10I: parent burger to the SCREEN, not the top bar.
    // Avoids the top bar's clipping issue. Positioned absolutely so it
    // straddles the top bar boundary and extends into the spectrum area.
    s_burger_btn = lv_btn_create(parent);  /* parent = screen */
    // Step 4c.2 polish: 60x60 (top-bar height), dim theme, flush in top-right.
    // Visible region only; touch-to-tune deadzone is a separate coordinate
    // filter in touch_event_cb and is unaffected by this resize.
    lv_obj_set_size(s_burger_btn, 60, 60);
    lv_obj_align(s_burger_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_burger_btn, lv_color_hex(0x202028), 0);
    lv_obj_set_style_bg_opa(s_burger_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_burger_btn, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(s_burger_btn, 1, 0);
    lv_obj_set_style_radius(s_burger_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_burger_btn, 0, 0);
    lv_obj_add_event_cb(s_burger_btn, settings_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *blbl = lv_label_create(s_burger_btn);
    lv_label_set_text(blbl, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(blbl, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(blbl, &lv_font_montserrat_24, 0);
    lv_obj_center(blbl);
}

// ==== Spectrum region (Phase 5.1: real-time line graph) ====
static void build_spectrum(lv_obj_t *parent)
{
    s_spectrum_obj = lv_obj_create(parent);
    lv_obj_set_size(s_spectrum_obj, DISPLAY_H_RES, SPECTRUM_H);
    lv_obj_align(s_spectrum_obj, LV_ALIGN_TOP_LEFT, 0, TOP_BAR_H);
    lv_obj_set_style_bg_color(s_spectrum_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(s_spectrum_obj, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(s_spectrum_obj, 1, 0);
    lv_obj_set_style_radius(s_spectrum_obj, 0, 0);
    lv_obj_set_style_pad_all(s_spectrum_obj, 0, 0);
    lv_obj_clear_flag(s_spectrum_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 1280 x 200 x 2 bytes = 512 KB in PSRAM
    size_t buf_size = LV_CANVAS_BUF_SIZE(DISPLAY_H_RES, SPECTRUM_H, 16,
                                         LV_DRAW_BUF_STRIDE_ALIGN);
    s_spec_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!s_spec_canvas_buf) {
        ESP_LOGE(TAG, "Failed to alloc spectrum canvas (%zu bytes)", buf_size);
        return;
    }
    memset(s_spec_canvas_buf, 0, buf_size);

    s_spec_canvas = lv_canvas_create(s_spectrum_obj);
    lv_obj_add_flag(s_spectrum_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_spectrum_obj, touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_spectrum_obj, touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_canvas_set_buffer(s_spec_canvas, s_spec_canvas_buf,
                         DISPLAY_H_RES, SPECTRUM_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_spec_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    // Phase 5.4: dB range labels (top-left and bottom-left of spectrum)
    s_db_max_label = lv_label_create(s_spectrum_obj);
    lv_label_set_text(s_db_max_label, "");
    lv_obj_set_style_text_color(s_db_max_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_db_max_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_db_max_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_db_max_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_db_max_label, 3, 0);
    lv_obj_align(s_db_max_label, LV_ALIGN_TOP_LEFT, 4, 2);

    s_db_min_label = lv_label_create(s_spectrum_obj);
    lv_label_set_text(s_db_min_label, "");
    lv_obj_set_style_text_color(s_db_min_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_db_min_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_db_min_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_db_min_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_db_min_label, 3, 0);
    lv_obj_align(s_db_min_label, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    // Phase 5.5: show static defaults immediately (no autoscale to update them)
    char buf_max[16], buf_min[16];
    snprintf(buf_max, sizeof(buf_max), "%.0f dBm", (double)DB_MAX_DISPLAY);
    snprintf(buf_min, sizeof(buf_min), "%.0f dBm", (double)DB_MIN_DISPLAY);
    lv_label_set_text(s_db_max_label, buf_max);
    lv_label_set_text(s_db_min_label, buf_min);
}

// ==== Label band (Phase 5.3): black strip between spectrum and waterfall with offset ticks ====
static lv_obj_t *s_label_canvas = NULL;
static uint8_t *s_label_canvas_buf = NULL;
// Phase 5.10C: handles for the 5 tick labels under the spectrum, so we
// can update them with actual MHz when the VFO changes.
static lv_obj_t *s_tick_labels[5] = { NULL, NULL, NULL, NULL, NULL };

static void build_label_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    s_label_bar = bar;
    lv_obj_set_size(bar, DISPLAY_H_RES, LABEL_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, TOP_BAR_H + SPECTRUM_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Tick canvas: DISPLAY_H_RES x LABEL_BAR_H, drawn once
    size_t buf_size = LV_CANVAS_BUF_SIZE(DISPLAY_H_RES, LABEL_BAR_H, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_label_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (s_label_canvas_buf) {
        memset(s_label_canvas_buf, 0, buf_size);
        // Draw ticks: major at multiples of 12 kHz (top-aligned, taller),
        // minor at multiples of 3 kHz (top-aligned, shorter)
        uint16_t *px = (uint16_t *)s_label_canvas_buf;
        const uint16_t major_color = 0xC618;  // light grey
        const uint16_t minor_color = 0x8410;  // medium grey
        const int center_x = DISPLAY_H_RES / 2;
        const float px_per_khz = (float)DISPLAY_H_RES / 48.0f;  // 26.67 at 1280 / 15 at 720
        for (int khz = -24; khz <= 24; khz += 3) {
            int x = center_x + (int)(khz * px_per_khz);
            if (x < 0 || x >= DISPLAY_H_RES) continue;
            int is_major = (khz % 12 == 0);
            int h = is_major ? 10 : 5;
            uint16_t color = is_major ? major_color : minor_color;
            for (int y = 0; y < h; y++) {
                px[y * DISPLAY_H_RES + x] = color;
            }
        }
        s_label_canvas = lv_canvas_create(bar);
        lv_canvas_set_buffer(s_label_canvas, s_label_canvas_buf,
                             DISPLAY_H_RES, LABEL_BAR_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_label_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    // Labels sit below the ticks. Phase 5.10C: store handles so the labels
    // can be rewritten with actual MHz on every VFO change.
    const int tick_xs[5] = { 0, 320, 640, 960, 1280 };
    for (int i = 0; i < 5; i++) {
        s_tick_labels[i] = lv_label_create(bar);
        lv_label_set_text(s_tick_labels[i], "--.---");
        lv_obj_set_style_text_color(s_tick_labels[i], lv_color_hex(0xA0A0A0), 0);
        lv_obj_set_style_text_font(s_tick_labels[i], &lv_font_montserrat_18, 0);
        if (i == 0) {
            lv_obj_align(s_tick_labels[i], LV_ALIGN_BOTTOM_LEFT, 2, 0);
        } else if (i == 4) {
            lv_obj_align(s_tick_labels[i], LV_ALIGN_BOTTOM_RIGHT, -2, 0);
        } else {
            lv_obj_align(s_tick_labels[i], LV_ALIGN_BOTTOM_LEFT, tick_xs[i] - 28, 0);
        }
    }
}

// Phase 5.10C: rewrite the 5 tick labels with absolute MHz centered on VFO.
// At 48 kHz span, ticks are at -24/-12/0/+12/+24 kHz. Format as 7.000 / 14.012 etc.
static void update_freq_axis_labels(uint32_t center_hz)
{
    const int offsets_khz[5] = { -24, -12, 0, +12, +24 };
    for (int i = 0; i < 5; i++) {
        if (!s_tick_labels[i]) continue;
        int32_t hz = (int32_t)center_hz + offsets_khz[i] * 1000;
        if (hz < 0) hz = 0;
        char buf[16];
        // Format: MM.HHH where MM = MHz, HHH = kHz (1 kHz resolution shown)
        snprintf(buf, sizeof(buf), "%lu.%03lu",
                 (unsigned long)(hz / 1000000),
                 (unsigned long)((hz / 1000) % 1000));
        lv_label_set_text(s_tick_labels[i], buf);
    }
}

// ==== Waterfall region (placeholder gradient from Phase 1) ====
static void build_waterfall(lv_obj_t *parent)
{
    s_waterfall_obj = lv_obj_create(parent);
    lv_obj_set_size(s_waterfall_obj, DISPLAY_H_RES, WATERFALL_H);
    lv_obj_align(s_waterfall_obj, LV_ALIGN_TOP_LEFT, 0, TOP_BAR_H + SPECTRUM_H + LABEL_BAR_H);
    lv_obj_set_style_bg_color(s_waterfall_obj, lv_color_hex(0x000010), 0);
    lv_obj_set_style_border_width(s_waterfall_obj, 0, 0);
    lv_obj_set_style_radius(s_waterfall_obj, 0, 0);
    lv_obj_set_style_pad_all(s_waterfall_obj, 0, 0);
    lv_obj_clear_flag(s_waterfall_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Allocate 2x WATERFALL_H so we can use the "double buffer" scroll trick:
    // new rows are written to both write_head and write_head+WATERFALL_H positions,
    // and the canvas view pointer moves through the buffer instead of memmove'ing.
    size_t buf_size = LV_CANVAS_BUF_SIZE(DISPLAY_H_RES, WATERFALL_H * 2, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_wf_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!s_wf_canvas_buf) {
        ESP_LOGE(TAG, "Failed to alloc waterfall canvas (%zu bytes)", buf_size);
        return;
    }

    s_wf_canvas = lv_canvas_create(s_waterfall_obj);
    lv_obj_add_flag(s_waterfall_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_waterfall_obj, touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_waterfall_obj, touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_canvas_set_buffer(s_wf_canvas, s_wf_canvas_buf,
                         DISPLAY_H_RES, WATERFALL_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_wf_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    // Initialize entire 2x buffer to black (waterfall starts empty)
    memset(s_wf_canvas_buf, 0, (size_t)DISPLAY_H_RES * WATERFALL_H * 2 * 2);
    lv_obj_invalidate(s_wf_canvas);
}

// ==== Bottom status bar ====
static void build_bottom_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, DISPLAY_H_RES, BOTTOM_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A1014), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 3-zone bottom bar: battery (left), UTC clock (center), WiFi (right).
    s_bot_left = lv_label_create(bar);
    lv_label_set_text(s_bot_left, "");
    lv_obj_set_style_text_color(s_bot_left, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_bot_left, &lv_font_montserrat_24, 0);
    lv_obj_align(s_bot_left, LV_ALIGN_LEFT_MID, 8, 0);

    s_bot_center = lv_label_create(bar);
    lv_label_set_text(s_bot_center, "");
    lv_obj_set_style_text_color(s_bot_center, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_bot_center, &lv_font_montserrat_24, 0);
    lv_obj_align(s_bot_center, LV_ALIGN_CENTER, 0, 0);

    s_bot_right = lv_label_create(bar);
    lv_label_set_text(s_bot_right, "");
    lv_obj_set_style_text_color(s_bot_right, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_bot_right, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_bot_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_bot_right, LV_ALIGN_RIGHT_MID, -8, 0);
}

// ==== Public API ====
void ui_init(lv_display_t *disp)
{
    display_lock(portMAX_DELAY);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_top_bar(scr);
    build_spectrum(scr);
    build_label_bar(scr);
    build_waterfall(scr);
    build_bottom_bar(scr);
    ft8_screen_view_init(scr);

    display_unlock();

    // Phase 5.10I: ensure the oversized burger sits on top of everything
    if (s_burger_btn) lv_obj_move_foreground(s_burger_btn);

    // Hidden 80x80 long-press screenshot region in top-left
    screenshot_init(scr);
    // Step 4c.2 fix: ensure the screenshot region stays on top of the
    // FT8 container (which was created later and otherwise wins z-order
    // wherever they overlap in the top 20 px).
    lv_obj_move_foreground(screenshot_get_btn());
    ESP_LOGI(TAG, "UI built: top=%dpx spectrum=%dpx labels=%dpx waterfall=%dpx bottom=%dpx",
             TOP_BAR_H, SPECTRUM_H, LABEL_BAR_H, WATERFALL_H, BOTTOM_BAR_H);
}

// Phase 5.10: forward declaration for band_from_freq (defined below)
static const char *band_from_freq(uint32_t freq_hz);
static void update_freq_axis_labels(uint32_t center_hz);  // Phase 5.10C

void ui_update_frequency(uint32_t freq_hz)
{
    s_last_qmx_freq_hz = freq_hz;
    settings_set_last_vfo(freq_hz);
    if (!s_freq_label) return;
    char buf[32];
    uint32_t mhz = freq_hz / 1000000;
    uint32_t khz = (freq_hz / 1000) % 1000;
    uint32_t hz  = freq_hz % 1000;
    snprintf(buf, sizeof(buf), "Center Freq: %lu.%03lu.%03lu Hz", mhz, khz, hz);
    if (display_lock(20)) {
        lv_label_set_text(s_freq_label, buf);
        display_unlock();
    }
    // Phase 5.10: derive band and push to UI
    const char *band = band_from_freq(freq_hz);
    if (band) ui_update_band(band);
    // Phase 5.10C: refresh the frequency axis labels under the spectrum
    if (display_lock(100)) {
        update_freq_axis_labels(freq_hz);
        display_unlock();
    } else {
        ESP_LOGW("ui", "ui_update_frequency: axis label lock timeout");
    }
}

// Phase 5.10: map QMX frequency to ham-band name.
// Returns NULL outside known band ranges.
static const char *band_from_freq(uint32_t freq_hz)
{
    if (freq_hz >= 1800000  && freq_hz < 2000000)  return "160m";
    if (freq_hz >= 3500000  && freq_hz < 4000000)  return "80m";
    if (freq_hz >= 5330500  && freq_hz < 5406500)  return "60m";
    if (freq_hz >= 7000000  && freq_hz < 7300000)  return "40m";
    if (freq_hz >= 10100000 && freq_hz < 10150000) return "30m";
    if (freq_hz >= 14000000 && freq_hz < 14350000) return "20m";
    if (freq_hz >= 17900000 && freq_hz < 18500000) return "17m";
    if (freq_hz >= 21000000 && freq_hz < 21450000) return "15m";
    if (freq_hz >= 24890000 && freq_hz < 24990000) return "12m";
    if (freq_hz >= 28000000 && freq_hz < 29700000) return "10m";
    if (freq_hz >= 50000000 && freq_hz < 54000000) return "6m";
    return NULL;
}

void ui_update_mode(const char *mode)
{
    // Phase 5.10F: cache for snap-step lookup in touch handler
    if (mode) {
        strncpy(s_current_mode, mode, sizeof(s_current_mode) - 1);
        s_current_mode[sizeof(s_current_mode) - 1] = '\0';
    }
    if (!s_mode_label || !mode) return;
    if (display_lock(100)) {
        char buf[32]; snprintf(buf, sizeof(buf), "Mode: %s", mode); lv_label_set_text(s_mode_label, buf);
        lv_obj_invalidate(s_mode_label);
        display_unlock();
    } else {
        ESP_LOGW("ui", "ui_update_mode: display_lock timeout for '%s'", mode);
    }
}

void ui_update_band(const char *band)
{
    if (!s_band_label || !band) return;
    strncpy(s_current_band, band, sizeof(s_current_band) - 1);
    s_current_band[sizeof(s_current_band) - 1] = '\0';
    if (display_lock(100)) {
        char buf[32]; snprintf(buf, sizeof(buf), "Band: %s", band); lv_label_set_text(s_band_label, buf);
        lv_obj_invalidate(s_band_label);
        display_unlock();
    } else {
        ESP_LOGW("ui", "ui_update_band: display_lock timeout for '%s'", band);
    }
}

// Phase 5.10G: receive passband width from CAT (FW response) or
// fallback to 0 = use per-mode defaults. Called by cat.c.
void ui_update_passband_width(uint32_t hz)
{
    if (hz > 0 && hz != s_passband_width_hz) {
        ESP_LOGI("ui", "Passband width = %lu Hz (CAT FW)", (unsigned long)hz);
    }
    s_passband_width_hz = hz;
}

// Helper: returns (low, high) passband edges in Hz, relative to VFO,
// from current mode + (optional) CAT FW width. CW is symmetric around VFO,
// USB is +200 to +(200+width), LSB is -(200+width) to -200, etc.
static void compute_passband_edges_hz(int32_t *out_low, int32_t *out_high)
{
    // Mode defaults if CAT FW didn't report
    uint32_t w = s_passband_width_hz;
    int32_t low, high;
    if (strstr(s_current_mode, "CW")) {
        if (w == 0) w = 300;
        low = -(int32_t)w / 2;
        high = (int32_t)w / 2;
    } else if (strstr(s_current_mode, "USB")) {
        if (w == 0) w = 2700;
        low = 200;
        high = 200 + (int32_t)w;
    } else if (strstr(s_current_mode, "LSB")) {
        if (w == 0) w = 2700;
        low = -(200 + (int32_t)w);
        high = -200;
    } else if (strstr(s_current_mode, "AM")) {
        if (w == 0) w = 6000;
        low = -(int32_t)w / 2;
        high = (int32_t)w / 2;
    } else if (strstr(s_current_mode, "FM")) {
        if (w == 0) w = 10000;
        low = -(int32_t)w / 2;
        high = (int32_t)w / 2;
    } else if (strstr(s_current_mode, "DiGi") || strstr(s_current_mode, "RTTY")
               || strstr(s_current_mode, "FT") || strstr(s_current_mode, "DIG")) {
        if (w == 0) w = 2700;
        low = 200;
        high = 200 + (int32_t)w;
    } else {
        // Unknown mode: a small symmetric default
        if (w == 0) w = 2700;
        low = -(int32_t)w / 2;
        high = (int32_t)w / 2;
    }
    *out_low = low;
    *out_high = high;
}


void ui_update_smeter(int s_units)
{
    if (!s_smeter_label) return;
    char buf[32];
    if (s_units > 108) s_units = 108;  // clamp at S9+99
    if (s_units <= 0) {
        snprintf(buf, sizeof(buf), "Signal: S0");
    } else if (s_units <= 9) {
        snprintf(buf, sizeof(buf), "Signal: S%d", s_units);
    } else {
        // S9+xx (over S9 is reported as +dB above S9)
        snprintf(buf, sizeof(buf), "Signal: S9+%d", s_units - 9);
    }
    if (display_lock(100)) {
        lv_label_set_text(s_smeter_label, buf);
        lv_obj_invalidate(s_smeter_label);
        display_unlock();
    }
}


void ui_set_db_range(float db_min, float db_max)
{
    // Clamp to sane bounds
    if (db_max - db_min < 10.0f) return;  // ignore degenerate ranges
    DB_MIN_DISPLAY = db_min;
    DB_MAX_DISPLAY = db_max;
    // Refresh labels so any caller (menu, one-shot auto, NVS load) sees the change
    ui_set_db_labels(db_min, db_max);
}

void ui_set_db_labels(float db_min, float db_max)
{
    if (!s_db_max_label || !s_db_min_label) return;
    char buf_max[16], buf_min[16];
    snprintf(buf_max, sizeof(buf_max), "%.0f dBm", (double)db_max);
    snprintf(buf_min, sizeof(buf_min), "%.0f dBm", (double)db_min);
    if (display_lock(20)) {
        lv_label_set_text(s_db_max_label, buf_max);
        lv_label_set_text(s_db_min_label, buf_min);
        display_unlock();
    }
}

static inline int db_to_y(float db)
{
    if (db < DB_MIN_DISPLAY) db = DB_MIN_DISPLAY;
    if (db > DB_MAX_DISPLAY) db = DB_MAX_DISPLAY;
    float norm = (db - DB_MIN_DISPLAY) / (DB_MAX_DISPLAY - DB_MIN_DISPLAY);
    int y = (int)((1.0f - norm) * (float)(SPECTRUM_H - 1));
    if (y < 0) y = 0;
    if (y > SPECTRUM_H - 1) y = SPECTRUM_H - 1;
    return y;
}

/* Phase 5.12: flat-spectrum mode (per-bin floor tracking) */
#define FLAT_SMOOTH_ALPHA     0.25f
#define FLAT_FLOOR_UP_ALPHA   0.002f
#define FLAT_FLOOR_DOWN_ALPHA 0.08f
#define FLAT_FLOOR_BIAS_DB    6.0f
#define FLAT_RANGE_DB         30.0f
static float *s_flat_smooth = NULL;
static float *s_flat_floor  = NULL;
static bool   s_flat_ready  = false;
static bool   s_flat_mode   = true;  /* TODO: drawer toggle + NVS */

void ui_push_spectrum(const float *bins, int n_bins)
{
    if (!s_spec_canvas_buf || !bins || n_bins <= 0) return;
    if (!display_lock(20)) return;

    uint16_t *px = (uint16_t *)s_spec_canvas_buf;
    static int s_prev_y_top = 0;
    const uint16_t fg = 0x07E0;  // green in RGB565
    const uint16_t grid_color = 0x4208;  // dim grey grid lines

    // Clear canvas to black
    memset(px, 0, (size_t)DISPLAY_H_RES * SPECTRUM_H * 2);

    // dB grid lines (Phase 5.3) - draw before spectrum so green overdraws on hits
    // Phase 5.12: suppressed in flat mode (dBm axis meaningless when shown as dB-above-floor)
    if (!s_flat_mode)
    {
        const float grid_dbs[5] = { -120.0f, -100.0f, -80.0f, -60.0f, -40.0f };
        for (int g = 0; g < 5; g++) {
            int gy = db_to_y(grid_dbs[g]);
            if (gy >= 0 && gy < SPECTRUM_H) {
                uint16_t *row = px + gy * DISPLAY_H_RES;
                for (int x = 0; x < DISPLAY_H_RES; x++) row[x] = grid_color;
            }
        }
    }

    /* Phase 5.12: per-bin floor + smooth update, once per frame */
    if (s_flat_mode) {
        if (!s_flat_smooth) {
            s_flat_smooth = heap_caps_malloc(n_bins * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_flat_floor  = heap_caps_malloc(n_bins * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_flat_smooth || !s_flat_floor) s_flat_mode = false;
        }
        if (s_flat_mode) {
            if (!s_flat_ready) {
                float sum = 0.0f;
                for (int b = 0; b < n_bins; b++) { s_flat_smooth[b] = bins[b]; sum += bins[b]; }
                float avg = sum / (float)n_bins;
                for (int b = 0; b < n_bins; b++) s_flat_floor[b] = avg;
                s_flat_ready = true;
            } else {
                for (int b = 0; b < n_bins; b++) {
                    s_flat_smooth[b] += FLAT_SMOOTH_ALPHA * (bins[b] - s_flat_smooth[b]);
                    float d = s_flat_smooth[b] - s_flat_floor[b];
                    float a = (d > 0.0f) ? FLAT_FLOOR_UP_ALPHA : FLAT_FLOOR_DOWN_ALPHA;
                    s_flat_floor[b] += a * d;
                }
            }
        }
    }

    int N = n_bins;
    int half = N / 2;

    for (int x = 0; x < DISPLAY_H_RES; x++) {
        int shifted = (int)((float)x * (float)N / (float)DISPLAY_H_RES);
        if (shifted < 0) shifted = 0;
        if (shifted >= N) shifted = N - 1;

        int bin;
        if (shifted < half) {
            bin = shifted + half;
        } else {
            bin = shifted - half;
        }
        // Phase 5.10E: 12 kHz IF offset compensation. Shift selected bin
        // right by (IF_OFFSET_HZ / sample_rate * N) bins so the QMX tuned
        // frequency (+12 kHz in baseband) appears at the visual center.
        bin = (bin + (IF_OFFSET_HZ * N) / 48000) % N;
        if (bin < 0) bin += N;

        int y_top;
        if (s_flat_mode) {
            /* 5-tap spatial smooth on (smooth - floor), then map to flat-axis y. */
            float sum = 0.0f;
            int   cnt = 0;
            for (int dx = -2; dx <= 2; dx++) {
                int xn = x + dx;
                if (xn < 0 || xn >= DISPLAY_H_RES) continue;
                int sn = (int)((float)xn * (float)N / (float)DISPLAY_H_RES);
                if (sn < 0) sn = 0;
                if (sn >= N) sn = N - 1;
                int bn = (sn < half) ? (sn + half) : (sn - half);
                bn = (bn + (IF_OFFSET_HZ * N) / 48000) % N;
                if (bn < 0) bn += N;
                sum += s_flat_smooth[bn] - s_flat_floor[bn];
                cnt++;
            }
            float v = sum / (float)cnt - FLAT_FLOOR_BIAS_DB;
            if (v < 0.0f) v = 0.0f;
            if (v > FLAT_RANGE_DB) v = FLAT_RANGE_DB;
            y_top = SPECTRUM_H - 1 - (int)(v * (SPECTRUM_H - 1) / FLAT_RANGE_DB);
        } else {
            y_top = db_to_y(bins[bin]);
        }
        if (y_top < 0) y_top = 0;
        if (y_top >= SPECTRUM_H) y_top = SPECTRUM_H - 1;
        // Phase 5.9: continuous spectrum curve. Connect this column's y_top to
        // the previous column's y_top with a bright line, then fill the area
        // below with a dim green (matches docs/panadapter-mockup-ideal.svg).
        const uint16_t fg_dim = 0x01C0;  // ~25% green in RGB565
        // Connect from prev_y to y_top vertically so the curve is continuous,
        // not a series of disconnected column tops.
        int y_a = (x > 0) ? s_prev_y_top : y_top;
        int y_b = y_top;
        int y_lo = (y_a < y_b) ? y_a : y_b;
        int y_hi = (y_a > y_b) ? y_a : y_b;
        for (int y = y_lo; y <= y_hi; y++) {
            px[y * DISPLAY_H_RES + x] = fg;
        }
        // Dim fill from just below the connecting line down to the bottom.
        for (int y = y_hi + 1; y < SPECTRUM_H; y++) {
            px[y * DISPLAY_H_RES + x] = fg_dim;
        }
        s_prev_y_top = y_top;
    }

    // Center cursor: amber 1-px vertical line at canvas center (where QMX is tuned)
    {
        // Phase 5.10G: passband edges (2 px grey lines)
        int32_t pb_low_hz, pb_high_hz;
        compute_passband_edges_hz(&pb_low_hz, &pb_high_hz);
        const uint16_t pb_color = 0x8410;  /* medium grey */
        for (int side = 0; side < 2; side++) {
            int32_t edge_hz = (side == 0) ? pb_low_hz : pb_high_hz;
            /* Edge frequency in Hz -> screen x. Sample rate = 48 kHz spans full width. */
            int edge_x = DISPLAY_H_RES / 2 + (int)((int64_t)edge_hz * DISPLAY_H_RES / 48000);
            if (edge_x < 0 || edge_x >= DISPLAY_H_RES) continue;
            for (int y = 0; y < SPECTRUM_H; y++) {
                px[y * DISPLAY_H_RES + edge_x] = pb_color;
                if (edge_x + 1 < DISPLAY_H_RES) px[y * DISPLAY_H_RES + edge_x + 1] = pb_color;
            }
        }

        const uint16_t center_color = 0xFD00;
        int cx = DISPLAY_H_RES / 2;
        for (int y = 0; y < SPECTRUM_H; y++) {
            px[y * DISPLAY_H_RES + cx] = center_color;
        }
    }
    // Target cursor: cyan 1-px vertical line at last touched x, ~600 ms
    if (s_target_x >= 0) {
        uint64_t now = esp_timer_get_time();
        if (now < s_target_until_us) {
            const uint16_t target_color = 0x07FF;
            int tx = s_target_x;
            if (tx >= 0 && tx < DISPLAY_H_RES) {
                for (int y = 0; y < SPECTRUM_H; y++) {
                    px[y * DISPLAY_H_RES + tx] = target_color;
                }
            }
        } else {
            s_target_x = -1;
        }
    }
    lv_obj_invalidate(s_spec_canvas);
    display_unlock();
}

// Moving-pointer (double-buffer) scroll. We allocate a 2*WATERFALL_H buffer.
// Each tick we write the new row to position s_wf_head AND to s_wf_head + WATERFALL_H,
// then we increment s_wf_head (mod WATERFALL_H). The canvas's buffer pointer is set
// to (base + s_wf_head*row_bytes), giving LVGL a contiguous WATERFALL_H view of the
// freshest data. No memmove needed.
//
// Visual order: row 0 of the LVGL canvas view = newest row (top); WATERFALL_H-1 = oldest.
// To achieve this, after incrementing s_wf_head, the "newest" row in the buffer is at
// (s_wf_head - 1) mod WATERFALL_H, and we want the canvas view to start with it at row 0.
// That means the view base = the position of the newest row.
static int s_wf_head = 0;  // next write position (0..WATERFALL_H-1)

// (touch-target cursor state declared near top of file)

// Waterfall scroll via moving-pointer trick.
// build_waterfall() allocates the canvas buffer at 2x WATERFALL_H height so we
// can write each new row to BOTH s_wf_head and s_wf_head + WATERFALL_H. The
// head DECREMENTS each tick, so the just-written row is at the TOP of the
// WATERFALL_H window that starts at s_wf_head. We point the canvas's buffer at
// that window. No memmove needed; ~100 us/tick instead of ~92 ms.
void ui_push_waterfall_row(const uint8_t *rgb565_row)
{
    if (!s_wf_canvas_buf || !rgb565_row) return;

    const size_t row_bytes = DISPLAY_H_RES * 2;  // RGB565 = 2 B/px

    if (!display_lock(20)) return;

    // Decrement head (with wrap), then write the new row at head and head+WATERFALL_H.
    s_wf_head = (s_wf_head + WATERFALL_H - 1) % WATERFALL_H;
    memcpy(s_wf_canvas_buf +  s_wf_head                * row_bytes, rgb565_row, row_bytes);
    memcpy(s_wf_canvas_buf + (s_wf_head + WATERFALL_H) * row_bytes, rgb565_row, row_bytes);

    // Point the canvas at the WATERFALL_H window starting at s_wf_head.
    // Newest row sits at view row 0 (top), oldest at view row WATERFALL_H-1 (bottom).
    lv_canvas_set_buffer(s_wf_canvas,
                         s_wf_canvas_buf + s_wf_head * row_bytes,
                         DISPLAY_H_RES, WATERFALL_H, LV_COLOR_FORMAT_RGB565);

    lv_obj_invalidate(s_wf_canvas);
    display_unlock();
}

void ui_set_bottom_left(const char *text)
{
    if (!s_bot_left) return;
    if (display_lock(20)) {
        lv_label_set_text(s_bot_left, text ? text : "");
        display_unlock();
    }
}

void ui_set_bottom_center(const char *text)
{
    if (!s_bot_center) return;
    if (display_lock(20)) {
        lv_label_set_text(s_bot_center, text ? text : "");
        display_unlock();
    }
}

void ui_set_bottom_right(const char *text)
{
    if (!s_bot_right) return;
    if (display_lock(20)) {
        lv_label_set_text(s_bot_right, text ? text : "");
        display_unlock();
    }
}

void ui_set_fps_text(const char *text)
{
    (void)text;  // legacy no-op; status.c now uses zone setters
    if (!s_status_label) return;
    if (display_lock(20)) {
        lv_label_set_text(s_status_label, text);
        display_unlock();
    }
}










// =============================================================================
// Touch-to-tune (Phase 6.1)
// =============================================================================

#define UAC_SAMPLE_RATE   48000     // I/Q sample rate from QMX
#define DSP_FFT_SIZE_HZ   48000     // full FFT span = sample rate (complex FFT)
#define TUNE_ROUND_HZ     10        // round target frequency to nearest 10 Hz

// (s_last_qmx_freq_hz declared at top of file)

static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    // p.x is screen-x; matches canvas-x because spectrum/waterfall are full-width at x=0.
    if (p.x < 0 || p.x >= DISPLAY_H_RES) return;

    if (code == LV_EVENT_PRESSING) {
        // Live preview: cyan cursor tracks the finger; refresh the 200ms lingering window
        // every tick so the line stays visible while the finger is down.
        s_target_x = (int)p.x;
        s_target_until_us = esp_timer_get_time() + 200000;  // 200 ms grace after lift
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (s_last_qmx_freq_hz == 0) return;  // no freq known yet, can't tune
        // Phase 5.10D: deadzone under-and-left of the burger button.
        // Burger is at top-right (~x=1216..1276, ~y=top bar). Block touches
        // landing in a 180x80 region in the top-right of the spectrum so a
        // wide finger pressing the button doesn't also retune.
        if (p.x >= 1080 && p.y < 120) {  // Phase 5.10I: enlarged for 80x80 burger
            ESP_LOGI("ui_touch", "RELEASED in burger deadzone (200x120) (x=%d y=%d) - ignored", (int)p.x, (int)p.y);
            return;
        }

        // Screenshot button deadzone: top-left 80x80
        if (p.x < 80 && p.y < 80) {
            ESP_LOGI("ui_touch", "RELEASED in screenshot deadzone (80x80) (x=%d y=%d) - ignored", (int)p.x, (int)p.y);
            return;
        }

        // Compute target frequency from final touch position
        int dx = (int)p.x - DISPLAY_H_RES / 2;
        int32_t offset_hz = (int32_t)((int64_t)dx * UAC_SAMPLE_RATE / DISPLAY_H_RES);
        // Snap to strongest bin within +/-700 Hz of touch (handler falls through if no peak).
        int32_t snapped_hz = offset_hz;
        if (dsp_find_peak_hz_around(offset_hz, 700, &snapped_hz) == ESP_OK) {
            if (snapped_hz != offset_hz) {
                ESP_LOGI("ui_touch", "snap-to-peak: %ld -> %ld Hz", (long)offset_hz, (long)snapped_hz);
            }
            offset_hz = snapped_hz;
        }
        // CW pitch correction: signal sits at +pitch (CW/USB-side) or -pitch (CW-R/LSB-side)
        // above/below the dial. Subtract/add so the touched audio peak lands at sidetone.
        // CW-R must be tested before CW (strstr would match both).
        if (strstr(s_current_mode, "CW-R") || strstr(s_current_mode, "CWR")) {
            offset_hz += (int32_t)s_cw_pitch_hz;
        } else if (strstr(s_current_mode, "CW")) {
            offset_hz -= (int32_t)s_cw_pitch_hz;
        }
        // Phase 5.10F: mode-aware snap. CW=10 Hz (precision), SSB=500 Hz
        // (voice channels), FT8/data=100 Hz, AM/FM=1 kHz.
        int32_t snap = 10;
        if (strstr(s_current_mode, "USB") || strstr(s_current_mode, "LSB")) snap = 500;
        else if (strstr(s_current_mode, "FT") || strstr(s_current_mode, "DIG") || strstr(s_current_mode, "RTTY")
                 || strstr(s_current_mode, "DiGi")) snap = 100;
        else if (strstr(s_current_mode, "AM") || strstr(s_current_mode, "FM")) snap = 1000;
        else if (strstr(s_current_mode, "CW")) snap = 10;
        int32_t rounded = (offset_hz + (offset_hz >= 0 ? snap/2 : -snap/2)) / snap * snap;
        int64_t target = (int64_t)s_last_qmx_freq_hz + rounded;
        if (target < 0) return;
        uint32_t target_hz = (uint32_t)target;

        esp_err_t err = cat_set_frequency(target_hz);
        ESP_LOGI("ui_touch", "RELEASED x=%d dx=%d off=%ld tgt=%lu err=0x%x",
                 (int)p.x, dx, (long)rounded, (unsigned long)target_hz, err);
        // Phase 5.10H: optimistically update the on-screen freq label
        // immediately so the user sees their target before the CAT FA
        // poll confirms (~300 ms later). If the QMX rejects the tune,
        // the next CAT FA will correct the display.
        if (err == ESP_OK) {
            ui_update_frequency(target_hz);
        }
        // Let the cursor linger briefly after release, then clear
        s_target_until_us = esp_timer_get_time() + 200000;
    }
}

void ui_set_cw_pitch_hz(uint16_t hz)
{
    if (hz < 300 || hz > 1200) return;  // sanity clamp
    s_cw_pitch_hz = hz;
    settings_set_cw_pitch_hz(hz);
    ESP_LOGI(TAG, "CW pitch set to %u Hz", (unsigned)hz);
}

// Hook into ui_update_frequency to track latest known QMX frequency


















// Phase 5.10D Stage 2: burger menu click -- toggle settings drawer
static void settings_button_cb(lv_event_t *e)
{
    (void)e;
    if (s_drawer_open) {
        drawer_close();
    } else {
        drawer_open();
    }
}

// Animate x position. Used for slide-in/out.
static void drawer_anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void drawer_close_button_cb(lv_event_t *e)
{
    (void)e;
    drawer_close();
}

static void iq_balance_toggle_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    iq_balance_set_enabled(on);
    settings_set_iq_enabled(on);
    if (on) iq_balance_reset();
}

// Build the drawer once. Hidden off-screen on the right initially.
static void drawer_build(void)
{
    if (s_drawer) return;

    lv_obj_t *scr = lv_screen_active();
    s_drawer = lv_obj_create(scr);
    lv_obj_set_size(s_drawer, DRAWER_W, DISPLAY_V_RES);
    // Park off-screen to the right
    lv_obj_set_pos(s_drawer, DISPLAY_H_RES, 0);
    lv_obj_set_style_bg_color(s_drawer, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_drawer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_drawer, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(s_drawer, 1, 0);
    lv_obj_set_style_border_side(s_drawer, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(s_drawer, 0, 0);
    lv_obj_set_style_pad_all(s_drawer, 16, 0);
    // Drawer scrolls vertically — content overflows once CW section is added.
    lv_obj_set_scroll_dir(s_drawer, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_drawer, LV_SCROLLBAR_MODE_AUTO);

    // Title bar with "Settings" + close X
    lv_obj_t *title = lv_label_create(s_drawer);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // Phase 5.10I: bigger close target (80x80 matching the burger)
    lv_obj_t *close_btn = lv_btn_create(s_drawer);
    lv_obj_set_size(close_btn, 80, 80);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(close_btn, drawer_close_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_32, 0);  // Phase 5.10I: matching the burger
    lv_obj_center(close_lbl);

    // === Phase 5.10D Stage 2b: presets + sliders ===
    // v0.8.x layout: 520 wide drawer, _24pt fonts, IQ row moved below title,
    // presets 3-across in a single row to free vertical space.
    int y = 96;

    // IQ balance ON/OFF row -- full width, well clear of the close button
    {
        lv_obj_t *iq_lbl = lv_label_create(s_drawer);
        lv_label_set_text(iq_lbl, "IQ Balance");
        lv_obj_set_style_text_color(iq_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(iq_lbl, &lv_font_montserrat_24, 0);
        lv_obj_align(iq_lbl, LV_ALIGN_TOP_LEFT, 0, y + 6);
        s_switch_iq = lv_switch_create(s_drawer);
        lv_obj_set_size(s_switch_iq, 72, 36);
        lv_obj_align(s_switch_iq, LV_ALIGN_TOP_RIGHT, 0, y);
        if (iq_balance_is_enabled()) lv_obj_add_state(s_switch_iq, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_switch_iq, iq_balance_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 56;
    }

    // Phase 5.12: Flat Spectrum ON/OFF row
    {
        lv_obj_t *flat_lbl = lv_label_create(s_drawer);
        lv_label_set_text(flat_lbl, "Flat Spectrum");
        lv_obj_set_style_text_color(flat_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(flat_lbl, &lv_font_montserrat_24, 0);
        lv_obj_align(flat_lbl, LV_ALIGN_TOP_LEFT, 0, y + 6);
        s_switch_flat = lv_switch_create(s_drawer);
        lv_obj_set_size(s_switch_flat, 72, 36);
        lv_obj_align(s_switch_flat, LV_ALIGN_TOP_RIGHT, 0, y);
        if (ui_get_flat_mode()) lv_obj_add_state(s_switch_flat, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_switch_flat, drawer_switch_flat_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 56;
    }
    // Presets section header
    lv_obj_t *presets_hdr = lv_label_create(s_drawer);
    lv_label_set_text(presets_hdr, "Presets");
    lv_obj_set_style_text_color(presets_hdr, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(presets_hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(presets_hdr, LV_ALIGN_TOP_LEFT, 0, y);
    y += 36;

    // Three preset buttons, side-by-side in a single row
    {
        const char *preset_names[3] = { "HF Normal", "HF DX", "Strong Sig." };
        lv_event_cb_t preset_cbs[3] = {
            drawer_preset_normal_cb,
            drawer_preset_dx_cb,
            drawer_preset_strong_cb,
        };
        const int row_w   = DRAWER_W - 32;   // inner usable width
        const int gap     = 8;
        const int btn_w   = (row_w - 2 * gap) / 3;
        const int btn_h   = 56;
        for (int i = 0; i < 3; i++) {
            lv_obj_t *btn = lv_btn_create(s_drawer);
            lv_obj_set_size(btn, btn_w, btn_h);
            lv_obj_align(btn, LV_ALIGN_TOP_LEFT, i * (btn_w + gap), y);
            lv_obj_add_event_cb(btn, preset_cbs[i], LV_EVENT_CLICKED, NULL);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, preset_names[i]);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
            lv_obj_center(lbl);
        }
        y += btn_h + 16;
    }

    // FT8/Panadapter mode toggle -- full width
    {
        lv_obj_t *btn = lv_btn_create(s_drawer);
        lv_obj_set_size(btn, DRAWER_W - 32, 56);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2c4d6e), 0);
        lv_obj_add_event_cb(btn, drawer_mode_btn_cb, LV_EVENT_CLICKED, NULL);
        s_mode_btn_lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(s_mode_btn_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_mode_btn_lbl, lv_color_hex(0xffffff), 0);
        lv_obj_center(s_mode_btn_lbl);
        ui_refresh_mode_button_label();
        y += 72;
    }
    // WiFi configuration button -- full width
    {
        lv_obj_t *btn = lv_btn_create(s_drawer);
        lv_obj_set_size(btn, DRAWER_W - 32, 56);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2c4d6e), 0);
        lv_obj_add_event_cb(btn, drawer_wifi_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "WiFi");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_center(lbl);
        y += 72;
    }

    // dB Range section header
    lv_obj_t *db_hdr = lv_label_create(s_drawer);
    lv_label_set_text(db_hdr, "dB Range");
    lv_obj_set_style_text_color(db_hdr, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(db_hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(db_hdr, LV_ALIGN_TOP_LEFT, 0, y);
    y += 40;

    // dB min slider
    s_lbl_db_min = lv_label_create(s_drawer);
    lv_label_set_text(s_lbl_db_min, "Min: -130 dBm");
    lv_obj_set_style_text_color(s_lbl_db_min, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_db_min, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_db_min, LV_ALIGN_TOP_LEFT, 0, y);
    y += 30;

    s_slider_db_min = lv_slider_create(s_drawer);
    lv_obj_set_size(s_slider_db_min, DRAWER_W - 32, 30);
    lv_slider_set_range(s_slider_db_min, -150, -50);
    lv_slider_set_value(s_slider_db_min, -130, LV_ANIM_OFF);
    lv_obj_align(s_slider_db_min, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_add_event_cb(s_slider_db_min, drawer_slider_db_min_cb, LV_EVENT_VALUE_CHANGED, NULL);
    y += 52;

    // dB max slider
    s_lbl_db_max = lv_label_create(s_drawer);
    lv_label_set_text(s_lbl_db_max, "Max: -30 dBm");
    lv_obj_set_style_text_color(s_lbl_db_max, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_db_max, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_db_max, LV_ALIGN_TOP_LEFT, 0, y);
    y += 30;

    s_slider_db_max = lv_slider_create(s_drawer);
    lv_obj_set_size(s_slider_db_max, DRAWER_W - 32, 30);
    lv_slider_set_range(s_slider_db_max, -50, 10);
    lv_slider_set_value(s_slider_db_max, -30, LV_ANIM_OFF);
    lv_obj_align(s_slider_db_max, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_add_event_cb(s_slider_db_max, drawer_slider_db_max_cb, LV_EVENT_VALUE_CHANGED, NULL);
    y += 60;

    // Smoothing section header
    lv_obj_t *sm_hdr = lv_label_create(s_drawer);
    lv_label_set_text(sm_hdr, "Smoothing");
    lv_obj_set_style_text_color(sm_hdr, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(sm_hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(sm_hdr, LV_ALIGN_TOP_LEFT, 0, y);
    y += 40;

    s_lbl_alpha = lv_label_create(s_drawer);
    lv_label_set_text(s_lbl_alpha, "Alpha: 0.40");
    lv_obj_set_style_text_color(s_lbl_alpha, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_alpha, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_alpha, LV_ALIGN_TOP_LEFT, 0, y);
    y += 30;

    s_slider_alpha = lv_slider_create(s_drawer);
    lv_obj_set_size(s_slider_alpha, DRAWER_W - 32, 30);
    lv_slider_set_range(s_slider_alpha, 5, 100);   // = alpha 0.05..1.00
    lv_slider_set_value(s_slider_alpha, 40, LV_ANIM_OFF);
    lv_obj_align(s_slider_alpha, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_add_event_cb(s_slider_alpha, drawer_slider_alpha_cb, LV_EVENT_VALUE_CHANGED, NULL);
    y += 60;
    // CW section header
    lv_obj_t *cw_hdr = lv_label_create(s_drawer);
    lv_label_set_text(cw_hdr, "CW");
    lv_obj_set_style_text_color(cw_hdr, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(cw_hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(cw_hdr, LV_ALIGN_TOP_LEFT, 0, y);
    y += 40;
    s_lbl_cwpitch = lv_label_create(s_drawer);
    lv_label_set_text(s_lbl_cwpitch, "Pitch: 700 Hz");
    lv_obj_set_style_text_color(s_lbl_cwpitch, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_cwpitch, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_cwpitch, LV_ALIGN_TOP_LEFT, 0, y);
    y += 30;
    s_slider_cwpitch = lv_slider_create(s_drawer);
    lv_obj_set_size(s_slider_cwpitch, DRAWER_W - 32, 30);
    lv_slider_set_range(s_slider_cwpitch, 400, 1000);
    lv_slider_set_value(s_slider_cwpitch, (int)s_cw_pitch_hz, LV_ANIM_OFF);
    lv_obj_align(s_slider_cwpitch, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_add_event_cb(s_slider_cwpitch, drawer_slider_cwpitch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    char cwbuf[24];
    snprintf(cwbuf, sizeof(cwbuf), "Pitch: %u Hz", (unsigned)s_cw_pitch_hz);
    lv_label_set_text(s_lbl_cwpitch, cwbuf);
    y += 60;
    // Waterfall colour-map section
    lv_obj_t *cmap_hdr = lv_label_create(s_drawer);
    lv_label_set_text(cmap_hdr, "Waterfall colour map");
    lv_obj_set_style_text_color(cmap_hdr, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(cmap_hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(cmap_hdr, LV_ALIGN_TOP_LEFT, 0, y);
    y += 40;
    s_dropdown_cmap = lv_dropdown_create(s_drawer);
    lv_dropdown_set_options(s_dropdown_cmap, "Thermal\nViridis\nTurbo\nGrayscale");
    lv_obj_set_size(s_dropdown_cmap, DRAWER_W - 32, 50);
    lv_obj_align(s_dropdown_cmap, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_text_font(s_dropdown_cmap, &lv_font_montserrat_24, 0);
    {
        qmx_settings_t scfg;
        settings_load_all(&scfg);
        if (scfg.colormap_idx < 4) lv_dropdown_set_selected(s_dropdown_cmap, scfg.colormap_idx);
    }
    lv_obj_add_event_cb(s_dropdown_cmap, drawer_dropdown_cmap_cb, LV_EVENT_VALUE_CHANGED, NULL);

    ESP_LOGI(TAG, "Settings drawer built (off-screen at x=%d)", DISPLAY_H_RES);
}

static void drawer_open(void)
{
    drawer_build();  // lazy build on first open
    if (!s_drawer || s_drawer_open) return;
    lv_obj_move_foreground(s_drawer);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_drawer);
    lv_anim_set_exec_cb(&a, drawer_anim_x_cb);
    lv_anim_set_values(&a, DISPLAY_H_RES, DISPLAY_H_RES - DRAWER_W);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    s_drawer_open = true;
    ESP_LOGI(TAG, "Settings drawer open");
}

static void drawer_close(void)
{
    if (!s_drawer || !s_drawer_open) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_drawer);
    lv_anim_set_exec_cb(&a, drawer_anim_x_cb);
    lv_anim_set_values(&a, DISPLAY_H_RES - DRAWER_W, DISPLAY_H_RES);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
    s_drawer_open = false;
    ESP_LOGI(TAG, "Settings drawer closed");
}


// === Phase 5.10D Stage 2b: drawer button + slider callbacks ===

static void drawer_apply_preset(int db_min, int db_max, float alpha)
{
    if (s_slider_db_min) lv_slider_set_value(s_slider_db_min, db_min, LV_ANIM_OFF);
    if (s_slider_db_max) lv_slider_set_value(s_slider_db_max, db_max, LV_ANIM_OFF);
    if (s_slider_alpha)  lv_slider_set_value(s_slider_alpha, (int)(alpha * 100.0f + 0.5f), LV_ANIM_OFF);

    char buf[24];
    if (s_lbl_db_min) {
        snprintf(buf, sizeof(buf), "Min: %d dBm", db_min);
        lv_label_set_text(s_lbl_db_min, buf);
    }
    if (s_lbl_db_max) {
        snprintf(buf, sizeof(buf), "Max: %d dBm", db_max);
        lv_label_set_text(s_lbl_db_max, buf);
    }
    if (s_lbl_alpha) {
        snprintf(buf, sizeof(buf), "Alpha: %.2f", (double)alpha);
        lv_label_set_text(s_lbl_alpha, buf);
    }

    ui_set_db_range((float)db_min, (float)db_max);
    render_set_ema_alpha(alpha);
    settings_set_db_min((float)db_min);
    settings_set_db_max((float)db_max);
    settings_set_ema_alpha(alpha);
}

/* Phase 5.12: flat-spectrum mode accessor + drawer callback */
bool ui_get_flat_mode(void)
{
    return s_flat_mode;
}

void ui_set_flat_mode(bool on)
{
    s_flat_mode  = on;
    s_flat_ready = false;  /* re-seed floor on next draw */
    if (s_switch_flat) {
        if (on) lv_obj_add_state(s_switch_flat, LV_STATE_CHECKED);
        else    lv_obj_remove_state(s_switch_flat, LV_STATE_CHECKED);
    }
    // Hide absolute dBm labels in flat mode (axis is dB-above-floor; labels misleading).
    if (s_db_min_label && s_db_max_label) {
        if (on) {
            lv_obj_add_flag(s_db_min_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_db_max_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_db_min_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_db_max_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void drawer_switch_flat_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_flat_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);
    s_flat_ready = false;  /* re-seed floor next time flat mode draws */
    ESP_LOGI(TAG, "flat-spectrum mode: %s", s_flat_mode ? "ON" : "OFF");
    settings_set_flat_mode(s_flat_mode);
    if (s_db_min_label && s_db_max_label) {
        if (s_flat_mode) {
            lv_obj_add_flag(s_db_min_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_db_max_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_db_min_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_db_max_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void drawer_preset_normal_cb(lv_event_t *e)  { (void)e; drawer_apply_preset(-130, -30, 0.40f); }
static void drawer_preset_dx_cb(lv_event_t *e)      { (void)e; drawer_apply_preset(-130, -50, 0.60f); }
static void drawer_preset_strong_cb(lv_event_t *e)  { (void)e; drawer_apply_preset(-110, -20, 0.20f); }
static void drawer_wifi_btn_cb(lv_event_t *e)       { (void)e; wifi_config_modal_show(); }

static void drawer_slider_db_min_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    char buf[24];
    snprintf(buf, sizeof(buf), "Min: %d dBm", v);
    if (s_lbl_db_min) lv_label_set_text(s_lbl_db_min, buf);
    int max_v = s_slider_db_max ? (int)lv_slider_get_value(s_slider_db_max) : -30;
    if (v >= max_v) v = max_v - 5;
    ui_set_db_range((float)v, (float)max_v);
    settings_set_db_min((float)v);
    settings_set_db_max((float)max_v);
}

static void drawer_slider_db_max_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    char buf[24];
    snprintf(buf, sizeof(buf), "Max: %d dBm", v);
    if (s_lbl_db_max) lv_label_set_text(s_lbl_db_max, buf);
    int min_v = s_slider_db_min ? (int)lv_slider_get_value(s_slider_db_min) : -130;
    if (v <= min_v) v = min_v + 5;
    ui_set_db_range((float)min_v, (float)v);
    settings_set_db_min((float)min_v);
    settings_set_db_max((float)v);
}

static void drawer_slider_alpha_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    float alpha = (float)v / 100.0f;
    char buf[24];
    snprintf(buf, sizeof(buf), "Alpha: %.2f", (double)alpha);
    if (s_lbl_alpha) lv_label_set_text(s_lbl_alpha, buf);
    render_set_ema_alpha(alpha);
    settings_set_ema_alpha(alpha);
}

static void drawer_slider_cwpitch_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    // Snap to nearest 50 Hz
    int snapped = ((v + 25) / 50) * 50;
    if (snapped < 400) snapped = 400;
    if (snapped > 1000) snapped = 1000;
    ui_set_cw_pitch_hz((uint16_t)snapped);
    char buf[24];
    snprintf(buf, sizeof(buf), "Pitch: %d Hz", snapped);
    if (s_lbl_cwpitch) lv_label_set_text(s_lbl_cwpitch, buf);
}

static void drawer_dropdown_cmap_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint8_t idx = (uint8_t)lv_dropdown_get_selected(dd);
    render_waterfall_set_colormap(idx);
    settings_set_colormap_idx(idx);
}

// ---- Phase 9 (v0.9.5): read-only getters for web JSON ------------------

const char *ui_get_mode_str(void) { return s_current_mode; }
const char *ui_get_band_str(void) { return s_current_band; }
uint32_t ui_get_passband_width_hz(void) { return s_passband_width_hz; }

// Step 4c.1 v0.10: drawer mode toggle.
//
// Tap flips ui_mode between PANADAPTER and FT8. On entry to FT8 we
// respawn ft8_task (it self-deletes on mode-exit, so we re-spawn
// each time the user re-enters FT8 mode). On exit, fft_task drops
// back to its panadapter path (DC blocker + FFT + spectrum push)
// and the waterfall resumes.
//
// 4c.2 will add the LVGL screen switch alongside this.
static void ui_refresh_mode_button_label(void)
{
    if (!s_mode_btn_lbl) return;
    ui_mode_t m = ui_mode_get();
    lv_label_set_text(s_mode_btn_lbl,
                      m == UI_MODE_FT8 ? "Mode: FT8 (tap for panadapter)"
                                       : "Mode: Panadapter (tap for FT8)");
}

static void drawer_mode_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_mode_t cur = ui_mode_get();
    ui_mode_t next = (cur == UI_MODE_FT8) ? UI_MODE_PANADAPTER : UI_MODE_FT8;
    ESP_LOGI(TAG, "Mode toggle: %s -> %s",
             cur  == UI_MODE_FT8 ? "FT8" : "Panadapter",
             next == UI_MODE_FT8 ? "FT8" : "Panadapter");
    ui_mode_set(next);
    // Swap visible widgets. Top/bottom bars stay visible in both modes.
    if (next == UI_MODE_FT8) {
        if (s_spectrum_obj)  lv_obj_add_flag(s_spectrum_obj,  LV_OBJ_FLAG_HIDDEN);
        if (s_label_bar)     lv_obj_add_flag(s_label_bar,     LV_OBJ_FLAG_HIDDEN);
        if (s_waterfall_obj) lv_obj_add_flag(s_waterfall_obj, LV_OBJ_FLAG_HIDDEN);
        ft8_screen_view_show();
        // Respawn the FT8 task; it self-deleted on previous exit.
        ft8_self_test();
    } else {
        ft8_screen_view_hide();
        if (s_spectrum_obj)  lv_obj_clear_flag(s_spectrum_obj,  LV_OBJ_FLAG_HIDDEN);
        if (s_label_bar)     lv_obj_clear_flag(s_label_bar,     LV_OBJ_FLAG_HIDDEN);
        if (s_waterfall_obj) lv_obj_clear_flag(s_waterfall_obj, LV_OBJ_FLAG_HIDDEN);
    }
    ui_refresh_mode_button_label();
    // Close the drawer after toggling. UX nicety, and (4c.1 finding)
    // an open drawer keeps LVGL busy enough to starve audio/fft tasks
    // and stall the next FT8 capture.
    drawer_close();
}
