#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// Live POTA/RBN spots drawn at their frequency as a SEE-THROUGH overlay on the
// spectrum - the FlexRadio/SmartSDR convention: a bright callsign with a thin
// vertical line dropping from it to the frequency axis, so the line points at the
// frequency the spot is on.
//
// This replaced a dedicated 36 px strip between the spectrum and the frequency
// axis (operator's call, 2026-08-05, after seeing both). The overlay is the
// better trade: it reads the way every other panadapter does, and it gives those
// 36 px back to the waterfall.
//
// The callsign block is centred on the middle of the spectrum and the lines run
// from there DOWN to the axis. See-through comes from the line being 2 px wide
// rather than from dimming it - line and callsign are drawn at the same opacity
// so they read as one object.
//
// Implementation note that matters: the spots are LVGL objects composited over
// the spectrum canvas, NOT drawn into it. The render task rewrites that canvas
// at 30 Hz, so anything drawn in would be erased on the next frame.
//
// The overlay container is deliberately NOT clickable, and only the callsign
// labels are - otherwise a transparent object covering the whole spectrum would
// swallow tap-to-tune and pinch-zoom. Tapping the callsign itself is also how
// Flex does it.
//
// The visible window is fed in by ui.c from update_freq_axis_labels(), so the x
// mapping is the SAME one the axis labels use. If the two ever drifted, a spot
// would point at the wrong frequency under a correct axis - the one failure mode
// that makes this feature worse than not having it.

// Build the overlay. (x spans the display; `y`/`h` are the spectrum's own rect.)
void spots_lane_build(lv_obj_t *parent, int y, int h);

// Publish the currently visible frequency window. Call from wherever the
// frequency axis is recomputed so the two can never disagree.
void spots_lane_set_view(uint32_t lo_hz, uint32_t hi_hz);

// Show/hide as a whole - the lane belongs to the panadapter page only.
void spots_lane_set_visible(bool visible);

// The strip object, so the page-transition code can slide and hide it exactly
// like the other panadapter panes. NULL before spots_lane_build().
lv_obj_t *spots_lane_obj(void);

// Verify the frequency->x mapping, the age fade and the row packing against
// known values, logging PASS or the individual failures. Runs at boot: the lane
// is pure geometry, which is the part that cannot be checked by re-reading the
// code, and a wrong mapping would point callsigns at the wrong frequencies while
// looking perfectly plausible.
void spots_lane_selftest(void);
