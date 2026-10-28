#include "render.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>          // lroundf - see the S-meter conversion below
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "dsp.h"
#include "ui.h"
#include "ui_mode.h"
#include <limits.h>        /* UINT_MAX, for the push-spacing min */
#include "render_waterfall.h"
#include <string.h>

static const char *TAG = "render";

// RENDER_PERIOD_MS moved to render.h on 2026-10-24: the drawer's smoothing
// label turns the EMA coefficient into a time constant, and that arithmetic
// needs the frame interval. A second copy of the number in ui.c is the
// two-surfaces-drift class, so there is one definition and ui.c includes it.

static TaskHandle_t s_render_task = NULL;
static float *s_scratch = NULL;

// Phase 5.4: smoothing (EMA per bin, alpha=0.4)
static float *s_smoothed = NULL;
static bool s_smoothed_init = false;
// Phase 5.10D Stage 2: runtime-adjustable EMA smoothing
static float s_ema_alpha = 0.4f;

// v0.16.0: separate, more heavily smoothed spectrum fed only to the
// waterfall. The spectrum trace wants responsiveness (alpha 0.4), but the
// waterfall's per-bin noise floor tracker compares against a single frame -
// with alpha 0.4 the frame-to-frame variance of pure noise routinely
// exceeds the floor+6dB threshold, so noise bins light up as speckle even
// when the floor tracking itself is correct. A slower EMA here reduces that
// per-frame variance without affecting the spectrum trace's responsiveness.
#define WF_EMA_ALPHA 0.15f
static float *s_wf_smoothed = NULL;
static bool s_wf_smoothed_init = false;

void render_set_ema_alpha(float alpha)
{
    if (alpha < 0.05f) alpha = 0.05f;
    if (alpha > 1.0f) alpha = 1.0f;
    s_ema_alpha = alpha;
    ESP_LOGI("render", "EMA alpha = %.2f", (double)alpha);
}

// Operator-facing waterfall rate, in rows per second. Started life as the
// FT8-sync-lines diagnostic's private s_wf_2x, became a 1..4x drawer
// multiplier, and is now rows/s so it can go DOWN as well as up - which is
// what it was actually asked for. See render.h.
#define WF_ROWS_NOMINAL (1000 / RENDER_PERIOD_MS)   /* 10 rows/s at 1x */
static volatile uint8_t s_wf_rows = WF_ROWS_NOMINAL;
/* Milliseconds-weighted since frame sync: rows * dt_ms, emitting per whole
 * 1000. Widened from uint16 because 40 rows/s x a 500 ms clamp is 20,000. */
static uint32_t s_wf_acc;   /* render_task only - no other task touches it */

/* ⛔ 0 IS "OFF", AND IT IS A MEASURING INSTRUMENT, NOT A FEATURE.
 *
 * The floor used to be 1. The operator asked for the waterfall to be switched
 * off completely "so the scope can work freely", chasing the Tab5 spectrum
 * stutter Sam W7STF reported - and 1 row/s was not enough to answer it,
 * because a carpet that shifts once a second STILL redraws its whole
 * 1280x370 = 474 kpx when it does.
 *
 * ⚠ At 0 the carpet FREEZES on screen rather than disappearing - nothing
 * ticks it, so nothing invalidates it, which is the whole point: `wf` in
 * cpu_stats' per-panel line should fall to ~0 and the spectrum gets the
 * budget. The object is deliberately NOT hidden; hiding it would change the
 * layout and make the comparison a different screen.
 *
 * ⛔ NO OTHER CHANGE IS NEEDED HERE. The accumulator below already does the
 * right thing: `s_wf_acc += 0` never reaches WF_ROWS_NOMINAL, so the tick
 * simply never fires. Do not add a branch for it. */
void render_set_waterfall_rows_per_s(uint8_t rows)
{
    if (rows > 40) rows = 40;
    s_wf_rows = rows;
    ESP_LOGI("render", "waterfall rate: %u rows/s", rows);
}

/* The live rate, for ui.c's shift batching - it sizes the batch from the row
 * rate so a fast setting cannot re-flood what a slow one fixed. Reading the
 * one owner beats a second copy of the value drifting from it. */
uint8_t render_get_waterfall_rows_per_s(void) { return s_wf_rows; }



/* ⭐ PUSH SPACING - see render.h for why this exists and what it rules out.
 *
 * Nominal interval is RENDER_PERIOD_MS = 100 ms. "Late" is 150 ms, i.e. 1.5x,
 * matching the spirit of display.c's FRAME_LATE_US (~2x its own nominal): far
 * enough out that it cannot be ordinary tick rounding, close enough that a
 * real hitch is counted.
 *
 * ⚠ O(1): a subtract and a few compares per push, no division and no walk,
 * which is the same rule the display counters follow. The mean is divided only
 * when the window is read. */
#define RENDER_PUSH_LATE_US 150000u
static volatile unsigned s_push_min_us = UINT_MAX;
static volatile unsigned s_push_max_us;
static volatile uint64_t s_push_sum_us;
static volatile unsigned s_push_n;
static volatile unsigned s_push_late;
static volatile uint64_t s_push_dur_sum_us;
static volatile unsigned s_push_dur_max_us;
static volatile unsigned s_push_dur_n;

void render_push_spacing(render_push_spacing_t *out)
{
    if (!out) return;
    unsigned n = s_push_n;
    out->n       = n;
    out->min_us  = (n && s_push_min_us != UINT_MAX) ? s_push_min_us : 0;
    out->max_us  = s_push_max_us;
    out->mean_us = n ? (unsigned)(s_push_sum_us / n) : 0;
    out->late    = s_push_late;
    out->push_mean_us = s_push_dur_n ? (unsigned)(s_push_dur_sum_us / s_push_dur_n) : 0;
    out->push_max_us  = s_push_dur_max_us;
    s_push_min_us = UINT_MAX;
    s_push_max_us = 0;
    s_push_sum_us = 0;
    s_push_n      = 0;
    s_push_late   = 0;
    s_push_dur_sum_us = 0;
    s_push_dur_max_us = 0;
    s_push_dur_n      = 0;
}

/* ⭐ SYNTHETIC TRACE - see render.h for what it decides and why counters
 * cannot. A Gaussian peak gliding at a known, constant rate on a flat floor.
 *
 * Substituted where the real spectrum would land, so EVERYTHING downstream is
 * the untouched production path: the same EMA, the same ui_push_spectrum(),
 * the same lock, the same canvas, the same LVGL refresh. Only the numbers
 * differ, and their motion is exactly known.
 *
 * ⛔ THE EMA IS LEFT IN, deliberately. Taking it out would test a pipeline we
 * do not ship. A first-order filter on a constant-rate ramp settles to the
 * same constant rate with a fixed lag - it cannot introduce stepping, so it
 * costs the experiment nothing and keeps it honest.
 *
 * ⛔ THE FIRST VALUE WAS WRONG BY 6x AND THE COMMENT HID IT. It said 0.15 bin
 * per push was "~1.2 px per frame". Do the arithmetic the other way: a bin is
 * 1280/1024 = 1.25 SCREEN PIXELS (not 1.25 bins per pixel, which is what the
 * old wording inverted), so 0.15 bin is 0.19 px per push = 1.9 px/s. The peak
 * would have taken ELEVEN MINUTES to cross the screen - an experiment that
 * looks like a stationary blob and proves nothing about motion. Flashed and
 * switched on in that state; caught by doing the sum instead of re-reading the
 * sentence. [[feedback_measure_the_thing_not_the_word]]
 *
 * 2.0 bins per push = 2.5 px per push = 25 px/s, crossing the 1280 px screen
 * in ~51 s. Slow enough that stepping is obvious - which is the point - and
 * fast enough to read as motion. Phase is kept in a float and wrapped, so it
 * never quantises to the push index. */
#define SYNTH_STEP_BINS   2.0f
#define SYNTH_WIDTH_BINS  6.0f      /* Gaussian sigma - a few px wide        */
#define SYNTH_FLOOR_DBM   -125.0f
#define SYNTH_PEAK_DBM    -55.0f

static volatile int s_synth_mode;
static float        s_synth_pos;     /* render_task only */

void render_set_synthetic(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    s_synth_mode = mode;
    ESP_LOGW(TAG, "synthetic trace: %s",
             (mode == 2) ? "ON (noise-like floor)"
                         : (mode ? "ON (gliding peak)" : "off"));
}

int render_get_synthetic(void) { return s_synth_mode; }

/* Writes DSP_FFT_SIZE dB values in the same layout dsp_get_spectrum() uses -
 * raw, non-fftshifted, index 0 = DC - so the display's bin mapping, pan and
 * zoom all behave exactly as they do on real data. */
/* ⭐ MODE 2: WHAT A REAL NOISE FLOOR LOOKS LIKE TO THE BAND LOGIC.
 *
 * ⛔ MODE 1 CANNOT TEST THE PARTIAL INVALIDATE, AND I TRIED. Its peak runs
 * from -125 to -55 dBm, i.e. 70 dB, which on a 200 px canvas spanning 100 dB
 * is ~140 px. Every column at the peak's leading and trailing edge swings
 * y_top across most of the canvas, the changed band covers two thirds, and
 * ui_push_spectrum()'s guard correctly falls back to a full invalidate.
 * Measured 2362 kpx/s that way - indistinguishable from no optimisation at
 * all, and it proved nothing except that the test was wrong.
 *
 * A real noise floor is the opposite shape: flat, with every bin jittering a
 * few dB frame to frame. That is a NARROW band, which is where the partial
 * path is supposed to pay. So mode 2 is a flat floor plus bounded per-bin
 * noise - no signal, no motion, just the jitter - and it exercises exactly
 * the case mode 1 cannot.
 *
 * ⚠ This is a TEST SHAPE, not a simulation. Its only job is to put the band
 * width in the right range so the mechanism can be measured without a radio.
 * The real number still has to come from real audio; a mechanism that works
 * here and not on the air is still broken.
 *
 * xorshift32 rather than rand(): no lock, no shared state, and it runs on the
 * render task every push. */
#define SYNTH_NOISE_DB  8.0f     /* peak-to-peak, ~16 px of a 200 px canvas */

static uint32_t synth_rng(void)
{
    static uint32_t st = 0x2545F491u;
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    return st;
}

static void synth_fill_noise(float *dst)
{
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        /* [-0.5, 0.5) scaled to the peak-to-peak width. */
        float u = (float)(synth_rng() >> 8) / 16777216.0f - 0.5f;
        dst[i] = SYNTH_FLOOR_DBM + u * SYNTH_NOISE_DB;
    }
}

static void synth_fill(float *dst)
{
    s_synth_pos += SYNTH_STEP_BINS;
    if (s_synth_pos >= (float)DSP_FFT_SIZE) s_synth_pos -= (float)DSP_FFT_SIZE;

    const float inv2s2 = 1.0f / (2.0f * SYNTH_WIDTH_BINS * SYNTH_WIDTH_BINS);
    for (int i = 0; i < DSP_FFT_SIZE; i++) {
        /* Shortest distance on the circle, so the peak crosses the wrap
         * without a discontinuity - a jump there would look like the very
         * stepping this is meant to detect. */
        float d = (float)i - s_synth_pos;
        if (d >  (float)(DSP_FFT_SIZE / 2)) d -= (float)DSP_FFT_SIZE;
        if (d < -(float)(DSP_FFT_SIZE / 2)) d += (float)DSP_FFT_SIZE;
        float g = expf(-(d * d) * inv2s2);
        dst[i] = SYNTH_FLOOR_DBM + (SYNTH_PEAK_DBM - SYNTH_FLOOR_DBM) * g;
    }
}

// Phase 5.5: autoscale removed — static Ref/Range, manual control


/* ⭐⭐ ONE SPECTRUM PER FRAME, INSTEAD OF TEN PER SECOND INTO WHATEVER THE
 * DISPLAY HAPPENS TO BE DOING.
 *
 * ⛔ WHY NOT THE OBVIOUS FIX. The plan was to PHASE-LOCK the existing 10 Hz
 * push to the frame boundary, and the arithmetic killed it before it was
 * built: the push currently lands at a uniformly random point inside a frame
 * interval, which at ~58 ms frames is 58/sqrt(12) = 17 ms of standard
 * deviation. The measured appear SD is 47 ms. Removing 17 ms of independent
 * jitter leaves sqrt(47^2 - 17^2) = 44. THREE MILLISECONDS, for a flash and a
 * power cycle. Not worth doing.
 *
 * ⭐ WHAT THE OPERATOR ACTUALLY ASKED FOR is the trace updating more often -
 * "the spectrum ... probably updates double (or less) as often as before.
 * Still far far from what i see on the web spectrum". The web looks better for
 * a structural reason: it repaints at 60 Hz against 10 Hz data, so every frame
 * of the animation lands exactly 100 ms apart. The Tab5 repaints at ~17 fps,
 * so a 10 Hz push means most frames carry NOTHING NEW - they redraw the same
 * numbers.
 *
 * ⭐ AND THE PARTIAL INVALIDATE JUST MADE THE FIX AFFORDABLE. Each push used
 * to invalidate the whole 256 kpx canvas; it now invalidates the rows the
 * trace moved, measured at ~160 kpx. So 17 pushes/s costs ~2.7 Mpx/s against
 * the 2.56 Mpx/s that 10 pushes/s cost this morning - the headroom e68b711b
 * bought, spent on the thing he can see rather than banked.
 *
 * So the loop now waits for the DISPLAY to finish a frame and pushes then.
 * Every frame carries fresh data; nothing is computed that no frame will show.
 * It is self-limiting in exactly the right direction: if the display slows
 * under load, the pushes slow with it instead of piling up behind a timer.
 *
 * ⛔ THREE THINGS THAT WERE PER-ITERATION AND ARE NOW PER-MILLISECOND. The old
 * loop could assume 100 ms per pass. Nothing may assume that any more:
 *   - the EMA alpha, or the trace's time constant would halve with the frame
 *     rate and the smoothing would change every time the load did;
 *   - the waterfall accumulator, or rows/s would silently become rows/frame;
 *   - the S-meter divider, which counted iterations.
 * Each is now driven by measured elapsed time. Missing one of these is how a
 * rate change turns into a behaviour change nobody attributes to it.
 *
 * ⚠ FALLBACK AND FLOOR. The wait times out at RENDER_FALLBACK_MS so the loop
 * still runs when no frame is coming (overlay up, FT8 mode, display idle), and
 * a push closer than RENDER_MIN_PERIOD_MS to the last is skipped - a burst of
 * cheap 2 ms frames must not drag the FFT path with it.
 *
 * A/B: {"action":"framesync","on":false} restores the 100 ms timer. */
#define RENDER_FALLBACK_MS    100   /* no frames coming - keep the loop alive */
#define RENDER_MIN_PERIOD_MS   45   /* ~22 Hz ceiling, so a 2 ms frame burst
                                       cannot run the push at frame rate      */
static volatile bool s_frame_sync = true;

void render_set_frame_sync(bool on)
{
    s_frame_sync = on;
    ESP_LOGW(TAG, "frame sync %s - spectrum pushed %s",
             on ? "ON" : "off",
             on ? "once per display frame" : "every 100 ms by timer");
}

bool render_get_frame_sync(void) { return s_frame_sync; }

/* Called from ui.c's LV_EVENT_REFR_READY hook, on taskLVGL. O(1), no lock. */
void render_frame_done(void)
{
    if (s_render_task) xTaskNotifyGive(s_render_task);
}

static void render_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    int64_t last_push_us = 0;
    while (1) {
        if (s_frame_sync) {
            /* Wait for the display to finish a frame, then draw the next one's
             * content. The timeout is what keeps this alive when nothing is
             * refreshing at all. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RENDER_FALLBACK_MS));
            int64_t nw = esp_timer_get_time();
            if (last_push_us && (nw - last_push_us) < RENDER_MIN_PERIOD_MS * 1000) {
                continue;                      /* too soon - let frames bunch */
            }
            last = xTaskGetTickCount();        /* keep the timer path coherent
                                                  if frame sync is turned off */
        } else {
            vTaskDelayUntil(&last, pdMS_TO_TICKS(RENDER_PERIOD_MS));
        }
        /* Elapsed time since the previous pass drives the EMA, the waterfall
         * and the S-meter - see the note above. First pass has no history, so
         * it uses the nominal period. */
        int64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = last_push_us ? (uint32_t)((now_us - last_push_us) / 1000)
                                      : RENDER_PERIOD_MS;
        if (dt_ms < 1)   dt_ms = 1;
        if (dt_ms > 500) dt_ms = 500;          /* a gap is not a huge timestep */
        last_push_us = now_us;

        // v0.19.3 (Tier 1): the spectrum + waterfall canvases are fully
        // covered by the FT8 screen, but this loop used to keep drawing them
        // at 10 Hz anyway — every canvas write invalidates the region, and
        // every invalidation drags the whole flush + 90° software-rotation
        // pipeline (the highest-priority CPU consumer on core 0) over pixels
        // nobody can see, directly on top of audio_task and the ft8_dec0
        // decode helper on the same core. Skip ALL canvas work while the FT8
        // screen is up; only the S-meter below stays live (it is visible in
        // the FT8 top bar — the v0.15.7 fix exists precisely to keep it
        // running there). The web UI is unaffected: ws_push_task reads
        // dsp_get_spectrum() itself, not this pipeline.
        // WSPR is NOT excluded here, and that is a decision backed by numbers.
        //
        // It was excluded at first, on the theory that a 66 s decode inside a
        // 120 s cycle needs core 0 the way FT8 does. Measured, it buys nothing:
        // the decode takes 64.1-65.5 s with the panadapter rendering (the
        // self-test, in panadapter mode) and 65.7-66.1 s with it gated off (the
        // live loop, in WSPR mode). What it DID buy was a Tab5 frozen for as
        // long as the loop ran - reported by the operator within minutes - while
        // the web kept moving, because ws_push_task reads dsp_get_spectrum() on
        // its own path.
        //
        // The panadapter still freezes for the 120 s of each CAPTURE, because
        // dsp.c skips the FFT while one is armed. That is inherent to capturing
        // and not this gate's business.
        //
        // ⭐ WIDENED 2026-09-13 to any full-screen overlay, not just FT8 mode.
        // Operator: "I think we really need to close any other process down
        // when entering these resource eating features". The Reader, "Need
        // guidance?", the radio terminal and now SelfSpotter are all opaque
        // and cover the ENTIRE screen - the spectrum/waterfall canvases behind
        // any of them are exactly as invisible as they are in FT8 mode, and
        // this gate's own reasoning (a canvas write costs the flush + 90 deg
        // rotation pipeline regardless of whether anyone can see the result)
        // applies identically. ui_any_overlay_active() is the SAME four-way OR
        // sync_nav_affordances() already uses to hide the edge-swipe strips
        // for these same overlays - one predicate, not a second copy that
        // could drift from it.
        /* ⭐ SERVER MODE joins the same predicate (T19). Sam W7STF asked to
         * "shut down any activity (including the screen) that do not serve
         * the Web Socket", and this is where that activity is: every canvas
         * write drags the flush plus the 90-degree software rotation over
         * pixels nobody can see - which is exactly the reasoning the overlay
         * gate beside it already states. The browser keeps its full rate
         * because ws_push_task reads dsp_get_spectrum() on its own path. */
        bool pan_visible = (ui_mode_get() != UI_MODE_FT8) && !ui_any_overlay_active()
                           && !ui_get_server_mode();
        bool have_spectrum = false;

        if (pan_visible) {
            if (s_synth_mode) {
                /* Substituted here and nowhere else: every stage after this
                 * line is the production path, unchanged. */
                if (s_synth_mode == 2) synth_fill_noise(s_scratch);
                else                   synth_fill(s_scratch);
                have_spectrum = true;
            } else {
                have_spectrum = (dsp_get_spectrum(s_scratch) == ESP_OK);
                // ESP_ERR_NOT_FOUND just means no spectrum yet (no audio).
            }
        } else {
            // Restart both EMAs from fresh data when the panadapter returns,
            // instead of blending new frames into a minutes-old picture.
            s_smoothed_init = false;
            s_wf_smoothed_init = false;
            // Same reasoning for the row accumulator: a part-accumulated row
            // from before the overlay is not owed to the operator now.
            s_wf_acc = 0;
        }

        if (have_spectrum) {
            // Phase 5.4: EMA smoothing
            if (!s_smoothed_init) {
                // First frame: initialize smoothed with current values (no fade-in)
                memcpy(s_smoothed, s_scratch, DSP_FFT_SIZE * sizeof(float));
                s_smoothed_init = true;
            } else {
                /* ⛔ THE COEFFICIENT MUST FOLLOW THE INTERVAL, or the trace's
                 * smoothing silently changes with the frame rate. alpha 0.4
                 * per 100 ms is a time constant of
                 *     tau = -100 / ln(1 - 0.4) = 196 ms
                 * and the setting the operator adjusts is that feel, not the
                 * coefficient. Re-derive it for the interval actually
                 * measured: a = 1 - exp(-dt/tau). At 100 ms it returns 0.4
                 * exactly, so the timer path is unchanged bit for bit. */
                float a = s_ema_alpha;
                if (dt_ms != RENDER_PERIOD_MS) {
                    float tau = -(float)RENDER_PERIOD_MS / logf(1.0f - s_ema_alpha);
                    a = 1.0f - expf(-(float)dt_ms / tau);
                    if (a < 0.02f) a = 0.02f;
                    if (a > 1.0f)  a = 1.0f;
                }
                for (int i = 0; i < DSP_FFT_SIZE; i++) {
                    s_smoothed[i] = a * s_scratch[i]
                                  + (1.0f - a) * s_smoothed[i];
                }
            }


            // Push smoothed spectrum to UI
            /* Instrumented - see render_push_spacing(). The interval is taken
             * around the push rather than at the top of the loop, so it is the
             * cadence the GLASS sees, not the cadence the timer intended. */
            {
                static int64_t s_last_push_us;
                int64_t t0 = esp_timer_get_time();
                if (s_last_push_us) {
                    unsigned dt = (unsigned)(t0 - s_last_push_us);
                    if (dt < s_push_min_us) s_push_min_us = dt;
                    if (dt > s_push_max_us) s_push_max_us = dt;
                    if (dt > RENDER_PUSH_LATE_US) s_push_late++;
                    s_push_sum_us += dt;
                    s_push_n++;
                }
                s_last_push_us = t0;

                ui_push_spectrum(s_smoothed, DSP_FFT_SIZE);

                unsigned dur = (unsigned)(esp_timer_get_time() - t0);
                if (dur > s_push_dur_max_us) s_push_dur_max_us = dur;
                s_push_dur_sum_us += dur;
                s_push_dur_n++;
            }

            // Separate, more heavily smoothed spectrum for the waterfall only
            if (!s_wf_smoothed_init) {
                memcpy(s_wf_smoothed, s_scratch, DSP_FFT_SIZE * sizeof(float));
                s_wf_smoothed_init = true;
            } else {
                /* Same correction, same reason - see the spectrum EMA above. */
                float wa = WF_EMA_ALPHA;
                if (dt_ms != RENDER_PERIOD_MS) {
                    float wtau = -(float)RENDER_PERIOD_MS / logf(1.0f - WF_EMA_ALPHA);
                    wa = 1.0f - expf(-(float)dt_ms / wtau);
                    if (wa < 0.01f) wa = 0.01f;
                    if (wa > 1.0f)  wa = 1.0f;
                }
                for (int i = 0; i < DSP_FFT_SIZE; i++) {
                    s_wf_smoothed[i] = wa * s_scratch[i]
                                     + (1.0f - wa) * s_wf_smoothed[i];
                }
            }
        }

        // Phase 5.10D: sample S-meter at ~5 Hz from spectrum peak around VFO.
        // Runs in BOTH modes (dsp keeps publishing a spectrum every ~10 FFT
        // iterations while FT8 captures, and dsp_get_peak_dbm_around_vfo()
        // reads the DSP's own copy — it doesn't need s_scratch).
        //
        // ⭐ Also skipped under any full-screen overlay (2026-09-13), same
        // reasoning as pan_visible above: the S-meter is only ever visible in
        // the main app's own top bar (FT8's included), and every overlay this
        // file gates on covers that bar completely. Cheaper than the canvas
        // pipeline either way, but there is no reason to pay it for a widget
        // nobody can see.
        if (!ui_any_overlay_active()) {
            /* ⛔ WAS A COUNT OF ITERATIONS ("every 6th pass"), which only
             * meant 1.7 Hz while a pass was 100 ms. Under frame sync a pass is
             * a frame, so the S-meter would have sped up with the display.
             * Milliseconds, not passes. */
            static int64_t s_smeter_last_us = 0;
            if (now_us - s_smeter_last_us >= 600000) {
                s_smeter_last_us = now_us;
                float peak_dbm;
                int vfo_bin = ((ui_get_if_bin_shift(DSP_FFT_SIZE) % DSP_FFT_SIZE) + DSP_FFT_SIZE) % DSP_FFT_SIZE;
                if (dsp_get_peak_dbm_around_vfo(vfo_bin, 64, &peak_dbm) == ESP_OK) {
                    // S-unit conversion: S9 = -73 dBm, 6 dB per S-unit below.
                    // Above S9, we use S9+xx where xx = dbm - (-73).
                    //
                    // ⛔ lroundf, NOT the (int)(x + 0.5f) idiom. That idiom is
                    // only correct for x >= 0: C truncates toward zero, so for
                    // a NEGATIVE x it rounds up instead of to nearest, biasing
                    // the result high by up to a whole unit. Below S9 the
                    // argument here is always negative, and each unit is 6 dB,
                    // so the bias was up to 6 dB of real signal.
                    //
                    // Measured over the whole S1..S9 span: the Tab5 bar sat 5
                    // to 8.5 dB above the browser's for the SAME dBm, never
                    // less than 0.83 S-units out. Example: -103 dBm is exactly
                    // S4, and the bar drew S5. Above S9 the two always agreed,
                    // which is what made this look like a rendering style
                    // difference rather than arithmetic.
                    //
                    // Found 2026-10-09 from the operator's report that the Tab5
                    // and browser S-meters disagree. The browser was right.
                    int s_units;
                    if (peak_dbm >= -73.0f) {
                        s_units = 9 + (int)lroundf(peak_dbm + 73.0f);
                    } else {
                        s_units = 9 + (int)lroundf((peak_dbm + 73.0f) / 6.0f);
                        if (s_units < 0) s_units = 0;
                    }
                    ui_update_smeter(s_units);
                }
            }
        }

        if (have_spectrum) {
            /* One accumulator covers the whole 1..40 rows/s range, instead of
             * a fast branch and a slow branch that could disagree: add the
             * rate each period and emit a row for every whole 10 that have
             * accumulated. At 10 that is exactly one row per tick (today's
             * behaviour, bit for bit), at 40 four, at 1 one row every tenth
             * tick. Non-multiples land evenly rather than in bursts - 3
             * rows/s gives 10,3,3,3,10,... ticks apart, never 3 rows then a
             * second of nothing.
             *
             * ⛔ The accumulator is reset when the panadapter is not visible
             * (see the else branch), so returning from an overlay cannot dump
             * a backlog of rows in one frame. */
            /* ⛔ ROWS PER SECOND, AND IT USED TO MEAN ROWS PER PASS.
             * The old accumulator added s_wf_rows once per 100 ms pass and
             * emitted on every whole 10 - correct only while a pass WAS
             * 100 ms. Under frame sync a pass is a frame, so 10 rows/s would
             * have become ~17. Accumulate in milliseconds instead: add
             * rows * dt_ms and emit per whole 1000. At dt_ms = 100 this is
             * arithmetically the old expression, so the timer path is
             * unchanged. */
            s_wf_acc += (uint32_t)s_wf_rows * dt_ms;
            while (s_wf_acc >= 1000) {
                s_wf_acc -= 1000;
                render_waterfall_tick(s_wf_smoothed, DSP_FFT_SIZE);
            }
        }
    }
}

esp_err_t render_init(void)
{
    ESP_LOGI(TAG, "Render init (Phase 5.5 - static scale, smoothed spectrum at %d Hz)",
             1000 / RENDER_PERIOD_MS);

    // Scratch buffer in PSRAM, accessed once per frame
    s_scratch = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_scratch) {
        ESP_LOGE(TAG, "Failed to alloc render scratch buffer");
        return ESP_ERR_NO_MEM;
    }
    // Moved to PSRAM 2026-09-20 (was MALLOC_CAP_INTERNAL "for fast access" -
    // no measurement behind that claim, unlike dsp_init's FFT buffers, which
    // stayed internal only after a real 10x-slowdown test). Both are touched
    // in exactly the same sequential per-bin loop as s_scratch above, which
    // has always been PSRAM - a simple EMA over 1024 floats at 10 Hz is
    // nowhere near PSRAM's bandwidth limit. Found chasing the boot-time
    // MALLOC_CAP_DMA trough (8,192 B of it back to the pool).
    s_smoothed = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_smoothed) {
        ESP_LOGE(TAG, "Failed to alloc smoothing buffer");
        return ESP_ERR_NO_MEM;
    }
    s_smoothed_init = false;

    s_wf_smoothed = heap_caps_malloc(DSP_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_wf_smoothed) {
        ESP_LOGE(TAG, "Failed to alloc waterfall smoothing buffer");
        return ESP_ERR_NO_MEM;
    }
    s_wf_smoothed_init = false;

    esp_err_t wferr = render_waterfall_init();
    if (wferr != ESP_OK) {
        return wferr;
    }
    /* ⭐ CORE 1, NOT CORE 0 - AND THIS ONE IS SAFE WHERE taskLVGL WAS NOT.
     *
     * Measured 2026-09-22 with cpu_owners while the radio streamed and RX audio
     * played, decomposing the panadapter with ui_dev_canvas_hide():
     *
     *   canvases shown :  taskLVGL 66.7%  render 13.5%  idle0 0.0%  fps 11.5
     *   canvases hidden:  taskLVGL 33.9%  render 44.3%  idle0 0.1%  fps 19.4
     *
     * render TAKES 13.5% and WANTS 44.3% - it is starved by taskLVGL at
     * priority 5 above it. Adding up what core 0 is actually asked for:
     * taskLVGL 67 + render 44 + audio_task 13 + UAC 5 + misc 5 = ~134% of one
     * core. ⛔ CORE 0 IS OVERSUBSCRIBED BY A THIRD, which is why a year of
     * trying to shave it has failed - there is no 30% saving inside any single
     * task. Core 1 idles at 81% in the same sample.
     *
     * ⛔ WHY THIS IS NOT THE FALSIFIED taskLVGL MOVE. display.c's note by
     * .task_affinity records moving taskLVGL to core 1 killing the audio ring
     * within seconds: taskLVGL is priority 5, ABOVE fft_task's 4, so it
     * preempted the ring's only consumer. render is priority 3, BELOW
     * fft_task - FreeRTOS cannot schedule it while fft_task is ready, the same
     * structural argument rx_audio.c makes for its own task. And render never
     * touches LVGL or display_lock (grep: zero matches in render.c and
     * render_waterfall.c), so there is no lock to carry across the cores.
     *
     * ⚠ It now shares priority 3 with rx_audio on core 1. rx_audio spends
     * nearly all its time blocked in the I2S write (it does not even appear in
     * the cpu_owners table, i.e. under 0.2%), so they should not contend - but
     * that is the thing to watch if audio gets worse rather than better. */
    BaseType_t ok = xTaskCreatePinnedToCore(
        render_task, "render", 4096, NULL, 3, &s_render_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create render task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Render task started");
    return ESP_OK;
}











