#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// The spots lane: live POTA/RBN spots drawn at their frequency, in a dedicated
// strip BETWEEN the spectrum and the frequency axis.
//
// Why its own strip and not an overlay. Drawing spots on the trace ruins both -
// the labels sit in the signals you are trying to read - and the waterfall is
// out because it scrolls, so a label would either smear downwards with the
// history or have to be redrawn every tick at 30 Hz. A dedicated lane costs
// SPOTS_LANE_H pixels of waterfall once and is then free: the strip only
// repaints when the view moves, the spot table changes, or a spot ages.
//
// The lane is fed the visible window by ui.c from update_freq_axis_labels(), so
// the x mapping is the SAME one the axis labels use. That is deliberate - if the
// two ever drift, a spot would point at the wrong frequency on a correct axis,
// which is the one failure mode that would make the feature worse than useless.

// Height of the strip. Two label rows (montserrat_14) under a tick row; see
// place_labels() for why two and not three.
#define SPOTS_LANE_H 36

// Build the strip. `y` is its top edge in screen coordinates.
void spots_lane_build(lv_obj_t *parent, int y);

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
