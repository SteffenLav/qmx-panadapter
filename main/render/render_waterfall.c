#include "render_waterfall.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ui.h"
#include "display.h"
#include <string.h>

static const char *TAG = "render_wf";

// Must match the display canvas width.
#define WF_WIDTH        DISPLAY_H_RES
#define DB_MIN_DISPLAY  -130.0f  /* dBm, matches ui.c spectrum range */
#define DB_MAX_DISPLAY  -30.0f   /* dBm, matches ui.c spectrum range */

// 256-entry RGB565 classic-SDR gradient LUT:
//  black -> dark blue -> blue -> cyan -> green -> yellow -> red
// Built at init from 7 anchor colors with linear interpolation.
static uint16_t s_lut[256];

// One scratch row in DRAM (small, 2.5 KB) - written every tick, read by UI.
static uint8_t *s_row = NULL;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void build_lut(void)
{
    // Anchor points: (index, R, G, B). Indices must be ascending and cover 0..255.
    static const struct { int idx; uint8_t r, g, b; } anchors[] = {
        {   0,   0,   0,   0 },  // black
        {  32,   0,   0,  80 },  // dark blue
        {  80,   0,   0, 255 },  // blue
        { 128,   0, 255, 255 },  // cyan
        { 176,   0, 255,   0 },  // green
        { 224, 255, 255,   0 },  // yellow
        { 255, 255,   0,   0 },  // red
    };
    const int n = sizeof(anchors) / sizeof(anchors[0]);
    for (int seg = 0; seg < n - 1; seg++) {
        int i0 = anchors[seg].idx;
        int i1 = anchors[seg + 1].idx;
        int span = i1 - i0;
        for (int i = i0; i <= i1; i++) {
            float t = (float)(i - i0) / (float)span;
            uint8_t r = (uint8_t)(anchors[seg].r + t * (anchors[seg + 1].r - anchors[seg].r));
            uint8_t g = (uint8_t)(anchors[seg].g + t * (anchors[seg + 1].g - anchors[seg].g));
            uint8_t b = (uint8_t)(anchors[seg].b + t * (anchors[seg + 1].b - anchors[seg].b));
            s_lut[i] = rgb565(r, g, b);
        }
    }
}

esp_err_t render_waterfall_init(void)
{
    build_lut();
    // Row scratch: 1280 px * 2 B = 2560 B. DRAM is fine and faster than PSRAM here.
    s_row = heap_caps_malloc(WF_WIDTH * 2, MALLOC_CAP_8BIT);
    if (!s_row) {
        ESP_LOGE(TAG, "Failed to alloc waterfall row scratch");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Waterfall renderer init (LUT 256 entries, row scratch %d B)",
             WF_WIDTH * 2);
    return ESP_OK;
}

void render_waterfall_tick(const float *spectrum, int n_bins)
{
    if (!s_row || !spectrum || n_bins <= 0) return;

    const float db_span = DB_MAX_DISPLAY - DB_MIN_DISPLAY;
    uint16_t *row = (uint16_t *)s_row;

    // Pixel x -> FFT bin, with fftshift:
    //   x=0       -> bin n_bins/2  (most-negative freq)
    //   x=WF_W/2  -> bin 0         (DC)
    //   x=WF_W-1  -> bin n_bins/2 - 1 (most-positive freq, wraps to n_bins/2 - 1)
    // For 1024 bins on 1280 px we have ~0.8 bins/px (slight oversample, fine for waterfall).
    for (int x = 0; x < WF_WIDTH; x++) {
        // Map x in [0, WF_WIDTH) to bin in [0, n_bins) with fftshift
        int bin_unshifted = (x * n_bins) / WF_WIDTH;       // 0..n_bins-1
        int bin = (bin_unshifted + n_bins / 2) % n_bins;    // fftshift
        // Phase 5.10E: QMX 12 kHz IF offset compensation (match ui_push_spectrum)
        bin = (bin + n_bins / 4) % n_bins;  // shift right by 12 kHz out of 48 kHz = n_bins/4

        float db = spectrum[bin];
        // Clip and scale to 0..255 LUT index
        if (db < DB_MIN_DISPLAY) db = DB_MIN_DISPLAY;
        if (db > DB_MAX_DISPLAY) db = DB_MAX_DISPLAY;
        int idx = (int)(((db - DB_MIN_DISPLAY) / db_span) * 255.0f);
        if (idx < 0) idx = 0;
        if (idx > 255) idx = 255;
        row[x] = s_lut[idx];
    }

    ui_push_waterfall_row(s_row);
}

