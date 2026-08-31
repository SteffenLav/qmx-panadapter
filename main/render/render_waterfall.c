#include "render_waterfall.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ui.h"
#include "display.h"
#include "dsp.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>


/* No-data shading for the region the radio cannot hear (#297). Deliberately
 * the same slate as the spectrum's hatch so the two panes read as one block,
 * and deliberately NOT a colour from the waterfall LUT - it must not look like
 * a signal level. s_wf_row_parity advances once per row so the strokes lean
 * instead of forming vertical lines.
 */
#define WF_NODATA_BG     0x18E3   /* very dark slate */
#define WF_NODATA_HATCH  0x3186   /* one shade up, the stroke */
static int s_wf_row_parity;
static const char *TAG = "render_wf";

// Must match the display canvas width.
#define WF_WIDTH        DISPLAY_H_RES
// Phase 5.10F: waterfall floor color now auto-tracks the running
// noise-floor estimate (median of the spectrum) so the bottom of the
// color LUT always sits at "current noise level" regardless of band
// conditions. Spectrum trace remains user-controlled via the drawer.
#define DB_MIN_INITIAL  -130.0f  /* dBm, starting floor */
#define DB_MAX_DISPLAY  -30.0f   /* dBm, matches ui.c spectrum range */

// v0.16.0: per-bin adaptive noise-floor tracker. Replaces the old single
// global DB_MIN_DISPLAY (passband median) so each part of the spectrum gets
// its own black level - the QMX's filter rolloff means bins outside the
// passband sit several dB hotter than the passband floor, and colorizing
// them against the passband's black point made them look like speckled
// noise. Asymmetric EMA per bin: fast attack when the current value is
// below the tracked floor (follow the noise down quickly), slow decay when
// above (so a steady signal doesn't get absorbed into the floor too fast).
#define WF_FLOOR_ATTACK       0.05f  /* ~0.6 s time constant @ 30 Hz */
#define WF_FLOOR_DECAY_FAST   0.05f  /* ~0.6 s, used briefly after (re)init */
#define WF_FLOOR_DECAY_SLOW   0.0005f /* ~67 s, steady state - signals must
                                          persist for minutes before they're
                                          absorbed into the floor */
#define WF_FLOOR_WARMUP_TICKS 90     /* ~3 s @ 30 Hz of fast decay after reset */
static float s_floor_main[DSP_FFT_SIZE];
static float s_floor_zoom[DSP_FFT_SIZE];
static bool s_floor_main_init = false;
static bool s_floor_zoom_init = false;
static int s_floor_warmup = 0;

// v0.18.3: user-adjustable colorisation, live from the Waterfall drawer.
// Each is read every tick so a slider drag is visible as the new rows scroll
// in immediately.
//  - black level: dB above each bin's floor that maps to LUT black. Higher =>
//    more of the near-noise leakage is gated to black => crisper signals.
//  - contrast span: dB above (floor+black) that maps to full-scale red. Wider
//    => strong signals stop saturating into a fat blob and keep their shape.
//  - floor blend: 0 = single global floor (the pre-v0.16.0 look, no per-bin
//    smear), 1 = full per-bin adaptive floor. Lets the user dial back exactly
//    the per-bin tracker that fattens signals, or turn it off entirely.
#define WF_BLACK_DB_DEFAULT    9.0f
#define WF_CONTRAST_DB_DEFAULT 45.0f
#define WF_FLOOR_BLEND_DEFAULT 1.0f
static float s_wf_black_db    = WF_BLACK_DB_DEFAULT;
static float s_wf_contrast_db = WF_CONTRAST_DB_DEFAULT;
static float s_wf_floor_blend = WF_FLOOR_BLEND_DEFAULT;

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

// Anchor-based colour maps. Each map is a list of (idx, R, G, B) anchors;
// indices must be ascending and the last must be 255. build_lut() interpolates linearly.
typedef struct { uint8_t idx, r, g, b; } cmap_anchor_t;

static const cmap_anchor_t CMAP_THERMAL[] = {
    {   0,   0,   0,   0 },  // black
    {  51,   0,   0, 128 },  // dark blue
    { 102,   0, 128, 192 },  // teal
    { 153,   0, 192,   0 },  // green
    { 204, 255, 224,   0 },  // yellow
    { 255, 255,   0,   0 },  // red
};
static const cmap_anchor_t CMAP_VIRIDIS[] = {
    {   0,  68,   1,  84 },  // deep purple
    {  64,  59,  82, 139 },  // blue
    { 128,  33, 144, 141 },  // teal
    { 192,  93, 201,  99 },  // green
    { 255, 253, 231,  37 },  // yellow
};
static const cmap_anchor_t CMAP_TURBO[] = {
    {   0,  48,  18,  59 },  // deep purple-blue
    {  43,  64, 100, 225 },  // blue
    {  85,  46, 197, 233 },  // cyan
    { 128, 119, 245, 138 },  // green
    { 170, 251, 197,  53 },  // yellow-orange
    { 212, 245,  98,  37 },  // orange-red
    { 255, 122,   4,   2 },  // dark red
};
static const cmap_anchor_t CMAP_GRAYSCALE[] = {
    {   0,   0,   0,   0 },
    { 255, 255, 255, 255 },
};

static const struct {
    const cmap_anchor_t *anchors;
    int n;
    const char *name;
} g_colormaps[] = {
    { CMAP_THERMAL,   sizeof(CMAP_THERMAL)/sizeof(CMAP_THERMAL[0]),   "Thermal"   },
    { CMAP_VIRIDIS,   sizeof(CMAP_VIRIDIS)/sizeof(CMAP_VIRIDIS[0]),   "Viridis"   },
    { CMAP_TURBO,     sizeof(CMAP_TURBO)/sizeof(CMAP_TURBO[0]),       "Turbo"     },
    { CMAP_GRAYSCALE, sizeof(CMAP_GRAYSCALE)/sizeof(CMAP_GRAYSCALE[0]), "Grayscale" },
};
#define CMAP_COUNT (sizeof(g_colormaps)/sizeof(g_colormaps[0]))

static uint8_t s_colormap_idx = 0;

static void build_lut(void)
{
    if (s_colormap_idx >= CMAP_COUNT) s_colormap_idx = 0;
    const cmap_anchor_t *anchors = g_colormaps[s_colormap_idx].anchors;
    int n = g_colormaps[s_colormap_idx].n;
    for (int seg = 0; seg < n - 1; seg++) {
        int i0 = anchors[seg].idx;
        int i1 = anchors[seg + 1].idx;
        int span = i1 - i0;
        if (span <= 0) continue;
        for (int i = i0; i <= i1; i++) {
            float t = (float)(i - i0) / (float)span;
            uint8_t r = (uint8_t)(anchors[seg].r + t * (anchors[seg + 1].r - anchors[seg].r));
            uint8_t g = (uint8_t)(anchors[seg].g + t * (anchors[seg + 1].g - anchors[seg].g));
            uint8_t b = (uint8_t)(anchors[seg].b + t * (anchors[seg + 1].b - anchors[seg].b));
            s_lut[i] = rgb565(r, g, b);
        }
    }
}

void render_waterfall_floor_reset(void)
{
    s_floor_main_init = false;
    s_floor_zoom_init = false;
    s_floor_warmup = 0;
}

void render_waterfall_set_black_level(float db)
{
    if (db < 0.0f)  db = 0.0f;
    if (db > 30.0f) db = 30.0f;
    s_wf_black_db = db;
}

void render_waterfall_set_contrast_db(float db)
{
    if (db < 10.0f) db = 10.0f;   // avoid div-by-tiny / everything saturates
    if (db > 80.0f) db = 80.0f;
    s_wf_contrast_db = db;
}

void render_waterfall_set_floor_blend(float blend)
{
    if (blend < 0.0f) blend = 0.0f;
    if (blend > 1.0f) blend = 1.0f;
    s_wf_floor_blend = blend;
}

void render_waterfall_set_colormap(uint8_t idx)
{
    if (idx >= CMAP_COUNT) return;
    if (idx == s_colormap_idx) return;
    s_colormap_idx = idx;
    build_lut();
    ESP_LOGI(TAG, "Colour map set to '%s' (idx %u)", g_colormaps[idx].name, (unsigned)idx);
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

// Comparator for qsort (one-time median seed of the floor array)
static int float_cmp(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
}

void render_waterfall_tick(const float *spectrum, int n_bins)
{
    if (!s_row || !spectrum || n_bins <= 0) return;

    uint16_t *row = (uint16_t *)s_row;

    // Pixel x -> FFT bin, with fftshift:
    //   x=0       -> bin n_bins/2  (most-negative freq)
    //   x=WF_W/2  -> bin 0         (DC)
    //   x=WF_W-1  -> bin n_bins/2 - 1 (most-positive freq, wraps to n_bins/2 - 1)
    // For 1024 bins on 1280 px we have ~0.8 bins/px (slight oversample, fine for waterfall).
    // Zoom+pan: mirror spectrum render path exactly. v0.16.0: if a
    // higher-resolution zoom-FFT spectrum centered on the pan target is
    // available (zoom >= x2), use that with center_bin=0 and only the
    // residual zoom on top (see ui_push_spectrum for the rationale).
    const float *use_spectrum = spectrum;
    float wf_eff_zoom = ui_get_zoom_factor();
    int wf_center;
    const float *zoom_spec = (n_bins == DSP_FFT_SIZE) ? dsp_get_zoom_spectrum() : NULL;
    if (zoom_spec) {
        use_spectrum = zoom_spec;
        wf_eff_zoom = dsp_get_zoom_residual();
        wf_center = 0;
    } else {
        wf_center = ((ui_get_if_bin_shift(n_bins) + ui_get_pan_offset_bins()) % n_bins + n_bins) % n_bins;
    }
    int wf_window = (int)((float)n_bins / wf_eff_zoom);
    if (wf_window < 4) wf_window = 4;
    if (wf_window > n_bins) wf_window = n_bins;
    int wf_start  = wf_center - wf_window / 2;

    // v0.16.0: per-bin noise-floor tracker, updated every tick. Use a
    // separate floor array for the zoom spectrum vs the main spectrum,
    // since the same array index means a different frequency in each, and
    // the zoom-FFT spectrum has different dB characteristics (gain/
    // averaging) than the main one. Reseed whichever array becomes active
    // whenever we switch between zoomed and unzoomed - otherwise the
    // inactive array's stale seed only has the slow steady-state decay to
    // catch up, which takes ~1 min.
    float *floor_arr = zoom_spec ? s_floor_zoom : s_floor_main;
    bool *floor_init = zoom_spec ? &s_floor_zoom_init : &s_floor_main_init;
    static bool s_prev_zoom_active = false;
    bool zoom_active = (zoom_spec != NULL);
    if (zoom_active != s_prev_zoom_active) {
        *floor_init = false;
    }
    s_prev_zoom_active = zoom_active;
    if (!*floor_init) {
        // Seed every bin from one global median, not each bin's own value -
        // a bin holding a steady signal would otherwise start with
        // floor == signal and never show contrast. Quiet bins then attack
        // down to their true per-bin floor during the warmup window;
        // signal bins start well above the seed and stay bright.
        static float scratch[DSP_FFT_SIZE];
        int n = n_bins < DSP_FFT_SIZE ? n_bins : DSP_FFT_SIZE;
        memcpy(scratch, use_spectrum, n * sizeof(float));
        qsort(scratch, n, sizeof(float), float_cmp);
        float median = scratch[n / 2];
        for (int i = 0; i < DSP_FFT_SIZE; i++) {
            floor_arr[i] = median;
        }
        *floor_init = true;
        s_floor_warmup = WF_FLOOR_WARMUP_TICKS;
    }
    float decay = (s_floor_warmup > 0) ? WF_FLOOR_DECAY_FAST : WF_FLOOR_DECAY_SLOW;
    if (s_floor_warmup > 0) s_floor_warmup--;
    // Track a single global floor (mean of the per-bin floors) alongside the
    // per-bin array, so the floor-blend control can cross-fade between them.
    float floor_sum = 0.0f;
    int   floor_cnt = 0;
    for (int i = 0; i < n_bins && i < DSP_FFT_SIZE; i++) {
        float db = use_spectrum[i];
        float diff = db - floor_arr[i];
        floor_arr[i] += (diff < 0.0f ? WF_FLOOR_ATTACK : decay) * diff;
        floor_sum += floor_arr[i];
        floor_cnt++;
    }
    float floor_global = (floor_cnt > 0) ? (floor_sum / (float)floor_cnt)
                                         : floor_arr[0];

    /* The half of #297 the waterfall owns. The spectrum above already refuses
     * to wrap; if this still did, the two panes would disagree and the history
     * would carry a band of somebody else's spectrum down the screen for the
     * next minute. A column outside what the radio can hear is hatched with the
     * same 9-px diagonal the spectrum uses, so the dead area reads as one block
     * across both panes rather than stopping at the axis. */

    /* The same sub-bin correction the spectrum applies (see ui.c). The zoom FFT
     * can only be centred to a whole base bin - 46.875 Hz, which is 10 px at x16
     * - so the remainder is applied to the mapping instead. Without it the
     * waterfall steps while the spectrum above it does not, and the two panes
     * disagree by up to half a bin. */
    int wf_res_bins = 0;
    if (zoom_spec) {
        int decim = dsp_get_zoom_decim();
        if (decim < 1) decim = 1;
        int64_t pan_q = (int64_t)ui_get_pan_offset_bins() * DSP_SAMPLE_RATE_HZ / n_bins;
        int64_t r_hz  = ui_get_pan_offset_hz() - pan_q;
        wf_res_bins   = (int)llround((double)r_hz * (double)n_bins * (double)decim
                                     / (double)DSP_SAMPLE_RATE_HZ);
    }

    /* ⛔ THE SAME VIEWPORT THE SPECTRUM IS DRAWN WITH, not one derived here.
     *
     * This used to test a signed bin offset against +/- n/2, built on a
     * wf_center taken MODULO n_bins - a different question from the one the
     * spectrum asks. With the view panned the two answers had nothing to do
     * with each other: on the bench, 2026-08-31, the spectrum hatched its left
     * 13% and the waterfall hatched everything else, burying the signals.
     * pan_view.h had already written the rule - one mapping, or they drift. */
    pan_view_cfg_t pvc;
    pan_view_t     pv;
    const bool     pv_ok = ui_pan_view_current(&pvc, &pv, n_bins);

    for (int x = 0; x < WF_WIDTH; x++) {
        int bin;
        if (pv_ok) {
            bin = pan_view_x_to_bin(&pvc, &pv, x);
            if (bin == PAN_VIEW_NO_DATA) {
                row[x] = (((x + s_wf_row_parity) % 9) == 0) ? WF_NODATA_HATCH
                                                            : WF_NODATA_BG;
                continue;
            }
        } else {
            /* Zoom FFT: dsp_set_zoom() has mixed the pan target to DC, so these
             * bins mean something else and pan_view does not apply. The span is
             * at most 24 kHz and sits inside the capture window by
             * construction, so this path cannot wrap either. */
            int b = wf_start + wf_res_bins
                  + (int)((float)x * (float)wf_window / (float)WF_WIDTH);
            bin = ((b % n_bins) + n_bins) % n_bins;
        }

        float db = use_spectrum[bin];
        // Blend each bin's own adaptive floor with the single global floor
        // (blend=1 -> per-bin, blend=0 -> global), then map:
        //   floor+black -> LUT black (idx 32), +contrast dB above that -> red.
        // black level gates near-noise leakage; contrast span sets how many dB
        // fill the colour ramp (wider = strong signals don't bloom). All three
        // are live from the Waterfall drawer.
        float eff_floor = floor_global + (floor_arr[bin] - floor_global) * s_wf_floor_blend;
        float db_min_display = eff_floor + s_wf_black_db;
        int idx = (int)((db - db_min_display) * (255.0f / s_wf_contrast_db) + 32.0f);
        if (idx < 0) idx = 0;
        if (idx > 255) idx = 255;
        row[x] = s_lut[idx];
    }

    s_wf_row_parity++;
    ui_push_waterfall_row(s_row);
}

