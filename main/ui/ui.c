#include "ui.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "display.h"

static const char *TAG = "ui";

// Layout constants (1280x720)
#define TOP_BAR_H       60
#define BOTTOM_BAR_H    30
#define SPECTRUM_H      200
#define WATERFALL_H     (DISPLAY_V_RES - TOP_BAR_H - SPECTRUM_H - BOTTOM_BAR_H)

// Widget handles
static lv_obj_t *s_freq_label = NULL;
static lv_obj_t *s_smeter_label = NULL;
static lv_obj_t *s_mode_label = NULL;
static lv_obj_t *s_spectrum_obj = NULL;
static lv_obj_t *s_waterfall_obj = NULL;
static lv_obj_t *s_status_label = NULL;

static lv_obj_t *s_wf_canvas = NULL;
static uint8_t *s_wf_canvas_buf = NULL;

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

    s_freq_label = lv_label_create(bar);
    lv_label_set_text(s_freq_label, "14.074.000 MHz");
    lv_obj_set_style_text_color(s_freq_label, lv_color_hex(0xFFD76B), 0);
    lv_obj_set_style_text_font(s_freq_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_freq_label, LV_ALIGN_LEFT_MID, 8, 0);

    s_mode_label = lv_label_create(bar);
    lv_label_set_text(s_mode_label, "USB");
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_CENTER, -60, 0);

    s_smeter_label = lv_label_create(bar);
    lv_label_set_text(s_smeter_label, "S9+20");
    lv_obj_set_style_text_color(s_smeter_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_smeter_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_smeter_label, LV_ALIGN_CENTER, 60, 0);

    lv_obj_t *btn = lv_btn_create(bar);
    lv_obj_set_size(btn, 60, 44);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_t *blbl = lv_label_create(btn);
    lv_label_set_text(blbl, LV_SYMBOL_LIST);
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
    lv_canvas_set_buffer(s_spec_canvas, s_spec_canvas_buf,
                         DISPLAY_H_RES, SPECTRUM_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_spec_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
}

// ==== Waterfall region (placeholder gradient from Phase 1) ====
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
    // Placeholder
}

#define DB_MIN_DISPLAY   40.0f
#define DB_MAX_DISPLAY  130.0f

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

void ui_push_spectrum(const float *bins, int n_bins)
{
    if (!s_spec_canvas_buf || !bins || n_bins <= 0) return;
    if (!display_lock(20)) return;

    uint16_t *px = (uint16_t *)s_spec_canvas_buf;
    const uint16_t fg = 0x07E0;  // green in RGB565

    // Clear canvas to black
    memset(px, 0, (size_t)DISPLAY_H_RES * SPECTRUM_H * 2);

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

        int y_top = db_to_y(bins[bin]);
        for (int y = y_top; y < SPECTRUM_H; y++) {
            px[y * DISPLAY_H_RES + x] = fg;
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

void ui_set_fps_text(const char *text)
{
    if (!s_status_label) return;
    if (display_lock(20)) {
        lv_label_set_text(s_status_label, text);
        display_unlock();
    }
}








