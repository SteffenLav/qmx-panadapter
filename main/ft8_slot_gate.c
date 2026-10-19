/* See ft8_slot_gate.h for why this is portable and what it is guarding. */
#include "ft8_slot_gate.h"

int ft8_gate_slot_ms(ftx_protocol_t protocol)
{
    return ftx_protocol_slot_ms(protocol);
}

int ft8_gate_burst_ms(ftx_protocol_t protocol)
{
    /* Derived, so a new protocol gets a burst length without an entry here:
     * FT8 79 x 160 = 12640, FT4 105 x 48 = 5040, JS8 Turbo 79 x 50 = 3950. */
    return ftx_protocol_num_symbols(protocol) *
           (ftx_protocol_symbol_period_us(protocol) / 1000);
}

int ft8_gate_reply_window_ms(ftx_protocol_t protocol)
{
    int w = ft8_gate_slot_ms(protocol) - ft8_gate_burst_ms(protocol) - FT8_GATE_MARGIN_MS;
    return w < 0 ? 0 : w;
}

int ft8_gate_hold_deadline_ms(ftx_protocol_t protocol)
{
    /* A BAND below the window, not equal to it - see the header. Equal makes the
     * backstop reachable only at a single instant, which a 15 ms poll loop
     * essentially never hits. Later than the window would be unreachable
     * outright; much earlier would give up on a fresh reply sooner than needed. */
    int d = ft8_gate_reply_window_ms(protocol) - FT8_GATE_DEADLINE_BAND_MS;
    return d < 0 ? 0 : d;
}

bool ft8_gate_late_fire_enabled(ftx_protocol_t protocol)
{
    /* Every protocol. FT4 was excluded historically on the belief that its
     * shorter slot left no room; the arithmetic in the header says the opposite,
     * and it says the same for JS8 Turbo, whose 1950 ms window is the smallest
     * of the six and still six times the 300 ms deadline band.
     * What must never happen again is one gate answering differently from the
     * other, which is why both call THIS. */
    (void)protocol;
    return true;
}

bool ft8_gate_should_hold(const ft8_gate_boundary_t *g)
{
    if (!g) return false;
    if (!ft8_gate_late_fire_enabled(g->protocol)) return false;  /* nothing could fire it */
    return g->decode_in_flight && g->qso_busy && g->tx_would_run;
}

bool ft8_gate_should_late_fire(const ft8_gate_late_t *g)
{
    if (!g) return false;
    if (!ft8_gate_late_fire_enabled(g->protocol)) return false;
    if (!g->tx_should_run) return false;
    if (g->into_slot_ms < 0) return false;
    if (g->into_slot_ms > ft8_gate_reply_window_ms(g->protocol)) return false;

    /* A request that was never held may fire as soon as it is armed - that is
     * the reply-on-immediate-slot path. A held one waits for the decode to land,
     * or for the deadline, whichever comes first. */
    if (!g->held) return true;
    return g->decode_landed || g->into_slot_ms >= ft8_gate_hold_deadline_ms(g->protocol);
}
