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
/* ---- the transmit guard (T48) --------------------------------------------
 *
 * ⛔ READ THE WARNING AT THE TOP OF THIS HEADER FIRST: these numbers are
 * "an operator hint, not a legal authority", and on 2026-10-21 one of them was
 * MEASURABLY WRONG - the R1/R3 60 m top edge was 5366.0 where the licence edge
 * is 5366.5, which made the correct WSPR frequency read as out of band. A
 * guard built on this table will therefore be wrong occasionally, which is why
 * the operator can switch it off (settings: tx_band_guard) and why it says
 * what it refused and why.
 *
 * It exists because nothing checked at all: a WSPR 60 m dial that emitted
 * 63 kHz below the band shipped, and WSPR is one of the six sites that key the
 * radio with nobody deciding.
 *
 * bandplan_tx_in_band() takes the RF ACTUALLY EMITTED, not the dial. For a
 * digital mode the carrier is dial + audio, so the dial alone is the wrong
 * number - a dial just under the band edge with 2 kHz of audio is legitimately
 * inside, and a dial at the top edge is not. Pass the lowest and highest tone
 * of the burst; both must land in the same band.
 */
bool bandplan_tx_in_band(uint32_t rf_lo_hz, uint32_t rf_hi_hz,
                         bandplan_region_t region);

/* Is the effective region KNOWN, or merely defaulted?
 *
 * ⛔ bandplan_region_from_grid() returns BP_REGION_1 for a missing or
 * unparseable grid, and that default is invisible to its caller. For the
 * display strip that is harmless. For a TRANSMIT GUARD it is not: an operator
 * in the USA with the region on Auto and no grid set would be judged against
 * Region 1, whose 80 m stops at 3.8 MHz, and refused on 3.9 MHz - which is
 * legal where they are. So the guard does not refuse when this is false.
 */
bool bandplan_region_is_known(bandplan_region_t setting, const char *grid);

uint32_t bandplan_seg_color(bp_seg_type_t type);

// Short human label ("CW" / "Digi" / "Phone") for a segment type.
const char *bandplan_seg_label(bp_seg_type_t type);

#ifdef __cplusplus
}
#endif
