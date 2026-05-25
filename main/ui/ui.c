#include "ui.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "cat.h"

static const char *TAG = "ui";

// Layout constants (1280x720)
#define TOP_BAR_H       60
#define BOTTOM_BAR_H    30
#define SPECTRUM_H      200
#define LABEL_BAR_H     18
#define WATERFALL_H     (DISPLAY_V_RES - TOP_BAR_H - SPECTRUM_H - LABEL_BAR_H - BOTTOM_BAR_H)

// Forward declarations (Phase 6.1 - touch-to-tune)
static void touch_event_cb(lv_event_t *e);
static uint32_t s_last_qmx_freq_hz = 0;  // updated by ui_update_frequency

// Touch-target cursor state (Phase 6.1)
static int s_target_x = -1;
static uint64_t s_target_until_us = 0;
#define TARGET_DISPLAY_MS  600
// (s_last_qmx_freq_hz declared at top of file)

// Widget handles
static lv_obj_t *s_freq_label = NULL;
static lv_obj_t *s_smeter_label = NULL;
static lv_obj_t *s_mode_label = NULL;
static lv_obj_t *s_spectrum_obj = NULL;
static lv_obj_t *s_waterfall_obj = NULL;
static lv_obj_t *s_status_label = NULL;

// Phase 5.5: static defaults — manual Ref/Range, user-controlled later
// (internal arbitrary dB scale; ~80=noise floor, ~125=strong signal on test rig)
static float DB_MIN_DISPLAY = -130.0f;  /* dBm, calibrated scale */
static float DB_MAX_DISPLAY = -30.0f;  /* dBm, headroom for S9+40 */

// Forward decl so build_spectrum can call this
static void ui_set_db_labels_internal(float db_min, float db_max);

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

    s_freq_label = lv_label_create(bar);
    lv_label_set_text(s_freq_label, "14.074.000 MHz");
    lv_obj_set_style_text_color(s_freq_label, lv_color_hex(0xFFD76B), 0);
    lv_obj_set_style_text_font(s_freq_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_freq_label, LV_ALIGN_LEFT_MID, 8, 0);

    s_mode_label = lv_label_create(bar);
    lv_label_set_text(s_mode_label, "USB");
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0xA0E0A0), 0);
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_CENTER, -60, 0);

    s_smeter_label = lv_label_create(bar);
    lv_label_set_text(s_smeter_label, "S9+20");
    lv_obj_set_style_text_color(s_smeter_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_smeter_label, &lv_font_montserrat_20, 0);
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

static void build_label_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
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

    // Labels sit below the ticks
    const char *tick_labels[5] = { "-24k", "-12k", "0", "+12k", "+24k" };
    const int tick_xs[5] = { 0, 320, 640, 960, 1280 };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *lbl = lv_label_create(bar);
        lv_label_set_text(lbl, tick_labels[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xA0A0A0), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        if (i == 0) {
            lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 2, 0);
        } else if (i == 4) {
            lv_obj_align(lbl, LV_ALIGN_BOTTOM_RIGHT, -2, 0);
        } else {
            lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, tick_xs[i] - 14, 0);
        }
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
    build_label_bar(scr);
    build_waterfall(scr);
    build_bottom_bar(scr);

    display_unlock();

    ESP_LOGI(TAG, "UI built: top=%dpx spectrum=%dpx labels=%dpx waterfall=%dpx bottom=%dpx",
             TOP_BAR_H, SPECTRUM_H, LABEL_BAR_H, WATERFALL_H, BOTTOM_BAR_H);
}

// Phase 5.10: forward declaration for band_from_freq (defined below)
static const char *band_from_freq(uint32_t freq_hz);

void ui_update_frequency(uint32_t freq_hz)
{
    s_last_qmx_freq_hz = freq_hz;
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
    // Phase 5.10: derive band and push to UI
    const char *band = band_from_freq(freq_hz);
    if (band) ui_update_band(band);
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
    if (!s_mode_label || !mode) return;
    if (display_lock(100)) {
        lv_label_set_text(s_mode_label, mode);
        lv_obj_invalidate(s_mode_label);
        display_unlock();
    } else {
        ESP_LOGW("ui", "ui_update_mode: display_lock timeout for '%s'", mode);
    }
}

void ui_update_band(const char *band)
{
    if (!s_smeter_label || !band) return;
    if (display_lock(100)) {
        lv_label_set_text(s_smeter_label, band);
        lv_obj_invalidate(s_smeter_label);
        display_unlock();
    } else {
        ESP_LOGW("ui", "ui_update_band: display_lock timeout for '%s'", band);
    }
}

void ui_update_smeter(int s_units)
{
    // Placeholder
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

        // Compute target frequency from final touch position
        int dx = (int)p.x - DISPLAY_H_RES / 2;
        int32_t offset_hz = (int32_t)((int64_t)dx * UAC_SAMPLE_RATE / DISPLAY_H_RES);
        int32_t rounded = (offset_hz + (offset_hz >= 0 ? TUNE_ROUND_HZ/2 : -TUNE_ROUND_HZ/2))
                          / TUNE_ROUND_HZ * TUNE_ROUND_HZ;
        int64_t target = (int64_t)s_last_qmx_freq_hz + rounded;
        if (target < 0) return;
        uint32_t target_hz = (uint32_t)target;

        esp_err_t err = cat_set_frequency(target_hz);
        ESP_LOGI("ui_touch", "RELEASED x=%d dx=%d off=%ld tgt=%lu err=0x%x",
                 (int)p.x, dx, (long)rounded, (unsigned long)target_hz, err);
        // Let the cursor linger briefly after release, then clear
        s_target_until_us = esp_timer_get_time() + 200000;
    }
}

// Hook into ui_update_frequency to track latest known QMX frequency
















