#pragma once
#include <stdbool.h>
#include <stdint.h>

/* WSPR simulation mode - the sibling of ft8_sim.c, and deliberately built the
 * same way, because that design is what makes the FT8 one trustworthy:
 *
 *   IT FEEDS THE REAL DECODER. Phantom stations are synthesized as actual
 *   4-FSK audio and pushed through wspr_find_candidates() and
 *   wspr_decode_candidate() - the same code real RF goes through. Nothing here
 *   fabricates a spot. If the decoder cannot decode it, it does not appear, and
 *   that property is the entire point: a sim that injects finished results
 *   tests nothing and quietly lies about what the radio can do.
 *
 * Shares ONE setting with the FT8 sim, qmx_settings_t.sim_mode_en, so the
 * operator has a single "simulation" switch rather than two that can disagree.
 * The breathing red bezel follows that setting already, so it lights up here
 * for free.
 *
 * SAFETY: the interlock lives in wspr_tx.c, which reads sim_mode_en directly
 * and sends no CAT bytes when it is set - the same split ft8_tx.c uses. This
 * module never needs to know about TX.
 *
 * ⚠ Every phantom callsign ends in SIM (VK7SIM, W1SIM, ...). They are valid
 * WSPR type-1 callsigns so the real packer accepts them, but nobody reading the
 * spot list can mistake them for real traffic - which matters, because these
 * decodes land in the same store as real ones.
 */

// True when simulation is switched on (reads the shared setting each call, so
// a change takes effect on the next cycle without a restart).
bool wspr_sim_enabled(void);

/* Build one simulated 120 s receive window into `pcm` (WSPR_SAMPLE_RATE_HZ,
 * mono, int16). `cycle_utc` seeds which phantoms transmit and how strongly, so
 * successive cycles differ the way a real band does rather than repeating.
 * Includes noise and, some cycles, a broadband interference burst. */
void wspr_sim_build_window(int16_t *pcm, long n, int64_t cycle_utc);

/* ---- synthesis primitives, shared with wspr_selftest.c ----
 * One implementation, because three copies of a 4-FSK synthesizer is how they
 * drift apart and stop testing the same thing. */

void wspr_sim_noise(int16_t *pcm, long n, float sigma);

// Add one continuous-phase 4-FSK WSPR transmission on top of what is already
// there. `start_s` is where in the window it begins (real stations start ~1 s
// in). Returns false if the message will not pack.
bool wspr_sim_add_station(int16_t *pcm, long n, const char *call, const char *grid,
                          int dbm, double f0_hz, double start_s, float amplitude);

// A broadband burst - what local QRM actually looks like, and the thing that
// broke the waterfall's contrast scaling when it appeared on the real band.
// Having it here makes that a reproducible case rather than a story.
void wspr_sim_add_burst(int16_t *pcm, long n, double t_s, double dur_s, float amplitude);
