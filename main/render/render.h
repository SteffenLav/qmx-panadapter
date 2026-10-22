#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Initialize the render subsystem. Call after dsp_init().
esp_err_t render_init(void);

// Phase 5.10D Stage 2: runtime EMA smoothing setter
void render_set_ema_alpha(float alpha);

// Waterfall scroll rate in ROWS PER SECOND, 1..40, default 10. The
// spectrum/S-meter cadence in render_task is untouched at every setting - this
// only changes how many waterfall rows that same frame is pushed into, since a
// faster or slower FFT rate is not what "rate" means here.
//
// Above 10 the extra rows are duplicates of the one fresh frame (there is no
// newer sample inside the period); below 10 the push is simply skipped on some
// ticks, which is the point - the waterfall is ~21 ms of a ~64 ms frame draw,
// and a skipped push is a whole canvas invalidation that core 0 never has to
// blit. Was a 1..4x multiplier; one axis, one setting, both directions.
void render_set_waterfall_rows_per_s(uint8_t rows);

/* ⭐ THE EXPERIMENT THAT SEPARATES "THE DATA IS UNEVEN" FROM "THE PICTURE IS
 * UNEVEN", WITHOUT EITHER OF US JUDGING IT BY EYE FIRST.
 *
 * ⛔ WHY IT EXISTS (2026-10-22). The lock-timeout fix removed a MEASURED loss
 * of 14 spectrum frames in every 100 - lockfail 14 -> 0 over 40 windows, every
 * push now reaching the glass - and the operator looked at the screen and said
 * it still stutters. So the dropped frames were real and were not the symptom.
 *
 * Everything measured so far describes the TRANSPORT: how evenly pushes leave
 * render_task (metronomic, late 0/100), how many are lost (now none), how many
 * frames draw each one (2x 70, 3x 24), how stale they are (58 ms). None of it
 * says whether the PICTURE those numbers carry moves smoothly, because the
 * picture is a spectrum of real noise - it jumps about by its nature, and no
 * counter can tell a jumpy signal from a jumpy renderer.
 *
 * So: replace the content with something whose motion is known exactly. A
 * single Gaussian peak that advances a fixed fraction of a bin per push, on a
 * flat floor. Everything downstream is untouched - the same EMA, the same
 * canvas fill, the same lock, the same LVGL path.
 *
 *   The peak GLIDES smoothly  -> the pipeline presents evenly. The unevenness
 *                                is in the DATA (the FFT content itself, its
 *                                windowing, or the dial/bin mapping), not in
 *                                the drawing.
 *   The peak JERKS or stalls  -> the presentation is at fault and the FFT is
 *                                innocent. Look at the draw, the beat between
 *                                22 fps and 10 Hz, or num_fbs = 1.
 *
 * Either answer kills half the remaining search space, which is more than any
 * further counter on the transport can do.
 *
 * ⚠ The step is deliberately SLOW - a fraction of a bin per push, about 1.5 px
 * a frame at zoom 1. Fast motion hides stepping; slow motion is where the eye
 * is most sensitive to it, which is the whole point.
 *
 * mode 0 = off (real spectrum), 1 = gliding peak. Dev only, not persisted:
 * {"action":"spectest","mode":1}. */
void render_set_synthetic(int mode);
int  render_get_synthetic(void);

/* ⭐ HOW EVENLY DOES THE SPECTRUM DATA ACTUALLY ARRIVE?
 *
 * Deliberately the same shape as display_frame_spacing() in display/display.h,
 * because it answers the other half of the same question. That one says how
 * evenly LVGL FRAMES land; this says how evenly the DATA those frames draw
 * lands. The Tab5 can redraw at 17.6 fps and still look stepped if the trace
 * only CHANGES unevenly.
 *
 * ⛔ WHY IT EXISTS (2026-10-22). Sam W7STF reported the Tab5 spectrum
 * stuttering while the browser stays smooth. Three explanations have been
 * killed by measurement: waterfall RATE (1 row/s changed nothing), waterfall
 * LOAD (off entirely helped, but a big gap remains), and DATA RATE (both the
 * Tab5 and the web push at exactly 10 Hz under vTaskDelayUntil). What has
 * never been measured is whether render_task's 10 Hz actually LANDS at 10 Hz.
 * vTaskDelayUntil guarantees the wake-up, not that the task was not preempted
 * between waking and pushing - and core 0 is the known wall on this board.
 *
 * interval_* is wake-to-wake between pushes; push_* is the time inside
 * ui_push_spectrum() itself, so "it arrived late" and "it took long" cannot be
 * confused - the same split display.c made between frame spacing and draw
 * duration, for the same reason.
 *
 * All microseconds. Reads and RESETS, so the caller's cadence is the window.
 * n == 0 means no push happened in it - a real reading, not an error (the
 * panadapter can be covered by another screen). O(1) per push. */
typedef struct {
    unsigned n;              /* pushes in the window                        */
    unsigned min_us;         /* shortest interval between two pushes        */
    unsigned mean_us;        /* mean interval - should sit on 100,000       */
    unsigned max_us;         /* worst interval                              */
    unsigned late;           /* intervals over RENDER_PUSH_LATE_US          */
    unsigned push_mean_us;   /* mean time spent inside ui_push_spectrum()   */
    unsigned push_max_us;    /* worst single push                           */
} render_push_spacing_t;

void render_push_spacing(render_push_spacing_t *out);

