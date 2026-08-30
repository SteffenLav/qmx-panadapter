/* Host test for the FT8/FT4 slot-loop transmit gates.
 *
 * Build (from the repo root):
 *   gcc -I main -o ft8_slot_gate_harness test/ft8_slot_gate_harness.c \
 *       main/ft8_slot_gate.c && ./ft8_slot_gate_harness
 *
 * Why this exists (#299): the gate decision lived inline in ft8_test.c's slot
 * loop, could only be exercised with a live streaming radio, and shipped broken.
 *
 * v1.10.2 opened the HOLD gate to FT4 and left the LATE-FIRE gate FT8-only, so
 * an FT4 burst was held at the boundary with nothing permitted to fire it. FT4
 * stopped transmitting entirely, on CQ and on a call alike, with the slot
 * countdown running normally. Gyula HA3HZ found it within hours of release.
 *
 * The bench could not have caught it: flashing wedges the QMX, no radio means no
 * audio, no audio means no decode job is ever queued, and with nothing in flight
 * the hold condition never arises - so the same test passes on the broken build.
 * That is what this file replaces. It needs no radio and no flash.
 *
 * The first test below is the one that matters: a protocol that may be HELD must
 * also be allowed to LATE-FIRE. Holding a burst nothing may fire is a dead
 * transmitter, and no amount of on-air testing reliably reproduces it.
 */
#include <stdio.h>
#include <string.h>
#include "ft8_slot_gate.h"

static int fails = 0;


static void ok(const char *what, int cond)
{
    if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

int main(void)
{
    const bool protos[2] = { false, true };   /* FT8, FT4 */
    const char *pname[2] = { "FT8", "FT4" };

    /* ---------------------------------------------------------------- 1 ---
     * THE REGRESSION TEST. Exhaustive over every boundary input: if a hold is
     * possible for a protocol, a late fire must also be possible for it. */
    printf("1. a protocol that can be HELD can also LATE-FIRE\n");
    for (int p = 0; p < 2; p++) {
        bool is_ft4 = protos[p];
        int  holds  = 0;
        for (int m = 0; m < 8; m++) {
            ft8_gate_boundary_t b = {
                .is_ft4           = is_ft4,
                .decode_in_flight = (m & 1) != 0,
                .qso_busy         = (m & 2) != 0,
                .tx_would_run     = (m & 4) != 0,
            };
            if (ft8_gate_should_hold(&b)) holds++;
        }
        /* Can anything fire a held request for this protocol? */
        int can_fire = 0;
        for (int t = 0; t <= ft8_gate_slot_ms(is_ft4); t += 10) {
            ft8_gate_late_t l = { .is_ft4 = is_ft4, .held = true,
                                  .decode_landed = true, .tx_should_run = true,
                                  .into_slot_ms = t };
            if (ft8_gate_should_late_fire(&l)) { can_fire = 1; break; }
        }
        printf("   %s: %d of 8 boundary states hold, late fire reachable = %s\n",
               pname[p], holds, can_fire ? "yes" : "NO");
        if (holds > 0 && !can_fire) {
            printf("   FAIL %s can be held but never fired - this is the v1.10.2 bug\n", pname[p]);
            fails++;
        }
        /* And the converse: if late fire is off, nothing may ever be held. */
        if (!ft8_gate_late_fire_enabled(is_ft4)) ok("late-fire off implies never held", holds == 0);
    }

    /* ---------------------------------------------------------------- 2 ---
     * A burst fired at the last permitted moment must still land inside its
     * slot. The old shared 2800 ms window broke this for BOTH protocols. */
    printf("2. firing at the window edge does not overrun the slot\n");
    for (int p = 0; p < 2; p++) {
        bool is_ft4 = protos[p];
        int slot = ft8_gate_slot_ms(is_ft4);
        int burst = ft8_gate_burst_ms(is_ft4);
        int win  = ft8_gate_reply_window_ms(is_ft4);
        int dead = ft8_gate_hold_deadline_ms(is_ft4);
        printf("   %s: slot %d, burst %d, room %d, window %d, deadline %d\n",
               pname[p], slot, burst, slot - burst, win, dead);
        ok("window + burst fits the slot",        win  + burst <= slot);
        ok("deadline + burst fits the slot",      dead + burst <= slot);
        ok("deadline is reachable within window", dead <= win);
        ok("window is positive",                  win > 0);
        /* The historical constant, kept as an explicit negative: 2800 ms was
         * looser than either protocol's room and must not come back. */
        ok("2800 ms would have overrun",          2800 + burst > slot);
    }

    /* ---------------------------------------------------------------- 3 ---
     * The hold needs all three conditions, and only those. */
    printf("3. hold requires decode-in-flight AND busy AND due\n");
    for (int p = 0; p < 2; p++) {
        for (int m = 0; m < 8; m++) {
            ft8_gate_boundary_t b = {
                .is_ft4           = protos[p],
                .decode_in_flight = (m & 1) != 0,
                .qso_busy         = (m & 2) != 0,
                .tx_would_run     = (m & 4) != 0,
            };
            bool want = ft8_gate_late_fire_enabled(protos[p]) &&
                        b.decode_in_flight && b.qso_busy && b.tx_would_run;
            if (ft8_gate_should_hold(&b) != want) {
                printf("   FAIL %s m=%d got %d want %d\n", pname[p], m,
                       ft8_gate_should_hold(&b), want);
                fails++;
            }
        }
    }

    /* ---------------------------------------------------------------- 4 ---
     * A HELD request waits for the decode, then fires; and fires anyway at the
     * deadline so a slot is never simply skipped. */
    printf("4. a held request waits, then fires - never skips the slot\n");
    for (int p = 0; p < 2; p++) {
        bool is_ft4 = protos[p];
        int  win = ft8_gate_reply_window_ms(is_ft4);
        ft8_gate_late_t l = { .is_ft4 = is_ft4, .held = true, .tx_should_run = true };

        l.decode_landed = false; l.into_slot_ms = 0;
        ok("held + nothing landed + t=0 -> wait", !ft8_gate_should_late_fire(&l));

        l.decode_landed = true;  l.into_slot_ms = 500;
        ok("held + decode landed -> fire",         ft8_gate_should_late_fire(&l));

        l.decode_landed = false; l.into_slot_ms = win;
        ok("held + deadline reached -> fire anyway", ft8_gate_should_late_fire(&l));

        l.decode_landed = true;  l.into_slot_ms = win + 1;
        ok("past the window -> never fire",        !ft8_gate_should_late_fire(&l));

        l.held = false; l.decode_landed = false; l.into_slot_ms = 200;
        ok("not held -> fires immediately",         ft8_gate_should_late_fire(&l));

        l.tx_should_run = false;
        ok("nothing armed -> no fire",             !ft8_gate_should_late_fire(&l));
    }

    /* ---------------------------------------------------------------- 4b --
     * ⛔ MODEL THE POLL LOOP, NOT THE PREDICATE.
     *
     * The slot loop samples every FT8_GATE_POLL_MS. Testing the predicate at
     * hand-picked instants (t == window, t == window+1) passes even when the
     * deadline is unreachable in practice - which is exactly what happened: I
     * set deadline == window, every algebraic test still passed, and on hardware
     * FT4 held on seven consecutive transmit slots and fired once.
     *
     * So sweep the slot the way the caller really does, and require that a HELD
     * request whose decode NEVER lands still gets away. */
    printf("4b. a held request whose decode never lands still fires (polled)\n");
    for (int p = 0; p < 2; p++) {
        bool is_ft4 = protos[p];
        int fired_at = -1, chances = 0;
        for (int t = 0; t <= ft8_gate_slot_ms(is_ft4); t += FT8_GATE_POLL_MS) {
            ft8_gate_late_t l = { .is_ft4 = is_ft4, .held = true,
                                  .decode_landed = false, .tx_should_run = true,
                                  .into_slot_ms = t };
            if (ft8_gate_should_late_fire(&l)) {
                if (fired_at < 0) fired_at = t;
                chances++;
            }
        }
        printf("   %s: first chance at %d ms, %d polls could fire\n",
               pname[p], fired_at, chances);
        ok("the deadline backstop is reachable at all", fired_at >= 0);
        /* One reachable poll is luck. Demand real slack, or a jittery loop
         * misses it and the slot is silently skipped. */
        ok("several polls fall inside the band", chances >= 5);
        if (fired_at >= 0)
            ok("and it still fits the slot",
               fired_at + ft8_gate_burst_ms(is_ft4) <= ft8_gate_slot_ms(is_ft4));
    }

    /* ---------------------------------------------------------------- 5 ---
     * FT4 has MORE room than FT8, which is the fact the v1.10.3 release note
     * muddled. Pin it so nobody "corrects" it back. */
    printf("5. FT4 has more room than FT8\n");
    ok("FT4 room > FT8 room",
       (ft8_gate_slot_ms(true)  - ft8_gate_burst_ms(true)) >
       (ft8_gate_slot_ms(false) - ft8_gate_burst_ms(false)));
    ok("FT8 room is 2360 ms", ft8_gate_slot_ms(false) - ft8_gate_burst_ms(false) == 2360);
    ok("FT4 room is 2460 ms", ft8_gate_slot_ms(true)  - ft8_gate_burst_ms(true)  == 2460);

    /* ---------------------------------------------------------------- 6 ---
     * Nonsense inputs must be refused rather than guessed at. */
    printf("6. degenerate inputs\n");
    ok("NULL boundary",  !ft8_gate_should_hold(NULL));
    ok("NULL late",      !ft8_gate_should_late_fire(NULL));
    {
        ft8_gate_late_t l = { .is_ft4 = false, .held = false, .tx_should_run = true,
                              .into_slot_ms = -1 };
        ok("negative time", !ft8_gate_should_late_fire(&l));
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall pass\n", fails);
    return fails ? 1 : 0;
}
