#include "render_waterfall.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "ui.h"
#include "display.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "render_wf";

// Must match the display canvas width.
#define WF_WIDTH        DISPLAY_H_RES
// Phase 5.10F: waterfall floor color now auto-tracks the running
// noise-floor estimate (median of the spectrum) so the bottom of the
// color LUT always sits at "current noise level" regardless of band
// conditions. Spectrum trace remains user-controlled via the drawer.
#define DB_MIN_INITIAL  -130.0f  /* dBm, starting floor */
static float DB_MIN_DISPLAY = DB_MIN_INITIAL;  /* updated from noise floor */
#define DB_MAX_DISPLAY  -30.0f   /* dBm, matches ui.c spectrum range */

// 256-entry RGB565 thermal LUT (matches browser palette):
//  black -> dark blue -> teal -> green -> yellow -> red
// Built at init from 6 anchor colors with linear interpolation.
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
        {  51,   0,   0, 128 },  // dark blue
        { 102,   0, 128, 192 },  // teal
        { 153,   0, 192,   0 },  // green
        { 204, 255, 224,   0 },  // yellow
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

// Comparator for qsort (used for median noise-floor estimate)
static int float_cmp(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
}

void render_waterfall_tick(const float *spectrum, int n_bins)
{
    if (!s_row || !spectrum || n_bins <= 0) return;

    // Phase 5.10F: auto-track noise floor by EMA'd median, once per second.
    static int64_t last_floor_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_floor_us > 1000000) {
        last_floor_us = now_us;
        static float scratch[2048];  /* big enough for N up to 2048 */
        int n = n_bins;
        if (n > (int)(sizeof(scratch)/sizeof(scratch[0]))) n = sizeof(scratch)/sizeof(scratch[0]);
        memcpy(scratch, spectrum, n * sizeof(float));
        qsort(scratch, n, sizeof(float), float_cmp);
        float median = scratch[n/2];
        /* EMA smoothing so the floor doesn't jump between samples */
        const float alpha = 0.3f;
        /* Track median directly: floor lands at LUT idx 32 (dark blue), not 0. */
        DB_MIN_DISPLAY = alpha * (median + 6.0f) + (1.0f - alpha) * DB_MIN_DISPLAY;
        /* Sanity clamp */
        if (DB_MIN_DISPLAY < -150.0f) DB_MIN_DISPLAY = -150.0f;
        if (DB_MIN_DISPLAY > -30.0f)  DB_MIN_DISPLAY = -30.0f;
    }

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
        // Match browser LUT mapping: noise floor -> idx 32, +31 dB above floor -> idx 255.
        // 30 dB above floor -> idx 255 (red). Tuned by eye.
        int idx = (int)((db - DB_MIN_DISPLAY) * (255.0f / 30.0f) + 32.0f);
        if (idx < 0) idx = 0;
        if (idx > 255) idx = 255;
        row[x] = s_lut[idx];
    }

    ui_push_waterfall_row(s_row);
}

