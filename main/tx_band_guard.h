#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ⛔ REFUSE TO KEY THE RADIO OUTSIDE THE EFFECTIVE REGION'S ALLOCATION (T48).
 *
 * WHY IT EXISTS: nothing checked at all. util/bandplan.c drove the display
 * strip, the DX cluster and the web status, and never refused a transmission -
 * which is how a WSPR 60 m dial that emitted 63 kHz below the bottom of the
 * band shipped and ran unattended (Wolfgang DF1KZ, 2026-10-21; see
 * project_wspr_60m_was_out_of_band_every_region). WSPR is one of the six sites
 * that key the radio with nobody deciding.
 *
 * ONE function, called from both digital TX paths immediately before key-down,
 * so the two cannot drift apart in what they allow.
 *
 * ⛔ IT TAKES THE RF ACTUALLY EMITTED, NOT THE DIAL. For a digital mode the
 * carrier is dial + audio, so the dial alone is the wrong number twice over: a
 * dial just under the band edge with 2 kHz of audio is legitimately inside,
 * and a dial at the top edge is not. Both call sites already compute the
 * emitted RF for their own logging, so the right number was already in hand.
 *
 * ⛔ NO REGION MEANS NO TRANSMISSION. Operator's call, 2026-10-21: "the
 * region is just as important as the call sign and grid to do any tx'ing" -
 * and both paths already refuse without those two, so this joins that list.
 * It is also the safer of the two wrong answers: letting an unknown region
 * through leaves the hole this guard exists to close, while refusing makes the
 * gap visible and fixable in one setting.
 *
 * ⚠ TWO cases still pass unchecked, because refusing would be a guess:
 *
 *   - the operator switched it off (settings: tx_band_guard_off). bandplan.h
 *     says its own numbers are "an operator hint, not a legal authority", and
 *     one was wrong by 500 Hz at exactly the edge WSPR uses. Special and
 *     experimental licences have access this table does not model.
 *   - the dial reads 0, i.e. CAT has not told us where the radio is. A guard
 *     that refuses when it does not know the frequency would block every
 *     transmission on a flaky link, and that link failure has its own
 *     reporting already (wspr_tx.c verifies its key-down).
 */

/* rf_lo_hz / rf_hi_hz: the lowest and highest RF the burst will emit.
 * what:                "WSPR" / "FT8" etc, for the log and the toast.
 * err/err_len:         optional; on a refusal, what to show the operator.
 *
 * Returns true when it is safe to key. On false the caller MUST NOT transmit.
 */
bool tx_band_guard_allows(uint32_t rf_lo_hz, uint32_t rf_hi_hz,
                          const char *what, char *err, size_t err_len);
