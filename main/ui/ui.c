#include "ui.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "display.h"

static const char *TAG = "ui";

// Layout constants (1280x720)
#define TOP_BAR_H       60
#define BOTTOM_BAR_H    30
#define SPECTRUM_H      200
#define WATERFALL_H     (DISPLAY_V_RES - TOP_BAR_H - SPECTRUM_H - BOTTOM_BAR_H)  // 430

// Widget handles
static lv_obj_t *s_freq_label = NULL;
static lv_obj_t *s_smeter_label = NULL;
static lv_obj_t *s_mode_label = NULL;
static lv_obj_t *s_spectrum_obj = NULL;
static lv_obj_t *s_waterfall_obj = NULL;
static lv_obj_t *s_status_label = NULL;

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

    // Frequency (big, left-aligned)
    s_freq_label = lv_label_create(bar);
    lv_label_set_text(s_freq_label, "14.074.000 MHz");
    lv_obj_set_style_text_color(s_freq_label, lv_color_hex(0xFFD76B), 0);
    lv_obj_set_style_text_font(s_freq_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_freq_label, LV_ALIGN_LEFT_MID, 8, 0);

    // Mode (center)
    s_mode_label = lv_label_create(bar);
    lv_label_set_text(s_mode_label, "USB");
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_CENTER, -60, 0);

    // S-meter (right of mode)
    s_smeter_label = lv_label_create(bar);
    lv_label_set_text(s_smeter_label, "S9+20");
    lv_obj_set_style_text_color(s_smeter_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_smeter_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_smeter_label, LV_ALIGN_CENTER, 60, 0);

    // Menu button (right)
    lv_obj_t *btn = lv_btn_create(bar);
    lv_obj_set_size(btn, 60, 44);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_t *blbl = lv_label_create(btn);
    lv_label_set_text(blbl, LV_SYMBOL_LIST);
    lv_obj_center(blbl);
}

// ==== Spectrum region (placeholder) ====
// Phase 4 will replace this with real FFT data. For Phase 1 we draw a static
// fake spectrum so we can see the region and measure FPS.
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

    // Placeholder label to confirm rendering
    lv_obj_t *lbl = lv_label_create(s_spectrum_obj);
    lv_label_set_text(lbl, "[ spectrum — Phase 4 will fill this ]");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x707070), 0);
    lv_obj_center(lbl);
}

// ==== Waterfall region (placeholder with test gradient) ====
// Phase 5 will replace this with direct framebuffer writes. For Phase 1 we use
// an LVGL canvas filled with a gradient — this lets us visually confirm the
// region bounds and that LVGL isn't overdrawing it.
static lv_obj_t *s_wf_canvas = NULL;
static uint8_t *s_wf_canvas_buf = NULL;

static void build_waterfall(lv_obj_t *parent)
{
    s_waterfall_obj = lv_obj_create(parent);
    lv_obj_set_size(s_waterfall_obj, DISPLAY_H_RES, WATERFALL_H);
    lv_obj_align(s_waterfall_obj, LV_ALIGN_TOP_LEFT, 0, TOP_BAR_H + SPECTRUM_H);
    lv_obj_set_style_bg_color(s_waterfall_obj, lv_color_hex(0x000010), 0);
    lv_obj_set_style_border_width(s_waterfall_obj, 0, 0);
    lv_obj_set_style_radius(s_waterfall_obj, 0, 0);
    lv_obj_set_style_pad_all(s_waterfall_obj, 0, 0);
    lv_obj_clear_flag(s_waterfall_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Allocate canvas buffer in PSRAM (too big for internal SRAM)
    // 1280 x 430 x 2 bytes = ~1.1 MB
    size_t buf_size = LV_CANVAS_BUF_SIZE(DISPLAY_H_RES, WATERFALL_H, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_wf_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!s_wf_canvas_buf) {
        ESP_LOGE(TAG, "Failed to alloc waterfall canvas (%zu bytes)", buf_size);
        return;
    }

    s_wf_canvas = lv_canvas_create(s_waterfall_obj);
    lv_canvas_set_buffer(s_wf_canvas, s_wf_canvas_buf,
                         DISPLAY_H_RES, WATERFALL_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_wf_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    // Fill with a diagonal gradient to confirm we own this region
    uint16_t *px = (uint16_t *)s_wf_canvas_buf;
    for (int y = 0; y < WATERFALL_H; y++) {
        for (int x = 0; x < DISPLAY_H_RES; x++) {
            uint8_t r = (x * 31) / DISPLAY_H_RES;
            uint8_t g = ((x + y) * 63) / (DISPLAY_H_RES + WATERFALL_H);
            uint8_t b = (y * 31) / WATERFALL_H;
            px[y * DISPLAY_H_RES + x] = (r << 11) | (g << 5) | b;
        }
    }
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

    s_status_label = lv_label_create(bar);
    lv_label_set_text(s_status_label, "Span: 48kHz  Ref: -40dB  Avg: 4  FPS: --");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 4, 0);
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
    build_waterfall(scr);
    build_bottom_bar(scr);

    display_unlock();

    ESP_LOGI(TAG, "UI built: top=%dpx spectrum=%dpx waterfall=%dpx bottom=%dpx",
             TOP_BAR_H, SPECTRUM_H, WATERFALL_H, BOTTOM_BAR_H);
}

void ui_update_frequency(uint32_t freq_hz)
{
    if (!s_freq_label) return;
    char buf[32];
    uint32_t mhz = freq_hz / 1000000;
    uint32_t khz = (freq_hz / 1000) % 1000;
    uint32_t hz  = freq_hz % 1000;
    snprintf(buf, sizeof(buf), "%lu.%03lu.%03lu MHz", mhz, khz, hz);
    if (display_lock(20)) {
        lv_label_set_text(s_freq_label, buf);
        display_unlock();
    }
}

void ui_update_smeter(int s_units)
{
    // Placeholder — Phase 4
}

void ui_push_spectrum(const float *bins, int n_bins)
{
    // Placeholder — Phase 4
}

void ui_push_waterfall_row(const uint8_t *rgb565_row)
{
    // Placeholder — Phase 5
}

// Called by FPS counter to update the bottom bar
void ui_set_fps_text(const char *text)
{
    if (!s_status_label) return;
    if (display_lock(20)) {
        lv_label_set_text(s_status_label, text);
        display_unlock();
    }
}