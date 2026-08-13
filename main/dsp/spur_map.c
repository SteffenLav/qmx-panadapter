#include "spur_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "cat.h"
#include "dsp.h"
#include "ft8_tx.h"
#include "psram_task.h"
#include "settings.h"

static const char *TAG = "spurmap";

// ---- Tuning -------------------------------------------------------------
// 25 Hz: big enough that even the slowest window measured (multiplier ~3.75,
// itself uncertain) moves a spur ~2 bins, small enough that a real signal stays
// inside its own bin (0.53) and a CW note shifts only 25 Hz.
#define DITHER_HZ            25
// Timing, tightened 2026-08-13 after the operator measured 8-10 s to settle and
// found it restarting on every retune. The whole sequence is now ~2 s.
//
// AVG_FRAMES 16 (~0.33 s at the ~48 fps the FFT runs): the test only has to see
// a >=10 dB collapse on a 30-40 dB spur, and 16 frames leaves a noise bin at
// about 1.4 dB of spread - an order of magnitude of margin. 32 was simply
// cautious.
//
// RETUNE_SETTLE_MS stays clear of CAT's 200 ms write rate limit, and the writes
// go out FORCED so the limiter cannot silently drop one: a dropped nudge would
// mean measuring B at the same frequency as A, finding no movement, and learning
// nothing - a silent failure rather than a visible one.
#define AVG_FRAMES           16
#define RETUNE_SETTLE_MS     220
#define DIAL_STABLE_MS       600     /* never fire while the operator is tuning */
// 6, not 12. At 12 dB the mapped run stopped part way down each tooth's skirt,
// and the leftover edge bins were then the strongest thing on the display
// (measured: bin -333, the boundary of the -351..-333 run, at +13 dB). The
// vanish test still has to pass for every bin, so a lower threshold widens the
// run without admitting anything that did not move with the LO.
#define MIN_EXCESS_DB        6.0f    /* only consider peaks clearly above the floor */
#define VANISH_DROP_DB       10.0f   /* how far a peak must fall when nudged */
#define RETURN_MATCH_DB      4.0f    /* and how closely it must come back */
#define CACHE_ENTRIES        32
#define CACHE_STALE_US       (30LL * 60 * 1000000)  /* re-verify after 30 min */
// 14, not 9: the DC hump's skirt ran past the old edge and bin -9 was left as
// one of the two strongest things on the display. This is also the width of the
// blind spot the DC treatment costs - ~656 Hz, fixed at 12 kHz below the dial.
#define DC_GUARD_BINS        14      /* +/- bins around DC excluded from detection */
#define DC_MIN_EXCESS_DB     3.0f    /* DC bins this far over the floor are removed */

typedef struct {
    uint16_t bin;
    float    power_lin;   // linear power to subtract, trimmed by the servo
    float    power0_lin;  // as detected - the servo's clamp is relative to this
    float    floor_lin;   // what the bin should read once the spur is gone
    uint16_t ref_lo;      // reference bin below the run, clear of the skirt
    uint16_t ref_hi;      // reference bin above it
    float    ref_t;       // 0..1 position of this bin between the two
} spur_ent_t;

// The mapped set stops where a bin drops under MIN_EXCESS_DB, which is part way
// down the spur's skirt, not at the noise floor. Taking the interpolation
// reference from the bin immediately outside the run therefore reads the skirt
// and lifts the whole patch - which showed up as a wide flat-topped block that
// looked like a saturated signal. Step further out.
#define REF_GUARD_BINS 4

// Servo rate. Deliberately slow: it is averaging out the spur's own few-tenths
// wobble, and a fast loop would chase individual noisy frames.
// 0.008 (~2.6 s), not 0.002 (~11 s): the operator was timing the total settle,
// and the servo's convergence was most of it. The +/-1 dB clamp bounds whatever
// a faster loop chases, so there is little to lose.
#define SERVO_MU 0.008f

typedef struct {
    uint32_t   freq_hz;      // 0 = empty slot
    int64_t    learned_us;
    uint8_t    count;
    spur_ent_t ent[SPUR_MAP_MAX_ENTRIES];
} cache_row_t;

// Published map, double-buffered: the FFT task reads whichever index
// s_live points at, the detection task fills the other and then swaps. No lock
// on the hot path, and the worst possible race is one frame of stale data.
static spur_ent_t s_pub[2][SPUR_MAP_MAX_ENTRIES];
static uint8_t    s_pub_n[2] = {0, 0};
static volatile int s_live = 0;

static spur_mode_t s_mode     = SPUR_MODE_OFF;
static volatile bool s_measuring = false;
static cache_row_t *s_cache   = NULL;
static float   *s_bufA = NULL, *s_bufB = NULL, *s_bufC = NULL;
// Cold-path scratch, deliberately in PSRAM. Internal RAM is the scarce resource
// on this board and .bss growth there is what starves MALLOC_CAP_DMA (TODO #65,
// where 52 KB of our own statics was the root cause of WiFi instability). Only
// s_pub stays internal, because the FFT hot path reads it every frame.
static uint8_t    *s_mapped_flags = NULL;   /* DSP_FFT_SIZE */
static spur_ent_t *s_found        = NULL;   /* SPUR_MAP_MAX_ENTRIES */
static uint32_t s_mapped_freq = 0;   // frequency the published map belongs to

// ---- Small helpers ------------------------------------------------------

static int cmp_f(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

// Median of a dB spectrum, as the floor reference. Uses s_bufC as scratch when
// it is not otherwise in use; callers pass their own scratch to avoid that.
static float median_db(const float *src, float *scratch, int n)
{
    memcpy(scratch, src, n * sizeof(float));
    qsort(scratch, n, sizeof(float), cmp_f);
    return scratch[n / 2];
}

static void publish(const spur_ent_t *ent, int n, uint32_t freq_hz)
{
    int nxt = s_live ^ 1;
    if (n > SPUR_MAP_MAX_ENTRIES) n = SPUR_MAP_MAX_ENTRIES;
    if (n > 0) memcpy(s_pub[nxt], ent, n * sizeof(spur_ent_t));
    s_pub_n[nxt] = (uint8_t)n;

    // Precompute each bin's nearest UNMAPPED neighbours, so interpolate mode is
    // O(n) on the FFT path instead of searching. Mapped bins come in contiguous
    // runs (a spur is several bins wide with its skirts), so the reference has
    // to be outside the whole run - interpolating from an adjacent mapped bin
    // would just copy the spur back in.
    if (n > 0 && s_mapped_flags) {
        uint8_t *mapped = s_mapped_flags;   /* PSRAM - see spur_map_init */
        memset(mapped, 0, DSP_FFT_SIZE);
        for (int k = 0; k < n; k++)
            if (s_pub[nxt][k].bin < DSP_FFT_SIZE) mapped[s_pub[nxt][k].bin] = 1;
        for (int k = 0; k < n; k++) {
            int b = s_pub[nxt][k].bin;
            int lo = b, hi = b, guard = 0;
            // Bins wrap: the spectrum is circular, index 0 adjoins 1023.
            while (mapped[(lo + DSP_FFT_SIZE) % DSP_FFT_SIZE] && guard++ < 128)
                lo--;
            guard = 0;
            while (mapped[hi % DSP_FFT_SIZE] && guard++ < 128)
                hi++;
            lo -= REF_GUARD_BINS;
            hi += REF_GUARD_BINS;
            // The guard can land inside a NEIGHBOURING cluster (the teeth are
            // only ~20 bins apart in places). Walk clear again, or the
            // reference would be another spur - or a bin this same loop is
            // about to overwrite, making the result depend on iteration order.
            guard = 0;
            while (mapped[((lo % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE]
                   && guard++ < 128) lo--;
            guard = 0;
            while (mapped[((hi % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE]
                   && guard++ < 128) hi++;
            // Position of this bin between the two references, so the run gets
            // a straight ramp rather than one constant repeated across it.
            float span = (float)(hi - lo);
            s_pub[nxt][k].ref_t  = (span > 0.0f) ? ((float)(b - lo) / span) : 0.5f;
            s_pub[nxt][k].ref_lo = (uint16_t)(((lo % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE);
            s_pub[nxt][k].ref_hi = (uint16_t)(((hi % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE);
        }
    }

    s_live = nxt;
    s_mapped_freq = freq_hz;
}

// ---- Public API ---------------------------------------------------------

void spur_map_apply(float *mag2, int n_bins)
{
    if (s_mode == SPUR_MODE_OFF) return;
    int idx = s_live;
    int n = s_pub_n[idx];
    if (n <= 0) return;
    spur_ent_t *e = s_pub[idx];

    if (s_mode == SPUR_MODE_INTERPOLATE) {
        // Replace each mapped bin with a blend of the nearest bins OUTSIDE its
        // run. Those are never themselves mapped, so they are untouched by this
        // loop and the result does not depend on iteration order.
        for (int k = 0; k < n; k++) {
            int b = e[k].bin;
            if (b < 0 || b >= n_bins) continue;
            float a = mag2[e[k].ref_lo];
            float c = mag2[e[k].ref_hi];
            mag2[b] = a + e[k].ref_t * (c - a);
        }
        return;
    }

    const float lo_lim = 1.0f / powf(10.0f, SPUR_SERVO_MAX_TRIM_DB / 10.0f);
    const float hi_lim = powf(10.0f, SPUR_SERVO_MAX_TRIM_DB / 10.0f);

    for (int k = 0; k < n; k++) {
        int b = e[k].bin;
        if (b < 0 || b >= n_bins) continue;

        // Linear-domain subtraction: whatever else is in this bin survives.
        float v = mag2[b] - e[k].power_lin;

        // Servo: the residual should sit at the noise floor. Above it means we
        // are under-subtracting, below it means over. Trimming against the
        // residual is the only way to a deep null - a 0.37 dB error at
        // detection time caps cancellation at ~11 dB however long we average.
        float err = v - e[k].floor_lin;
        float p = e[k].power_lin + SERVO_MU * err;
        float lo = e[k].power0_lin * lo_lim;
        float hi = e[k].power0_lin * hi_lim;
        if (p < lo) p = lo;
        if (p > hi) p = hi;
        e[k].power_lin = p;

        // Clamp to the measured floor rather than to near-zero. The old code
        // dropped an over-subtracted bin to 0.001x, i.e. 30 dB BELOW the noise -
        // which does not read as a removed spur, it reads as a notch cut into
        // the display. The operator spotted exactly that.
        mag2[b] = (v > e[k].floor_lin) ? v : e[k].floor_lin;
    }
}

bool spur_map_is_enabled(void) { return s_mode != SPUR_MODE_OFF; }
bool spur_map_is_measuring(void) { return s_measuring; }
spur_mode_t spur_map_get_mode(void) { return s_mode; }

void spur_map_set_mode(spur_mode_t mode)
{
    if (mode > SPUR_MODE_INTERPOLATE) mode = SPUR_MODE_OFF;
    if (s_mode == mode) return;
    bool was_off = (s_mode == SPUR_MODE_OFF);
    s_mode = mode;
    // Switching off drops the map; switching on from off leaves it empty so the
    // detector re-measures rather than trusting something learned long ago.
    if (mode == SPUR_MODE_OFF || was_off) publish(NULL, 0, 0);
    ESP_LOGI(TAG, "spur suppression -> %s",
             mode == SPUR_MODE_OFF ? "off" :
             mode == SPUR_MODE_SUBTRACT ? "subtract" : "interpolate");
}

int spur_map_get_marks(uint16_t *bins, int max)
{
    int idx = s_live;
    int n = s_pub_n[idx];
    if (n > max) n = max;
    for (int k = 0; k < n; k++) bins[k] = s_pub[idx][k].bin;
    return n;
}

void spur_map_forget_all(void)
{
    if (s_cache) memset(s_cache, 0, CACHE_ENTRIES * sizeof(cache_row_t));
    publish(NULL, 0, 0);
    ESP_LOGI(TAG, "learned map cleared");
}

// ---- Cache --------------------------------------------------------------

static cache_row_t *cache_find(uint32_t freq)
{
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (s_cache[i].freq_hz == freq) return &s_cache[i];
    return NULL;
}

static cache_row_t *cache_slot_for(uint32_t freq)
{
    cache_row_t *hit = cache_find(freq);
    if (hit) return hit;
    cache_row_t *oldest = &s_cache[0];
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (s_cache[i].freq_hz == 0) return &s_cache[i];
        if (s_cache[i].learned_us < oldest->learned_us) oldest = &s_cache[i];
    }
    return oldest;
}

// ---- Detection ----------------------------------------------------------

static bool grab_average(float *dst)
{
    dsp_avg_start(AVG_FRAMES);
    for (int i = 0; i < 250; i++) {           // up to ~5 s
        vTaskDelay(pdMS_TO_TICKS(20));
        if (dsp_avg_ready(dst)) return true;
    }
    ESP_LOGW(TAG, "averaging timed out - no spectrum?");
    return false;
}

// A, B, C are dB spectra taken at freq, freq+DITHER, freq again. A bin holds a
// spur if it is strong in A AND back at the same level in C (so it is a stable
// feature of this dial setting, not fading) but has collapsed in B (so it moved
// when the LO did). A real signal cannot do that: 25 Hz shifts it only half a
// bin, so it stays put in all three.
static int classify(const float *A, const float *B, const float *C,
                    float *scratch, int n, spur_ent_t *out, int found)
{
    float fA = median_db(A, scratch, n);
    float fC = median_db(C, scratch, n);

    for (int i = 0; i < n; i++) {
        // Motion detection is MEANINGLESS at baseband DC: the LO's own leakage
        // sits at bin 0 and does not move when the LO moves, so the nudge test
        // has nothing to measure there. Without this guard the detector flagged
        // +0..+8 but none of -1..-8, and the operator saw exactly that - the
        // right half of the DC hump suppressed and the left half untouched.
        // Leaving the whole hump alone keeps it symmetric; removing it properly
        // needs a deterministic DC treatment, not this test.
        int sbin = (i < n / 2) ? i : i - n;
        if (sbin > -DC_GUARD_BINS && sbin < DC_GUARD_BINS) continue;

        float a = A[i], b = B[i], c = C[i];
        if (a - fA < MIN_EXCESS_DB) continue;
        if (c - fC < MIN_EXCESS_DB) continue;
        if (fabsf(a - c) > RETURN_MATCH_DB) continue;      // not stable: fading
        float lower = (a < c) ? a : c;
        if (lower - b < VANISH_DROP_DB) continue;          // did not move

        // Subtract the level it actually shows, less the local floor, so a real
        // signal sharing this bin keeps its own contribution.
        float mean_db = 0.5f * (a + c);
        float floor_db = 0.5f * (fA + fC);
        // A/B/C carry DSP_DB_CALIBRATION_OFFSET (they are dBm), but
        // spur_map_apply() subtracts from RAW mag2 straight out of the FFT.
        // Undo the calibration here or the stored power is ~15 orders of
        // magnitude too small and the subtraction silently does nothing.
        float p_sig  = powf(10.0f, (mean_db  - DSP_DB_CALIBRATION_OFFSET) / 10.0f);
        float p_flr  = powf(10.0f, (floor_db - DSP_DB_CALIBRATION_OFFSET) / 10.0f);
        float p = p_sig - p_flr;
        if (p <= 0.0f) continue;

        if (found < SPUR_MAP_MAX_ENTRIES) {
            out[found].bin = (uint16_t)i;
            out[found].power_lin  = p;
            out[found].power0_lin = p;
            out[found].floor_lin  = p_flr;
            found++;
        } else {
            // Full: keep the STRONGEST, never the first-found. Bins are visited
            // in index order and a negative offset is a high index, so
            // first-found silently discarded the biggest spurs on the band.
            // Never evict a DC-cluster entry: those are qualified a priori and
            // cannot be re-found by the motion test if they are dropped.
            int weakest = -1;
            for (int k = 0; k < found; k++) {
                int sb2 = (out[k].bin < n / 2) ? out[k].bin : (int)out[k].bin - n;
                if (sb2 > -DC_GUARD_BINS && sb2 < DC_GUARD_BINS) continue;
                if (weakest < 0 || out[k].power_lin < out[weakest].power_lin) weakest = k;
            }
            if (weakest < 0) continue;
            if (p > out[weakest].power_lin) {
                out[weakest].bin = (uint16_t)i;
                out[weakest].power_lin  = p;
                out[weakest].power0_lin = p;
                out[weakest].floor_lin  = p_flr;
            }
        }
    }
    return found;
}

// Baseband DC is LO self-mixing, present in every direct-conversion receiver
// and in this one measured at 15-22 dB over the floor. The dial-nudge test can
// never find it - bin 0 does not move when the LO does - so it is qualified a
// priori instead: it is an artifact by construction, and only its level needs
// measuring. Without this the DC hump is the one spur left fully visible, which
// is exactly the one the operator asked about first (it lands 12 kHz below the
// dial, at the +12 kHz IF offset).
//
// The cost is honest and bounded: a real signal within DC_GUARD_BINS of 12 kHz
// below the dial is treated as artifact. That is a ~840 Hz window at one fixed
// place on the display.
static int add_dc_cluster(const float *A, float *scratch, int n,
                          spur_ent_t *out, int found)
{
    float flr = median_db(A, scratch, n);
    float p_flr = powf(10.0f, (flr - DSP_DB_CALIBRATION_OFFSET) / 10.0f);

    for (int k = -(DC_GUARD_BINS - 1); k <= (DC_GUARD_BINS - 1); k++) {
        if (found >= SPUR_MAP_MAX_ENTRIES) break;
        int i = (k + n) % n;
        if (A[i] - flr < DC_MIN_EXCESS_DB) continue;   // nothing there worth removing
        float p = powf(10.0f, (A[i] - DSP_DB_CALIBRATION_OFFSET) / 10.0f) - p_flr;
        if (p <= 0.0f) continue;
        out[found].bin        = (uint16_t)i;
        out[found].power_lin  = p;
        out[found].power0_lin = p;
        out[found].floor_lin  = p_flr;
        found++;
    }
    return found;
}

static bool safe_to_dither(void)
{
    if (!cat_is_ready()) return false;
    if (cat_user_pause_active()) return false;
    // Retuning clears RIT, so a nudge would silently wipe the operator's offset.
    if (cat_get_rit_hz() != 0) return false;
    if (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_IDLE) return false;
    return true;
}

static void detect_at(uint32_t freq)
{
    if (!safe_to_dither()) return;

    s_measuring = true;
    ESP_LOGI(TAG, "learning spurs at %lu Hz (nudge %d Hz)",
             (unsigned long)freq, DITHER_HZ);

    bool ok = grab_average(s_bufA);
    if (ok) {
        cat_set_frequency_forced(freq + DITHER_HZ);
        vTaskDelay(pdMS_TO_TICKS(RETUNE_SETTLE_MS));
        ok = grab_average(s_bufB);
    }
    // Always put the dial back, even if a step failed - and FORCED, because a
    // restore dropped by the 200 ms rate limiter would leave the operator's dial
    // sitting 25 Hz high with nothing to indicate it.
    cat_set_frequency_forced(freq);
    vTaskDelay(pdMS_TO_TICKS(RETUNE_SETTLE_MS));
    if (ok) ok = grab_average(s_bufC);

    if (ok) {
        // NOT on the stack, and not in .bss either. As a local it was ~2 KB on
        // an 8 KB stack shared with qsort recursing over 1024 floats, and it
        // panicked with a Stack protection fault on the first detection. It now
        // lives in PSRAM: only spur_task touches it, once per detection.
        spur_ent_t *found = s_found;
        if (!found) { s_measuring = false; return; }
        // DC FIRST. It is qualified a priori and the motion test can never
        // re-find it, so it must not be left to compete for leftover slots -
        // that is exactly how it ended up unsuppressed the first time.
        // s_bufB is free as scratch by now; classify() only needs one array.
        int n = add_dc_cluster(s_bufA, s_bufB, DSP_FFT_SIZE, found, 0);
        n = classify(s_bufA, s_bufB, s_bufC, s_bufB, DSP_FFT_SIZE, found, n);
        publish(found, n, freq);

        cache_row_t *row = cache_slot_for(freq);
        row->freq_hz = freq;
        row->learned_us = esp_timer_get_time();
        row->count = (uint8_t)n;
        if (n > 0) memcpy(row->ent, found, n * sizeof(spur_ent_t));

        // Report the strongest few only - a full comb is 40+ bins once skirts
        // are counted and that is a wall of log nobody reads.
        float flr = median_db(s_bufA, s_bufB, DSP_FFT_SIZE);
        ESP_LOGI(TAG, "%lu Hz: %d spur bin(s) learned, strongest:",
                 (unsigned long)freq, n);
        for (int shown = 0; shown < 4 && shown < n; shown++) {
            int best = -1;
            for (int k = 0; k < n; k++) {
                if (found[k].power_lin < 0.0f) continue;   /* already reported */
                if (best < 0 || found[k].power_lin > found[best].power_lin) best = k;
            }
            if (best < 0) break;
            int sb = (found[best].bin < DSP_FFT_SIZE / 2)
                         ? found[best].bin : (int)found[best].bin - DSP_FFT_SIZE;
            ESP_LOGI(TAG, "   bin %+5d  %+9.1f Hz  %.1f dB over floor", sb,
                     sb * ((float)DSP_SAMPLE_RATE_HZ / (float)DSP_FFT_SIZE),
                     10.0f * log10f(found[best].power_lin)
                         + DSP_DB_CALIBRATION_OFFSET - flr);
            found[best].power_lin = -found[best].power_lin;  /* mark reported */
        }
        for (int k = 0; k < n; k++)
            if (found[k].power_lin < 0.0f) found[k].power_lin = -found[k].power_lin;

    }
    s_measuring = false;
}

static void spur_task(void *arg)
{
    (void)arg;
    uint32_t last_freq = 0;
    int64_t  last_change_us = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_mode == SPUR_MODE_OFF) { last_freq = 0; continue; }

        uint32_t f = cat_get_frequency();
        if (f == 0) continue;

        if (f != last_freq) {
            last_freq = f;
            last_change_us = esp_timer_get_time();
            // The old map belongs to the old frequency and is worse than
            // nothing now - a spur moves 16-50x the dial, so it is certainly
            // somewhere else.
            if (s_mapped_freq != f) publish(NULL, 0, 0);
            continue;
        }
        if (s_mapped_freq == f) continue;   // already have this one
        if ((esp_timer_get_time() - last_change_us) < (DIAL_STABLE_MS * 1000LL))
            continue;                       // still tuning

        cache_row_t *hit = cache_find(f);
        if (hit && (esp_timer_get_time() - hit->learned_us) < CACHE_STALE_US) {
            publish(hit->ent, hit->count, f);
            ESP_LOGI(TAG, "%lu Hz: %u spur(s) from cache (no nudge)",
                     (unsigned long)f, (unsigned)hit->count);
            continue;
        }
        detect_at(f);
    }
}

void spur_map_init(void)
{
    s_cache = heap_caps_calloc(CACHE_ENTRIES, sizeof(cache_row_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_bufA = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_bufB = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_bufC = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mapped_flags = heap_caps_malloc(DSP_FFT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_found = heap_caps_malloc(SPUR_MAP_MAX_ENTRIES * sizeof(spur_ent_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cache || !s_bufA || !s_bufB || !s_bufC || !s_mapped_flags || !s_found) {
        ESP_LOGE(TAG, "no PSRAM for spur map - feature disabled");
        s_mode = SPUR_MODE_OFF;
        return;
    }
    qmx_settings_t st;
    settings_load_all(&st);
    s_mode = (spur_mode_t)st.spur_mode;
    if (s_mode > SPUR_MODE_INTERPOLATE) s_mode = SPUR_MODE_OFF;
    ESP_LOGI(TAG, "spur suppression at boot: %s",
             s_mode == SPUR_MODE_OFF ? "off" :
             s_mode == SPUR_MODE_SUBTRACT ? "subtract" : "interpolate");
    // 8 KB, not 4: median_db() runs qsort over 1024 floats and that recursion is
    // most of the frame. The big per-detection arrays are statics (see
    // detect_at) precisely so this does not have to grow further.
    psram_task_create(spur_task, "spur_map", 8192, NULL, 2, tskNO_AFFINITY);
}
