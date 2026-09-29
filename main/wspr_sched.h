#pragma once
#include <stdbool.h>
#include <stdint.h>

/* WSPR transmit schedule, anchored to UTC.
 *
 * WHY THIS EXISTS. The schedule used to be RELATIVE: the next group was rolled
 * from wherever the last one ended, so it meant "20 minutes after the last one"
 * rather than "minutes 0 and 2 of every 20". Those agree right up until
 * something re-anchors the schedule - a missed cycle, a clock step, a restart,
 * a settings change - and from then on they never agree again. John W5JSS
 * watched his schedule move from the 0 and 2 minute marks to 2 and 4 after a
 * few hours, and nothing in the log said why.
 *
 * Anchoring to the cycle index removes the whole class: the answer depends only
 * on UTC and the two counts, so it cannot drift, cannot accumulate error, and
 * is the same after a reboot as before it. An operator can read their settings
 * and say which minutes they will transmit on.
 *
 * A WSPR cycle is 2 minutes and starts on an even UTC minute, so
 * cycle_idx = utc_seconds / 120.
 *
 * Pure and dependency-free so test/wspr_sched_harness.c can link it directly -
 * this decides when a transmitter keys, which is not something to verify by
 * watching a beacon for an afternoon. */

/* True when this cycle is a transmit cycle.
 *
 * The period is tx_cycles + rx_cycles, and the transmit cycles are the FIRST
 * tx_cycles of each period, so a group is contiguous - which is what the UI
 * describes ("2 tx + 8 rx = 20 min").
 *
 * tx_cycles == 0 means never transmit. rx_cycles == 0 is treated as 1: zero
 * would make the period equal the burst count and key the radio continuously,
 * which this project has done on the bench once already. Negative cycle_idx is
 * handled so a clock that has not been set yet cannot produce a negative
 * modulus and transmit on the wrong cycle. */
bool wspr_sched_is_tx_cycle(int64_t cycle_idx, uint8_t tx_cycles, uint8_t rx_cycles);

/* Which burst within its group this cycle is, 1-based; 0 when not a TX cycle.
 * The UI says "transmit 1 of 2 in this group", and deriving that from the cycle
 * index rather than a running counter means it cannot disagree with what
 * actually goes on the air. */
uint8_t wspr_sched_burst_index(int64_t cycle_idx, uint8_t tx_cycles, uint8_t rx_cycles);

/* The next transmit cycle at or after `from_cycle`. Used for the countdown, so
 * it points at a real burst rather than the next mere opportunity. Returns
 * from_cycle itself when that is already a TX cycle; -1 when tx_cycles == 0. */
int64_t wspr_sched_next_tx_cycle(int64_t from_cycle, uint8_t tx_cycles, uint8_t rx_cycles);
