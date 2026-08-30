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

/* Timing, derived rather than tabulated, so the invariants can be checked
 * against the real burst lengths instead of against remembered constants. */
#define FT8_GATE_FT8_SLOT_MS      15000
#define FT8_GATE_FT8_SYMBOLS         79
#define FT8_GATE_FT8_SYMBOL_MS      160   /* 79 x 160 ms = 12 640 ms on air */

#define FT8_GATE_FT4_SLOT_MS       7500
#define FT8_GATE_FT4_SYMBOLS        105
#define FT8_GATE_FT4_SYMBOL_MS       48   /* 105 x 48 ms = 5 040 ms on air */

/* Slack left between the end of a burst and the end of its slot. */
#define FT8_GATE_MARGIN_MS          100

int ft8_gate_slot_ms(bool is_ft4);
int ft8_gate_burst_ms(bool is_ft4);

/* Latest moment into the slot at which a reply may still be fired, and the
 * moment a held request fires whatever it is carrying. Both are
 * slot - burst - margin, so a burst started at the limit still lands inside its
 * slot. FT4 has MORE room than FT8, not less (2460 vs 2360 ms), because its
 * burst shrinks faster than its slot does. */
int ft8_gate_reply_window_ms(bool is_ft4);
int ft8_gate_hold_deadline_ms(bool is_ft4);

/* ⛔ THE SINGLE SOURCE OF TRUTH for which protocols use the hold/late-fire pair.
 * Both gates below consult it, so it cannot be half-removed. */
bool ft8_gate_late_fire_enabled(bool is_ft4);

typedef struct {
    bool is_ft4;
    bool decode_in_flight;   /* jobs_done != jobs_queued */
    bool qso_busy;           /* ft8_qso_is_busy() - true throughout a CQ run */
    bool tx_would_run;       /* an armed request is due at this boundary */
} ft8_gate_boundary_t;

/* At the slot boundary: hold the armed request back, because the previous
 * slot's decode may supersede it? */
bool ft8_gate_should_hold(const ft8_gate_boundary_t *g);

typedef struct {
    bool is_ft4;
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
