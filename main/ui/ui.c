#include "ui.h"
#include "render.h"

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
#include "iq_balance.h"

static const char *TAG = "ui";

// Layout constants (1280x720)
#define TOP_BAR_H       60
#define DRAWER_W        400  /* Phase 5.10D Stage 2: settings drawer width */
#define BOTTOM_BAR_H    30
#define SPECTRUM_H      200
#define LABEL_BAR_H     32  /* Phase 5.10C: room for Montserrat 18 labels under tick marks */
// Phase 5.10E: QMX I/Q has a 12 kHz IF offset Ã¢â‚¬â€ the signal at the QMX's
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
static uint32_t s_passband_width_hz = 0;  // Phase 5.10G: 0 = use mode default; else from CAT FW

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
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_burger_btn = NULL;  // Phase 5.10I: kept for foreground move after all UI built
static lv_obj_t *s_switch_iq  = NULL;  // Phase B: IQ balance toggle in settings drawer

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
static void drawer_preset_normal_cb(lv_event_t *e);
static void drawer_preset_dx_cb(lv_event_t *e);
static void drawer_preset_strong_cb(lv_event_t *e);
static void drawer_slider_db_min_cb(lv_event_t *e);
static void drawer_slider_db_max_cb(lv_event_t *e);
static void drawer_slider_alpha_cb(lv_event_t *e);
static void drawer_apply_preset(int db_min, int db_max, float alpha);
static void drawer_build(void);
static void drawer_open(void);
static void drawer_close(void);
static void drawer_close_button_cb(lv_event_t *e);
static void iq_balance_toggle_cb(lv_event_t *e);
static void drawer_anim_y_cb(void *obj, int32_t v);

// Phase 5.5: static defaults Ã¢â‚¬â€ manual Ref/Range, user-controlled later
// (internal arbitrary dB scale; ~80=noise floor, ~125=strong signal on test rig)
static float DB_MIN_DISPLAY = -130.0f;  /* dBm, calibrated scale */
static float DB_MAX_DISPLAY = -30.0f;  /* dBm, headroom for S9+40 */


static lv_obj_t *s_wf_canvas = NULL;
static uint8_t *s_wf_canvas_buf = NULL;
static lv_draw_buf_t s_wf_draw_buf;

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
    lv_obj_set_size(bar, TOP_BAR_H, DISPLAY_H_RES);  // 60 wide × 1280 tall (portrait)
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Phase 5.10D: top-bar layout Ã¢â‚¬â€ Band | Mode | [center: Freq] | S-meter
    // Labels rotated 900 (90° CW); landscape_x → portrait_y = 1279 - landscape_x.
    // Band: landscape x≈8 → portrait y=1271, offset from center=631
    s_band_label = lv_label_create(bar);
    lv_label_set_text(s_band_label, "Band: ---");
    lv_obj_set_style_text_color(s_band_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_band_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_transform_rotation(s_band_label, 900, 0);
    lv_obj_align(s_band_label, LV_ALIGN_CENTER, 0, 631);

    // Mode: landscape x≈200 → portrait y=1079, offset=439
    s_mode_label = lv_label_create(bar);
    lv_label_set_text(s_mode_label, "Mode: USB");
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_transform_rotation(s_mode_label, 900, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_CENTER, 0, 439);

    // Freq: landscape x=640 → portrait y=639 (center of bar)
    s_freq_label = lv_label_create(bar);
    lv_label_set_text(s_freq_label, "Center Freq: 14.074.000 Hz");
    lv_obj_set_style_text_color(s_freq_label, lv_color_hex(0xFFD76B), 0);
    lv_obj_set_style_text_font(s_freq_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_transform_rotation(s_freq_label, 900, 0);
    lv_obj_align(s_freq_label, LV_ALIGN_CENTER, 0, 0);

    // S-meter: landscape x≈960 → portrait y=319, offset=-321
    s_smeter_label = lv_label_create(bar);
    lv_label_set_text(s_smeter_label, "Signal: S0");
    lv_obj_set_style_text_color(s_smeter_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(s_smeter_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_transform_rotation(s_smeter_label, 900, 0);
    lv_obj_align(s_smeter_label, LV_ALIGN_CENTER, 0, -321);

    // Burger: 80×80 at portrait (px=10, py=3) = landscape top-right.
    s_burger_btn = lv_btn_create(parent);
    lv_obj_set_size(s_burger_btn, 80, 80);
    lv_obj_set_pos(s_burger_btn, 10, 3);
    lv_obj_add_event_cb(s_burger_btn, settings_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *blbl = lv_label_create(s_burger_btn);
    lv_label_set_text(blbl, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(blbl, &lv_font_montserrat_32, 0);
    lv_obj_center(blbl);
}

// ==== Spectrum region (Phase 5.1: real-time line graph) ====
static void build_spectrum(lv_obj_t *parent)
{
    // Phase 6.3: portrait (SPECTRUM_H wide × DISPLAY_H_RES tall) at portrait x=TOP_BAR_H
    s_spectrum_obj = lv_obj_create(parent);
    lv_obj_set_size(s_spectrum_obj, SPECTRUM_H, DISPLAY_H_RES);
    lv_obj_set_pos(s_spectrum_obj, TOP_BAR_H, 0);
    lv_obj_set_style_bg_color(s_spectrum_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(s_spectrum_obj, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(s_spectrum_obj, 1, 0);
    lv_obj_set_style_radius(s_spectrum_obj, 0, 0);
    lv_obj_set_style_pad_all(s_spectrum_obj, 0, 0);
    lv_obj_clear_flag(s_spectrum_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 200 × 1280 × 2 = 512 KB in PSRAM (same total, dimensions swapped)
    size_t buf_size = LV_CANVAS_BUF_SIZE(SPECTRUM_H, DISPLAY_H_RES, 16,
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
                         SPECTRUM_H, DISPLAY_H_RES, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_spec_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    // Phase 5.4: dB range labels (top-left and bottom-left of spectrum)
    // dB labels: portrait x=0 (left) = high dB, x=SPECTRUM_H-1 (right) = low dB.
    // db_max at landscape top (portrait left=x=0→near TOP_LEFT), db_min at portrait RIGHT.
    s_db_max_label = lv_label_create(s_spectrum_obj);
    lv_label_set_text(s_db_max_label, "");
    lv_obj_set_style_text_color(s_db_max_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_db_max_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_db_max_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_db_max_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_db_max_label, 3, 0);
    lv_obj_set_style_transform_rotation(s_db_max_label, 900, 0);
    lv_obj_align(s_db_max_label, LV_ALIGN_TOP_LEFT, 2, 4);

    s_db_min_label = lv_label_create(s_spectrum_obj);
    lv_label_set_text(s_db_min_label, "");
    lv_obj_set_style_text_color(s_db_min_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_db_min_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_db_min_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_db_min_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_db_min_label, 3, 0);
    lv_obj_set_style_transform_rotation(s_db_min_label, 900, 0);
    lv_obj_align(s_db_min_label, LV_ALIGN_TOP_RIGHT, -2, 4);

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
    // Phase 6.3: portrait — bar is LABEL_BAR_H wide × DISPLAY_H_RES tall at portrait x=260
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LABEL_BAR_H, DISPLAY_H_RES);
    lv_obj_set_pos(bar, TOP_BAR_H + SPECTRUM_H, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Tick canvas: LABEL_BAR_H wide × DISPLAY_H_RES tall (portrait).
    // landscape_x → portrait_y = (DISPLAY_H_RES-1) - landscape_x.
    // Ticks are horizontal lines at specific portrait_y positions.
    size_t buf_size = LV_CANVAS_BUF_SIZE(LABEL_BAR_H, DISPLAY_H_RES, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_label_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (s_label_canvas_buf) {
        memset(s_label_canvas_buf, 0, buf_size);
        uint16_t *px = (uint16_t *)s_label_canvas_buf;
        const uint16_t major_color = 0xC618;
        const uint16_t minor_color = 0x8410;
        const int center_x = DISPLAY_H_RES / 2;
        const float px_per_khz = (float)DISPLAY_H_RES / 48.0f;
        for (int khz = -24; khz <= 24; khz += 3) {
            int lx = center_x + (int)(khz * px_per_khz);  // landscape x
            if (lx < 0 || lx >= DISPLAY_H_RES) continue;
            int py = (DISPLAY_H_RES - 1) - lx;  // portrait y
            if (py < 0 || py >= DISPLAY_H_RES) continue;
            int is_major = (khz % 12 == 0);
            int h = is_major ? 10 : 5;
            uint16_t color = is_major ? major_color : minor_color;
            // Horizontal tick: fill portrait x=0..h-1 at portrait y=py
            for (int dx = 0; dx < h; dx++) {
                px[py * LABEL_BAR_H + dx] = color;
            }
        }
        s_label_canvas = lv_canvas_create(bar);
        lv_canvas_set_buffer(s_label_canvas, s_label_canvas_buf,
                             LABEL_BAR_H, DISPLAY_H_RES, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_label_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    // Tick labels: rotated 900. landscape_x → portrait_y = 1279 - landscape_x.
    // Offset from portrait bar center (DISPLAY_H_RES/2=640).
    const int tick_xs[5] = { 0, 320, 640, 960, 1279 };
    for (int i = 0; i < 5; i++) {
        s_tick_labels[i] = lv_label_create(bar);
        lv_label_set_text(s_tick_labels[i], "--.---");
        lv_obj_set_style_text_color(s_tick_labels[i], lv_color_hex(0xA0A0A0), 0);
        lv_obj_set_style_text_font(s_tick_labels[i], &lv_font_montserrat_18, 0);
        lv_obj_set_style_transform_rotation(s_tick_labels[i], 900, 0);
        int py = (DISPLAY_H_RES - 1) - tick_xs[i];
        lv_obj_align(s_tick_labels[i], LV_ALIGN_CENTER, 0, py - DISPLAY_H_RES / 2);
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
// Phase 6.3: portrait waterfall double-buffer using custom-stride lv_draw_buf_t.
// Physical buffer: WATERFALL_H*2 wide × DISPLAY_H_RES tall (row-major).
// Viewing window: WATERFALL_H wide × DISPLAY_H_RES tall, starting at col s_wf_head.
// Write each new column at physical_col = s_wf_head AND s_wf_head+WATERFALL_H.
// lv_draw_buf stride = WATERFALL_H*2*2 bytes allows the window pointer to slide.
static void build_waterfall(lv_obj_t *parent)
{
    s_waterfall_obj = lv_obj_create(parent);
    lv_obj_set_size(s_waterfall_obj, WATERFALL_H, DISPLAY_H_RES);
    lv_obj_set_pos(s_waterfall_obj, TOP_BAR_H + SPECTRUM_H + LABEL_BAR_H, 0);
    lv_obj_set_style_bg_color(s_waterfall_obj, lv_color_hex(0x000010), 0);
    lv_obj_set_style_border_width(s_waterfall_obj, 0, 0);
    lv_obj_set_style_radius(s_waterfall_obj, 0, 0);
    lv_obj_set_style_pad_all(s_waterfall_obj, 0, 0);
    lv_obj_clear_flag(s_waterfall_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Physical buffer: WATERFALL_H*2 cols × DISPLAY_H_RES rows (row-major).
    size_t phys_stride_px = (size_t)(WATERFALL_H * 2);
    size_t buf_bytes = phys_stride_px * DISPLAY_H_RES * 2;
    s_wf_canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!s_wf_canvas_buf) {
        ESP_LOGE(TAG, "Failed to alloc waterfall canvas (%zu bytes)", buf_bytes);
        return;
    }
    memset(s_wf_canvas_buf, 0, buf_bytes);

    // Initialise draw_buf with custom stride so we can slide the view window.
    uint32_t stride_bytes = (uint32_t)(phys_stride_px * 2);
    lv_draw_buf_init(&s_wf_draw_buf, WATERFALL_H, DISPLAY_H_RES,
                     LV_COLOR_FORMAT_RGB565, stride_bytes,
                     s_wf_canvas_buf, (uint32_t)buf_bytes);

    s_wf_canvas = lv_canvas_create(s_waterfall_obj);
    lv_obj_add_flag(s_waterfall_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_waterfall_obj, touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_waterfall_obj, touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_canvas_set_draw_buf(s_wf_canvas, &s_wf_draw_buf);
    lv_obj_align(s_wf_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_invalidate(s_wf_canvas);
}

// ==== Bottom status bar ====
static void build_bottom_bar(lv_obj_t *parent)
{
    // Phase 6.3: portrait — bottom bar is BOTTOM_BAR_H wide × DISPLAY_H_RES tall at portrait x=690
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, BOTTOM_BAR_H, DISPLAY_H_RES);
    lv_obj_set_pos(bar, TOP_BAR_H + SPECTRUM_H + LABEL_BAR_H + WATERFALL_H, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A1014), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(bar);
    lv_label_set_text(s_status_label, "Span: 48kHz  Ref: -40dB  Avg: 4  FPS: --");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_transform_rotation(s_status_label, 900, 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);
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

    display_unlock();

    // Phase 5.10I: ensure the oversized burger sits on top of everything
    if (s_burger_btn) lv_obj_move_foreground(s_burger_btn);

    // Hidden 80x80 long-press screenshot region in top-left
    screenshot_init(scr);
    ESP_LOGI(TAG, "UI built: top=%dpx spectrum=%dpx labels=%dpx waterfall=%dpx bottom=%dpx",
             TOP_BAR_H, SPECTRUM_H, LABEL_BAR_H, WATERFALL_H, BOTTOM_BAR_H);
}

// Phase 5.10: forward declaration for band_from_freq (defined below)
static const char *band_from_freq(uint32_t freq_hz);
static void update_freq_axis_labels(uint32_t center_hz);  // Phase 5.10C

void ui_update_frequency(uint32_t freq_hz)
{
    s_last_qmx_freq_hz = freq_hz;
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
    } else if (strstr(s_current_mode, "FSK") || strstr(s_current_mode, "RTTY")
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

// Phase 6.3: portrait spectrum canvas is SPECTRUM_H wide × DISPLAY_H_RES tall.
// Buffer layout: px[portrait_y * SPECTRUM_H + portrait_x]
//   portrait_x = 0..SPECTRUM_H-1: left = high dB (landscape top), right = low dB
//   portrait_y = 0..DISPLAY_H_RES-1: maps to landscape_x = DISPLAY_H_RES-1-portrait_y
// Cursor lines are horizontal (constant portrait_y) rather than vertical.
void ui_push_spectrum(const float *bins, int n_bins)
{
    if (!s_spec_canvas_buf || !bins || n_bins <= 0) return;
    if (!display_lock(20)) return;

    uint16_t *px = (uint16_t *)s_spec_canvas_buf;
    static int s_prev_x_top = 0;
    const uint16_t fg = 0x07E0;
    const uint16_t fg_dim = 0x01C0;
    const uint16_t grid_color = 0x4208;

    memset(px, 0, (size_t)SPECTRUM_H * DISPLAY_H_RES * 2);

    // dB grid lines: vertical bands at constant portrait_x = db_to_y(dB)
    {
        const float grid_dbs[5] = { -120.0f, -100.0f, -80.0f, -60.0f, -40.0f };
        for (int g = 0; g < 5; g++) {
            int gx = db_to_y(grid_dbs[g]);  // portrait x
            if (gx < 0 || gx >= SPECTRUM_H) continue;
            for (int py = 0; py < DISPLAY_H_RES; py++)
                px[py * SPECTRUM_H + gx] = grid_color;
        }
    }

    int N = n_bins;
    int half = N / 2;

    // Iterate over portrait_y (= landscape_x in reverse)
    for (int py = 0; py < DISPLAY_H_RES; py++) {
        // landscape_x = (DISPLAY_H_RES-1) - py
        int lx = (DISPLAY_H_RES - 1) - py;
        int shifted = (int)((float)lx * (float)N / (float)DISPLAY_H_RES);
        if (shifted < 0) shifted = 0;
        if (shifted >= N) shifted = N - 1;

        int bin = (shifted < half) ? shifted + half : shifted - half;
        bin = (bin + (IF_OFFSET_HZ * N) / 48000) % N;
        if (bin < 0) bin += N;

        int x_top = db_to_y(bins[bin]);  // portrait x of signal peak
        if (x_top < 0) x_top = 0;
        if (x_top >= SPECTRUM_H) x_top = SPECTRUM_H - 1;

        // Connect prev_x to x_top horizontally
        int x_a = (py > 0) ? s_prev_x_top : x_top;
        int x_b = x_top;
        int x_lo = (x_a < x_b) ? x_a : x_b;
        int x_hi = (x_a > x_b) ? x_a : x_b;
        for (int px_x = x_lo; px_x <= x_hi; px_x++)
            px[py * SPECTRUM_H + px_x] = fg;
        // Dim fill to the right (low dB direction)
        for (int px_x = x_hi + 1; px_x < SPECTRUM_H; px_x++)
            px[py * SPECTRUM_H + px_x] = fg_dim;
        s_prev_x_top = x_top;
    }

    // Cursor lines: horizontal at portrait_y = (DISPLAY_H_RES-1) - landscape_x
    {
        int32_t pb_low_hz, pb_high_hz;
        compute_passband_edges_hz(&pb_low_hz, &pb_high_hz);
        const uint16_t pb_color = 0x8410;
        for (int side = 0; side < 2; side++) {
            int32_t edge_hz = (side == 0) ? pb_low_hz : pb_high_hz;
            int edge_lx = DISPLAY_H_RES / 2 + (int)((int64_t)edge_hz * DISPLAY_H_RES / 48000);
            if (edge_lx < 0 || edge_lx >= DISPLAY_H_RES) continue;
            int edge_py = (DISPLAY_H_RES - 1) - edge_lx;
            if (edge_py >= 0 && edge_py < DISPLAY_H_RES) {
                for (int px_x = 0; px_x < SPECTRUM_H; px_x++)
                    px[edge_py * SPECTRUM_H + px_x] = pb_color;
            }
            if (edge_py - 1 >= 0) {
                for (int px_x = 0; px_x < SPECTRUM_H; px_x++)
                    px[(edge_py - 1) * SPECTRUM_H + px_x] = pb_color;
            }
        }
        // Center cursor (amber): at landscape_x=DISPLAY_H_RES/2 → portrait_y=639
        const uint16_t center_color = 0xFD00;
        int center_py = (DISPLAY_H_RES - 1) - DISPLAY_H_RES / 2;
        for (int px_x = 0; px_x < SPECTRUM_H; px_x++)
            px[center_py * SPECTRUM_H + px_x] = center_color;
    }
    // Target cursor (cyan): at portrait_y = s_target_x (s_target_x stores landscape_x)
    if (s_target_x >= 0) {
        uint64_t now = esp_timer_get_time();
        if (now < s_target_until_us) {
            const uint16_t target_color = 0x07FF;
            int tpy = (DISPLAY_H_RES - 1) - s_target_x;
            if (tpy >= 0 && tpy < DISPLAY_H_RES) {
                for (int px_x = 0; px_x < SPECTRUM_H; px_x++)
                    px[tpy * SPECTRUM_H + px_x] = target_color;
            }
        } else {
            s_target_x = -1;
        }
    }
    lv_obj_invalidate(s_spec_canvas);
    display_unlock();
}

// Phase 6.3 portrait waterfall double-buffer (column-based).
// Physical buffer: WATERFALL_H*2 wide × DISPLAY_H_RES tall (row-major).
// Each tick: decrement s_wf_head, write new column at s_wf_head AND s_wf_head+WATERFALL_H.
// Update lv_draw_buf data pointer to buf + s_wf_head*2 bytes; stride = WATERFALL_H*2*2.
// Newest column at portrait x=0 of canvas = landscape top of waterfall.
static int s_wf_head = 0;

void ui_push_waterfall_row(const uint8_t *rgb565_col)
{
    if (!s_wf_canvas_buf || !rgb565_col) return;
    if (!display_lock(20)) return;

    // Decrement head so the new column lands at portrait x=0 of the view.
    s_wf_head = (s_wf_head + WATERFALL_H - 1) % WATERFALL_H;

    // Write 1280 pixels into column s_wf_head and s_wf_head+WATERFALL_H.
    // Physical pixel at (col c, row r) = buf16[r * WATERFALL_H*2 + c].
    uint16_t *buf16 = (uint16_t *)s_wf_canvas_buf;
    const uint16_t *col_data = (const uint16_t *)rgb565_col;
    const int phys_w = WATERFALL_H * 2;
    for (int r = 0; r < DISPLAY_H_RES; r++) {
        buf16[r * phys_w + s_wf_head]              = col_data[r];
        buf16[r * phys_w + s_wf_head + WATERFALL_H] = col_data[r];
    }

    // Slide the draw_buf window to start at column s_wf_head.
    s_wf_draw_buf.data = s_wf_canvas_buf + s_wf_head * 2;
    lv_canvas_set_draw_buf(s_wf_canvas, &s_wf_draw_buf);
    lv_obj_invalidate(s_wf_canvas);
    display_unlock();
}

void ui_set_fps_text(const char *text)
{
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

    // Phase 6.3: portrait coords. Frequency axis = portrait y (= landscape x).
    // landscape_x = (DISPLAY_H_RES-1) - p.y
    if (p.y < 0 || p.y >= DISPLAY_H_RES) return;
    int landscape_x = (DISPLAY_H_RES - 1) - (int)p.y;

    if (code == LV_EVENT_PRESSING) {
        s_target_x = landscape_x;  // stored as landscape_x for cursor rendering
        s_target_until_us = esp_timer_get_time() + 200000;
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (s_last_qmx_freq_hz == 0) return;
        // Burger deadzone: landscape (x>=1080, y<120)
        // In portrait: landscape_x = (DISPLAY_H_RES-1)-p.y >= 1080 → p.y <= 199
        //              landscape_y = p.x < 120 (p.x is portrait x)
        if (landscape_x >= 1080 && (int)p.x < 120) {
            ESP_LOGI("ui_touch", "RELEASED in burger deadzone (lx=%d px=%d) - ignored", landscape_x, (int)p.x);
            return;
        }

        // Screenshot deadzone: landscape (x<80, y<80) → portrait_y>1199 && portrait_x<80
        if (landscape_x < 80 && (int)p.x < 80) {
            ESP_LOGI("ui_touch", "RELEASED in screenshot deadzone - ignored");
            return;
        }

        // Compute target frequency from final touch position
        int dx = landscape_x - DISPLAY_H_RES / 2;
        int32_t offset_hz = (int32_t)((int64_t)dx * UAC_SAMPLE_RATE / DISPLAY_H_RES);
        // Phase 5.10F: mode-aware snap. CW=10 Hz (precision), SSB=500 Hz
        // (voice channels), FT8/data=100 Hz, AM/FM=1 kHz.
        int32_t snap = 10;
        if (strstr(s_current_mode, "USB") || strstr(s_current_mode, "LSB")) snap = 500;
        else if (strstr(s_current_mode, "FT") || strstr(s_current_mode, "DIG") || strstr(s_current_mode, "RTTY")
                 || strstr(s_current_mode, "FSK")) snap = 100;
        else if (strstr(s_current_mode, "AM") || strstr(s_current_mode, "FM")) snap = 1000;
        else if (strstr(s_current_mode, "CW")) snap = 10;
        int32_t rounded = (offset_hz + (offset_hz >= 0 ? snap/2 : -snap/2)) / snap * snap;
        int64_t target = (int64_t)s_last_qmx_freq_hz + rounded;
        if (target < 0) return;
        uint32_t target_hz = (uint32_t)target;

        esp_err_t err = cat_set_frequency(target_hz);
        ESP_LOGI("ui_touch", "RELEASED lx=%d dx=%d off=%ld tgt=%lu err=0x%x",
                 landscape_x, dx, (long)rounded, (unsigned long)target_hz, err);
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

// Hook into ui_update_frequency to track latest known QMX frequency


















// Phase 5.10D Stage 2: burger menu click Ã¢â‚¬â€ toggle settings drawer
static void settings_button_cb(lv_event_t *e)
{
    (void)e;
    if (s_drawer_open) {
        drawer_close();
    } else {
        drawer_open();
    }
}

// Phase 6.3: drawer slides in from portrait top (= landscape right).
static void drawer_anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
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
    if (on) iq_balance_reset();
}

// Build the drawer once. Hidden off-screen on the right initially.
static void drawer_build(void)
{
    if (s_drawer) return;

    lv_obj_t *scr = lv_screen_active();
    // Phase 6.3: portrait drawer — DISPLAY_V_RES wide × DRAWER_W tall.
    // Landscape: drawer slides in from the right (x-animation).
    // Portrait: slides in from portrait top (= landscape right); y-animation.
    // Parked off-screen above: y = -DRAWER_W.
    s_drawer = lv_obj_create(scr);
    lv_obj_set_size(s_drawer, DISPLAY_V_RES, DRAWER_W);  // 720 × 400
    lv_obj_set_pos(s_drawer, 0, -DRAWER_W);
    lv_obj_set_style_bg_color(s_drawer, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_drawer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_drawer, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(s_drawer, 1, 0);
    lv_obj_set_style_border_side(s_drawer, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(s_drawer, 0, 0);
    lv_obj_set_style_pad_all(s_drawer, 16, 0);
    lv_obj_add_flag(s_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_drawer, LV_DIR_VER);  // portrait y scrolling = landscape horizontal

    // Phase 6.3 portrait: title rotated 900, close button at portrait top-left
    // (= landscape bottom-left of drawer, which appears at landscape-right side).
    lv_obj_t *title = lv_label_create(s_drawer);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_transform_rotation(title, 900, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *close_btn = lv_btn_create(s_drawer);
    lv_obj_set_size(close_btn, 80, 80);
    lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(close_btn, drawer_close_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_32, 0);  // Phase 5.10I: matching the burger
    lv_obj_center(close_lbl);

    // === Phase 5.10D Stage 2b: presets + sliders ===
    int y = 80;

    // Phase B: IQ balance on/off toggle
    {
        lv_obj_t *iq_lbl = lv_label_create(s_drawer);
        lv_label_set_text(iq_lbl, "IQ Balance");
        lv_obj_set_style_text_color(iq_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(iq_lbl, &lv_font_montserrat_18, 0);
        lv_obj_align(iq_lbl, LV_ALIGN_TOP_LEFT, 0, y + 6);
        s_switch_iq = lv_switch_create(s_drawer);
        lv_obj_set_size(s_switch_iq, 60, 32);
        lv_obj_align(s_switch_iq, LV_ALIGN_TOP_RIGHT, 0, y);
        if (iq_balance_is_enabled()) lv_obj_add_state(s_switch_iq, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_switch_iq, iq_balance_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        y += 48;
    }

    // Presets section header
    lv_obj_t *presets_hdr = lv_label_create(s_drawer);
    lv_label_set_text(presets_hdr, "Presets");
    lv_obj_set_style_text_color(presets_hdr, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(presets_hdr, &lv_font_montserrat_18, 0);
    lv_obj_align(presets_hdr, LV_ALIGN_TOP_LEFT, 0, y);
    y += 40;

    // Three preset buttons, stacked
    const char *preset_names[3] = { "HF Normal", "HF DX", "Strong Sig." };
    lv_event_cb_t preset_cbs[3] = {
        drawer_preset_normal_cb,
        drawer_preset_dx_cb,
        drawer_preset_strong_cb,
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_btn_create(s_drawer);
        lv_obj_set_size(btn, DRAWER_W - 32, 60);  /* Phase 5.10D Stage 2b polish: bigger touch target */
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_add_event_cb(btn, preset_cbs[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, preset_names[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_center(lbl);
        y += 64;
    }

    y += 8;
    // dB Range section header
    lv_obj_t *db_hdr = lv_label_create(s_drawer);
    lv_label_set_text(db_hdr, "dB Range");
    lv_obj_set_style_text_color(db_hdr, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(db_hdr, &lv_font_montserrat_18, 0);
    lv_obj_align(db_hdr, LV_ALIGN_TOP_LEFT, 0, y);
    y += 40;

    // dB min slider
    s_lbl_db_min = lv_label_create(s_drawer);
    lv_label_set_text(s_lbl_db_min, "Min: -130 dBm");
    lv_obj_set_style_text_color(s_lbl_db_min, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_db_min, &lv_font_montserrat_18, 0);
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
    lv_obj_set_style_text_font(s_lbl_db_max, &lv_font_montserrat_18, 0);
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
    lv_obj_set_style_text_font(sm_hdr, &lv_font_montserrat_18, 0);
    lv_obj_align(sm_hdr, LV_ALIGN_TOP_LEFT, 0, y);
    y += 40;

    s_lbl_alpha = lv_label_create(s_drawer);
    lv_label_set_text(s_lbl_alpha, "Alpha: 0.40");
    lv_obj_set_style_text_color(s_lbl_alpha, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_alpha, &lv_font_montserrat_18, 0);
    lv_obj_align(s_lbl_alpha, LV_ALIGN_TOP_LEFT, 0, y);
    y += 30;

    s_slider_alpha = lv_slider_create(s_drawer);
    lv_obj_set_size(s_slider_alpha, DRAWER_W - 32, 30);
    lv_slider_set_range(s_slider_alpha, 5, 100);   // = alpha 0.05..1.00
    lv_slider_set_value(s_slider_alpha, 40, LV_ANIM_OFF);
    lv_obj_align(s_slider_alpha, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_add_event_cb(s_slider_alpha, drawer_slider_alpha_cb, LV_EVENT_VALUE_CHANGED, NULL);

    ESP_LOGI(TAG, "Settings drawer built (off-screen at y=%d)", -DRAWER_W);
}

// Phase 6.3: drawer slides in from portrait top (y: -DRAWER_W → 0).
static void drawer_open(void)
{
    drawer_build();
    if (!s_drawer || s_drawer_open) return;
    lv_obj_move_foreground(s_drawer);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_drawer);
    lv_anim_set_exec_cb(&a, drawer_anim_y_cb);
    lv_anim_set_values(&a, -DRAWER_W, 0);
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
    lv_anim_set_exec_cb(&a, drawer_anim_y_cb);
    lv_anim_set_values(&a, 0, -DRAWER_W);
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
}

static void drawer_preset_normal_cb(lv_event_t *e)  { (void)e; drawer_apply_preset(-130, -30, 0.40f); }
static void drawer_preset_dx_cb(lv_event_t *e)      { (void)e; drawer_apply_preset(-130, -50, 0.60f); }
static void drawer_preset_strong_cb(lv_event_t *e)  { (void)e; drawer_apply_preset(-110, -20, 0.20f); }

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
}
