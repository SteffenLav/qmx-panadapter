#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Coarse amateur band-plan data for the band-plan strip under the freq axis.
//
// "Coarse" by design: each HF band is split into at most a handful of CW /
// DIGI / PHONE blocks, enough to recognise where you are at a glance — NOT a
// full sub-band breakdown. Numbers are approximate IARU/region conventions and
// are deliberately easy to amend; treat them as an operator hint, not a
// legal authority.

typedef enum {
    BP_REGION_AUTO = 0,   // derive from the operator's Maidenhead grid
    BP_REGION_1    = 1,   // IARU R1: Europe, Africa, Middle East, N. Asia
    BP_REGION_2    = 2,   // IARU R2: the Americas
    BP_REGION_3    = 3,   // IARU R3: Asia-Pacific / Oceania
} bandplan_region_t;

typedef enum {
    BP_CW    = 0,
    BP_DIGI  = 1,
    BP_PHONE = 2,
} bp_seg_type_t;

typedef struct {
    uint32_t      lo_hz;
    uint32_t      hi_hz;
    bp_seg_type_t type;
} bp_seg_t;

// Map a Maidenhead grid to an IARU region (coarse, by longitude band).
// Returns BP_REGION_1 as a neutral fallback if the grid is unusable.
bandplan_region_t bandplan_region_from_grid(const char *grid);

// Resolve an effective region: if `setting` is BP_REGION_AUTO, derive it from
// `grid`; otherwise return `setting` as-is.
bandplan_region_t bandplan_effective_region(bandplan_region_t setting, const char *grid);

// Get the coarse segment list for the amateur band that contains freq_hz, for
// the given region. Returns the number of segments and points *out at a static,
// read-only array (valid for the program lifetime). Returns 0 (and *out = NULL)
// if freq_hz is not inside a known amateur band.
int bandplan_get_segments(uint32_t freq_hz, bandplan_region_t region, const bp_seg_t **out);

// Display colour (0xRRGGBB) for a segment type — shared by the strip and any
// legend so CW/DIGI/PHONE always read the same.
uint32_t bandplan_seg_color(bp_seg_type_t type);

// Short human label ("CW" / "Digi" / "Phone") for a segment type.
const char *bandplan_seg_label(bp_seg_type_t type);

#ifdef __cplusplus
}
#endif
