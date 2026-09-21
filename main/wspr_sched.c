// See wspr_sched.h.

#include "wspr_sched.h"

/* Period and the guards, in one place so all three entry points agree. */
static int sched_period(uint8_t tx_cycles, uint8_t rx_cycles)
{
    if (rx_cycles < 1) rx_cycles = 1;   /* 0 would key the radio continuously */
    return (int)tx_cycles + (int)rx_cycles;
}

/* Position within the period, always 0..period-1.
 *
 * C's % keeps the sign of the dividend, so a negative cycle_idx - which is what
 * an unset clock produces - would give a negative position and compare < tx as
 * "transmit". Folding it up front means the answer is well defined for every
 * input rather than only the ones we expect. */
static int sched_pos(int64_t cycle_idx, int period)
{
    int64_t pos = cycle_idx % (int64_t)period;
    if (pos < 0) pos += period;
    return (int)pos;
}

bool wspr_sched_is_tx_cycle(int64_t cycle_idx, uint8_t tx_cycles, uint8_t rx_cycles)
{
    if (tx_cycles == 0) return false;
    const int period = sched_period(tx_cycles, rx_cycles);
    return sched_pos(cycle_idx, period) < (int)tx_cycles;
}

uint8_t wspr_sched_burst_index(int64_t cycle_idx, uint8_t tx_cycles, uint8_t rx_cycles)
{
    if (tx_cycles == 0) return 0;
    const int period = sched_period(tx_cycles, rx_cycles);
    const int pos    = sched_pos(cycle_idx, period);
    return (pos < (int)tx_cycles) ? (uint8_t)(pos + 1) : 0;
}

int64_t wspr_sched_next_tx_cycle(int64_t from_cycle, uint8_t tx_cycles, uint8_t rx_cycles)
{
    if (tx_cycles == 0) return -1;
    const int period = sched_period(tx_cycles, rx_cycles);
    const int pos    = sched_pos(from_cycle, period);
    if (pos < (int)tx_cycles) return from_cycle;     /* already one */
    /* Past the group: the next period's first transmit cycle. */
    return from_cycle + (int64_t)(period - pos);
}
