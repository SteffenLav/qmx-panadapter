/* Host test for the FT8/FT4 slot-loop transmit gates.
 *
 * Build (from the repo root):
 *   gcc -I main -I components/ft8_lib -o ft8_slot_gate_harness \
 *       test/ft8_slot_gate_harness.c main/ft8_slot_gate.c \
 *       components/ft8_lib/ft8/constants.c components/ft8_lib/ft8/js8_tables.c \
 *       && ./ft8_slot_gate_harness
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
    /* ⭐ ALL SIX, not the two this harness used to cover. The gate is where a
     * protocol's slot arithmetic is decided, so every protocol the firmware can
     * select must be exercised here - JS8 Turbo's 1950 ms reply window is the
     * tightest of them and is exactly the case a bool could not express. */
    const ftx_protocol_t protos[] = {
        FTX_PROTOCOL_FT8, FTX_PROTOCOL_FT4, FTX_PROTOCOL_JS8,
        FTX_PROTOCOL_JS8_FAST, FTX_PROTOCOL_JS8_TURBO, FTX_PROTOCOL_JS8_SLOW,
    };
    const int NPROTO = (int)(sizeof(protos) / sizeof(protos[0]));

    /* ---------------------------------------------------------------- 1 ---
     * THE REGRESSION TEST. Exhaustive over every boundary input: if a hold is
     * possible for a protocol, a late fire must also be possible for it. */
    printf("1. a protocol that can be HELD can also LATE-FIRE\n");
    for (int p = 0; p < NPROTO; p++) {
        ftx_protocol_t proto = protos[p];
        int  holds  = 0;
        for (int m = 0; m < 8; m++) {
            ft8_gate_boundary_t b = {
                .protocol         = proto,
                .decode_in_flight = (m & 1) != 0,
                .qso_busy         = (m & 2) != 0,
                .tx_would_run     = (m & 4) != 0,
            };
            if (ft8_gate_should_hold(&b)) holds++;
        }
        /* Can anything fire a held request for this protocol? */
        int can_fire = 0;
        for (int t = 0; t <= ft8_gate_slot_ms(proto); t += 10) {
            ft8_gate_late_t l = { .protocol = proto, .held = true,
                                  .decode_landed = true, .tx_should_run = true,
                                  .into_slot_ms = t };
            if (ft8_gate_should_late_fire(&l)) { can_fire = 1; break; }
        }
        printf("   %s: %d of 8 boundary states hold, late fire reachable = %s\n",
               ftx_protocol_label(protos[p]), holds, can_fire ? "yes" : "NO");
        if (holds > 0 && !can_fire) {
            printf("   FAIL %s can be held but never fired - this is the v1.10.2 bug\n", ftx_protocol_label(protos[p]));
            fails++;
        }
        /* And the converse: if late fire is off, nothing may ever be held. */
        if (!ft8_gate_late_fire_enabled(proto)) ok("late-fire off implies never held", holds == 0);
    }

    /* ---------------------------------------------------------------- 2 ---
     * A burst fired at the last permitted moment must still land inside its
     * slot. The old shared 2800 ms window broke this for BOTH protocols. */
    printf("2. firing at the window edge does not overrun the slot\n");
    for (int p = 0; p < NPROTO; p++) {
        ftx_protocol_t proto = protos[p];
        int slot = ft8_gate_slot_ms(proto);
        int burst = ft8_gate_burst_ms(proto);
        int win  = ft8_gate_reply_window_ms(proto);
        int dead = ft8_gate_hold_deadline_ms(proto);
        printf("   %s: slot %d, burst %d, room %d, window %d, deadline %d\n",
               ftx_protocol_label(protos[p]), slot, burst, slot - burst, win, dead);
        ok("window + burst fits the slot",        win  + burst <= slot);
        ok("deadline + burst fits the slot",      dead + burst <= slot);
        ok("deadline is reachable within window", dead <= win);
        ok("window is positive",                  win > 0);
        /* The historical constant, kept as an explicit negative. 2800 ms was
         * looser than the room of EVERY protocol that existed when it was
         * retired, and the test simply asserted it would overrun.
         *
         * ⚠ THAT ASSERTION STOPPED BEING UNIVERSALLY TRUE when JS8 Slow
         * arrived: Slow has 4720 ms of room, so 2800 fits there comfortably.
         * The claim being guarded was never "2800 is always too big" - it was
         * "a single tabulated window cannot serve protocols with different
         * rooms, so it must stay derived". Stated that way it holds for six
         * protocols as it did for two, and it keeps teeth where it matters:
         * wherever 2800 WOULD have overrun, the derived window is smaller. */
        if (2800 + burst > slot)
            ok("2800 ms would have overrun, derived window does not", win < 2800);
        else
            ok("derived window never exceeds the room", win <= slot - burst);
    }

    /* ---------------------------------------------------------------- 3 ---
     * The hold needs all three conditions, and only those. */
    printf("3. hold requires decode-in-flight AND busy AND due\n");
    for (int p = 0; p < NPROTO; p++) {
        for (int m = 0; m < 8; m++) {
            ft8_gate_boundary_t b = {
                .protocol         = protos[p],
                .decode_in_flight = (m & 1) != 0,
                .qso_busy         = (m & 2) != 0,
                .tx_would_run     = (m & 4) != 0,
            };
            bool want = ft8_gate_late_fire_enabled(protos[p]) &&
                        b.decode_in_flight && b.qso_busy && b.tx_would_run;
            if (ft8_gate_should_hold(&b) != want) {
                printf("   FAIL %s m=%d got %d want %d\n", ftx_protocol_label(protos[p]), m,
                       ft8_gate_should_hold(&b), want);
                fails++;
            }
        }
    }

    /* ---------------------------------------------------------------- 4 ---
     * A HELD request waits for the decode, then fires; and fires anyway at the
     * deadline so a slot is never simply skipped. */
    printf("4. a held request waits, then fires - never skips the slot\n");
    for (int p = 0; p < NPROTO; p++) {
        ftx_protocol_t proto = protos[p];
        int  win = ft8_gate_reply_window_ms(proto);
        ft8_gate_late_t l = { .protocol = proto, .held = true, .tx_should_run = true };

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
    for (int p = 0; p < NPROTO; p++) {
        ftx_protocol_t proto = protos[p];
        int fired_at = -1, chances = 0;
        for (int t = 0; t <= ft8_gate_slot_ms(proto); t += FT8_GATE_POLL_MS) {
            ft8_gate_late_t l = { .protocol = proto, .held = true,
                                  .decode_landed = false, .tx_should_run = true,
                                  .into_slot_ms = t };
            if (ft8_gate_should_late_fire(&l)) {
                if (fired_at < 0) fired_at = t;
                chances++;
            }
        }
        printf("   %s: first chance at %d ms, %d polls could fire\n",
               ftx_protocol_label(protos[p]), fired_at, chances);
        ok("the deadline backstop is reachable at all", fired_at >= 0);
        /* One reachable poll is luck. Demand real slack, or a jittery loop
         * misses it and the slot is silently skipped. */
        ok("several polls fall inside the band", chances >= 5);
        if (fired_at >= 0)
            ok("and it still fits the slot",
               fired_at + ft8_gate_burst_ms(proto) <= ft8_gate_slot_ms(proto));
    }

    /* ---------------------------------------------------------------- 5 ---
     * FT4 has MORE room than FT8, which is the fact the v1.10.3 release note
     * muddled. Pin it so nobody "corrects" it back. */
    printf("5. FT4 has more room than FT8\n");
    /* ⚠ THESE READ `ft8_gate_slot_ms(false)` AND `(true)` UNTIL 2026-10-14,
     * and the day the argument became an enum they kept compiling: C converted
     * false to 0 and true to 1, which are FTX_PROTOCOL_FT4 and _FT8 - so the
     * line labelled "FT8 room" was measuring FT4 and vice versa. It was caught
     * only because the numbers then disagreed with the constants below. A
     * boolean that silently becomes an enum is a whole bug class; name the
     * protocol. */
    ok("FT4 room > FT8 room",
       (ft8_gate_slot_ms(FTX_PROTOCOL_FT4) - ft8_gate_burst_ms(FTX_PROTOCOL_FT4)) >
       (ft8_gate_slot_ms(FTX_PROTOCOL_FT8) - ft8_gate_burst_ms(FTX_PROTOCOL_FT8)));
    ok("FT8 room is 2360 ms",
       ft8_gate_slot_ms(FTX_PROTOCOL_FT8) - ft8_gate_burst_ms(FTX_PROTOCOL_FT8) == 2360);
    ok("FT4 room is 2460 ms",
       ft8_gate_slot_ms(FTX_PROTOCOL_FT4) - ft8_gate_burst_ms(FTX_PROTOCOL_FT4) == 2460);
    /* The submodes, as explicit figures rather than only as invariants: a
     * derivation that silently returned Normal's numbers would satisfy every
     * invariant above and be wrong. 79 symbols x the symbol period. */
    ok("JS8 Normal room is 2360 ms",
       ft8_gate_slot_ms(FTX_PROTOCOL_JS8) - ft8_gate_burst_ms(FTX_PROTOCOL_JS8) == 2360);
    ok("JS8 Fast room is 2100 ms",
       ft8_gate_slot_ms(FTX_PROTOCOL_JS8_FAST) - ft8_gate_burst_ms(FTX_PROTOCOL_JS8_FAST) == 2100);
    ok("JS8 Turbo room is 2050 ms",
       ft8_gate_slot_ms(FTX_PROTOCOL_JS8_TURBO) - ft8_gate_burst_ms(FTX_PROTOCOL_JS8_TURBO) == 2050);
    ok("JS8 Slow room is 4720 ms",
       ft8_gate_slot_ms(FTX_PROTOCOL_JS8_SLOW) - ft8_gate_burst_ms(FTX_PROTOCOL_JS8_SLOW) == 4720);

    /* ---------------------------------------------------------------- 6 ---
     * Nonsense inputs must be refused rather than guessed at. */
    printf("6. degenerate inputs\n");
    ok("NULL boundary",  !ft8_gate_should_hold(NULL));
    ok("NULL late",      !ft8_gate_should_late_fire(NULL));
    {
        ft8_gate_late_t l = { .protocol = FTX_PROTOCOL_FT8, .held = false, .tx_should_run = true,
                              .into_slot_ms = -1 };
        ok("negative time", !ft8_gate_should_late_fire(&l));
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall pass\n", fails);
    return fails ? 1 : 0;
}
