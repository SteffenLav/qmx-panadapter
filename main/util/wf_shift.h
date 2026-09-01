#pragma once
/* Moving the waterfall sideways when the still display's viewport moves (#298).
 *
 * Portable on purpose (no ESP/LVGL deps) so test/wf_shift_harness.c can link the
 * REAL function. The arithmetic is small and every one of its cases is a
 * different way to put history at the wrong frequency, which is the exact class
 * of silent wrongness #297 was.
 *
 * WHY A MARGIN AT ALL. In a still display the waterfall becomes frequency-
 * aligned history - a signal's past sits directly above its present - and that
 * is the main thing the still display buys. It also means the image has to move
 * with the viewport, and the canvas is 1280 x 740 RGB565 in PSRAM, so a full
 * horizontal memmove is roughly 20 ms on a core measured at 0-7% idle. Doing
 * that per 10 Hz dial click would be visible.
 *
 * So the canvas is WIDER than the screen and a view offset moves instead,
 * exactly as the existing double-height buffer already does for vertical
 * scrolling. Only when the margin runs out is a real memmove needed. The
 * viewport's own policy makes that rare: it holds still inside the dead band,
 * pushes at most 10% of the span (128 px) before it pages, and a page is the
 * one case where most of the history is legitimately gone anyway.
 *
 * ⛔ NEWLY EXPOSED COLUMNS MUST BE BLANKED, NOT LEFT. A column entering the view
 * holds whatever was at that canvas position long ago, which is not history for
 * the frequency now shown there. Blank means BLACK (no history yet), which is
 * deliberately NOT the hatching - hatching means "the radio cannot hear this",
 * and these are frequencies it can hear perfectly well.
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int canvas_w;   /* total canvas width in px, screen_w + 2*margin */
    int screen_w;   /* visible width */
    int margin;     /* slack each side; (canvas_w - screen_w) / 2 */
    int view_x;     /* canvas column currently shown at screen x = 0 */
} wf_shift_cfg_t;

/* One blank range, in POST-move canvas columns. */
typedef struct { int x, n; } wf_range_t;

typedef struct {
    int  view_x;        /* the new view_x the caller must adopt */
    bool move;          /* true = memmove keep_n columns from src_x to dst_x */
    int  src_x, dst_x, keep_n;
    wf_range_t clear[2];
    int  n_clear;
} wf_shift_plan_t;

/* Work out what has to happen to shift the view by `dx` columns (positive = the
 * view moves towards HIGHER frequency, so the image travels left under it).
 *
 * Returns false and leaves the plan zeroed for a degenerate config, which the
 * caller must treat as "do nothing", never as "clear everything". */
bool wf_shift_plan(const wf_shift_cfg_t *c, int dx, wf_shift_plan_t *out);

#ifdef __cplusplus
}
#endif
