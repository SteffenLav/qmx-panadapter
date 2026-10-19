#pragma once
/* The FT8/FT4 slot loop's two transmit gates, as a pure decision.
 *
 * Portable on purpose (no ESP deps) so test/ft8_slot_gate_harness.c can link the
 * REAL functions. This exists because the decision was inline in ft8_test.c's
 * slot loop, could only be exercised with a live streaming radio, and shipped
 * broken twice:
 *
 *  - v1.10.2: the HOLD gate was opened to FT4 while the LATE-FIRE gate 180 lines
 *    below stayed FT8-only. An FT4 burst was held at the boundary and nothing
 *    was permitted to fire it, so FT4 stopped transmitting altogether. Reported
 *    by Gyula HA3HZ within hours.
 *  - The bench could not catch it: with the radio wedged there is no audio, so
 *    no decode job is ever queued, so the hold condition can never arise and the
 *    identical test passes on the broken build (#299).
 *
 * The invariant that makes that class impossible is stated here and enforced by
 * the harness: a protocol that may be HELD must also be allowed to LATE-FIRE.
 * Holding a burst that nothing may fire is a dead transmitter.
 *
 * The second invariant is arithmetic: firing at the latest permitted moment must
 * not overrun the slot. The old shared FT8_REPLY_TX_WINDOW_MS of 2800 ms broke
 * it for BOTH protocols (FT8 has 2360 ms of room, FT4 2460 ms); it rarely bit
 * only because decodes usually land about a second in.
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ⛔ THE SLOT AND SYMBOL CONSTANTS THAT USED TO LIVE HERE ARE GONE.
 *
 * They were FT8_GATE_FT8_SLOT_MS/SYMBOLS/SYMBOL_MS and the FT4 trio, selected
 * by a `bool is_ft4` running through every function in this file. That bool
 * could express exactly two protocols, and the JS8 submodes make six:
 *
 *   protocol    slot ms  symbols  symbol ms  burst ms  reply window
 *   FT4            7500      105         48      5040          2360
 *   FT8           15000       79        160     12640          2260
 *   JS8 Normal    15000       79        160     12640          2260
 *   JS8 Fast      10000       79        100      7900          2000
 *   JS8 Turbo      6000       79         50      3950          1950
 *   JS8 Slow      30000       79        320     25280          4620
 *
 * Left as a bool, every JS8 submode would have taken FT8's row - so Turbo
 * would have waited 15 s for a 6 s slot boundary and transmitted into the
 * wrong one. The facts now come from ftx_protocol_*(), which is the single
 * place a new protocol is described, and the arithmetic below is unchanged:
 * reply window = slot - burst - margin, still derived, never tabulated. */
#include "ft8/constants.h"

/* Slack left between the end of a burst and the end of its slot. */
#define FT8_GATE_MARGIN_MS          100

/* ⛔ THE DEADLINE MUST BE REACHABLE BY A DISCRETE POLLER, NOT JUST BY ALGEBRA.
 *
 * The late-fire test is `into >= deadline && into <= window`. Setting the two
 * equal makes that satisfiable at exactly ONE instant, and the slot loop samples
 * every FT8_GATE_POLL_MS - so the "fire whatever is armed rather than skip the
 * slot" backstop becomes unreachable in practice. Measured on hardware when I
 * did precisely that: FT4 held on seven consecutive transmit slots and fired
 * once.
 *
 * So the deadline sits a BAND below the window, wide enough that several polls
 * fall inside it. The harness models the poll loop rather than testing the
 * predicate at hand-picked instants, which is how it missed this. */
#define FT8_GATE_POLL_MS             15   /* the slot loop's vTaskDelay */
#define FT8_GATE_DEADLINE_BAND_MS   300   /* window - deadline, ~20 polls */

int ft8_gate_slot_ms(ftx_protocol_t protocol);
int ft8_gate_burst_ms(ftx_protocol_t protocol);

/* Latest moment into the slot at which a reply may still be fired, and the
 * moment a held request fires whatever it is carrying. Both are
 * slot - burst - margin, so a burst started at the limit still lands inside its
 * slot. FT4 has MORE room than FT8, not less (2460 vs 2360 ms before the
 * margin), because its burst shrinks faster than its slot does - and JS8 Slow
 * has the most of all (4720 ms), for the same reason in the other direction. */
int ft8_gate_reply_window_ms(ftx_protocol_t protocol);
int ft8_gate_hold_deadline_ms(ftx_protocol_t protocol);

/* ⛔ THE SINGLE SOURCE OF TRUTH for which protocols use the hold/late-fire pair.
 * Both gates below consult it, so it cannot be half-removed. */
bool ft8_gate_late_fire_enabled(ftx_protocol_t protocol);

typedef struct {
    ftx_protocol_t protocol;
    bool decode_in_flight;   /* jobs_done != jobs_queued */
    bool qso_busy;           /* ft8_qso_is_busy() - true throughout a CQ run */
    bool tx_would_run;       /* an armed request is due at this boundary */
} ft8_gate_boundary_t;

/* At the slot boundary: hold the armed request back, because the previous
 * slot's decode may supersede it? */
bool ft8_gate_should_hold(const ft8_gate_boundary_t *g);

typedef struct {
    ftx_protocol_t protocol;
    bool held;              /* ft8_gate_should_hold() said yes at the boundary */
    bool decode_landed;     /* jobs_done == jobs_queued now */
    bool tx_should_run;     /* the armed request matches this slot */
    int  into_slot_ms;
} ft8_gate_late_t;

/* Mid-slot: may the request fire now? */
bool ft8_gate_should_late_fire(const ft8_gate_late_t *g);

#ifdef __cplusplus
}
#endif
