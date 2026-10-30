/* Host test: the DUTY CYCLE and the ported window arithmetic (T132).
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -o js8_alt_select_harness test/js8_alt_select_harness.c \
 *       main/js8_alt.c components/ft8_lib/ft8/constants.c \
 *       components/ft8_lib/ft8/js8_tables.c -lm
 *
 * WHY THIS EXISTS, ON TOP OF js8_alt_window_harness.c.
 *
 * That harness proved that an alternate submode CAN be decoded out of the
 * primary slot's capture, by synthesising a burst and requiring the real
 * decoder to find it. It says nothing about WHICH slots the firmware will
 * look at. Two things decide that and both are arithmetic that fails silently:
 *
 *   1. The window offsets, now ported into main/js8_alt.c so the firmware and
 *      the proven harness run the same calculation rather than two copies.
 *
 *   2. The duty cycle. A primary Normal slot is 15 s and a minute holds
 *      exactly four of them, so `slot_number % 4` IS the phase of the minute
 *      and never moves. A duty of one slot in four would therefore look at
 *      the same alternate window for ever - the feature would appear to work,
 *      the CPU cost would be right, and three quarters of the alternate
 *      traffic would be permanently unreachable. Nothing on the glass would
 *      say so. That is the defect this file exists to catch.
 *
 * It also pins the COVERAGE CEILING, which is a property of the slot grids and
 * not of any choice we made: a Fast slot that straddles a Normal boundary is
 * in neither capture, so Fast coverage is 4 of the 6 Fast slots per minute no
 * matter what the duty cycle is. Written down as a test so it is never
 * re-derived from intuition and never reported as a bug.
 *
 * ⚠ NOT INTEROP EVIDENCE, and nothing here touches the decoder. See
 * js8_alt_window_harness.c for what the decode path does and does not prove.
 */
#include <stdio.h>
#include <string.h>

#include "js8_alt.h"

static int g_fail = 0;

static void expect_int(const char *what, int got, int want)
{
    if (got != want) g_fail++;
    printf("  %s %-58s got %d, want %d\n",
           (got == want) ? "ok  " : "FAIL", what, got, want);
}

static void expect_true(const char *what, bool got, bool want)
{
    if (got != want) g_fail++;
    printf("  %s %-58s got %s, want %s\n",
           (got == want) ? "ok  " : "FAIL", what,
           got ? "true" : "false", want ? "true" : "false");
}

/* ---- 1. the ported window arithmetic ------------------------------------
 *
 * The same expectations js8_alt_window_harness.c established against the real
 * decoder. Repeated here against the PORTED function, because a port is a
 * second copy until something checks it. */
static void test_windows(void)
{
    int offs[JS8_ALT_MAX_WINDOWS];
    printf("ported window arithmetic (15 s primary):\n");

    for (int t0 = 0; t0 < 60; t0 += 15) {
        char lbl[72];
        snprintf(lbl, sizeof(lbl), "Fast  slots inside primary t0=%d", t0);
        expect_int(lbl, js8_alt_slots_in_primary(t0, 15, 10, offs,
                                                 JS8_ALT_MAX_WINDOWS), 1);
    }
    for (int t0 = 0; t0 < 60; t0 += 15) {
        char lbl[72];
        snprintf(lbl, sizeof(lbl), "Turbo slots inside primary t0=%d", t0);
        expect_int(lbl, js8_alt_slots_in_primary(t0, 15, 6, offs,
                                                 JS8_ALT_MAX_WINDOWS), 2);
    }
    for (int t0 = 0; t0 < 60; t0 += 15) {
        char lbl[72];
        snprintf(lbl, sizeof(lbl), "Slow  slots inside primary t0=%d (never)", t0);
        expect_int(lbl, js8_alt_slots_in_primary(t0, 15, 30, offs,
                                                 JS8_ALT_MAX_WINDOWS), 0);
    }

    /* The offsets themselves, not just the count. A count that is right with
     * the offsets wrong is exactly the silent failure this is about. */
    js8_alt_slots_in_primary(0, 15, 10, offs, JS8_ALT_MAX_WINDOWS);
    expect_int("Fast offset in primary t0=0  (samples)",  offs[0], 0);
    js8_alt_slots_in_primary(15, 15, 10, offs, JS8_ALT_MAX_WINDOWS);
    expect_int("Fast offset in primary t0=15 (samples)",  offs[0], 5 * JS8_ALT_SR_HZ);
    js8_alt_slots_in_primary(30, 15, 10, offs, JS8_ALT_MAX_WINDOWS);
    expect_int("Fast offset in primary t0=30 (samples)",  offs[0], 0);
    js8_alt_slots_in_primary(45, 15, 10, offs, JS8_ALT_MAX_WINDOWS);
    expect_int("Fast offset in primary t0=45 (samples)",  offs[0], 5 * JS8_ALT_SR_HZ);

    js8_alt_slots_in_primary(0, 15, 6, offs, JS8_ALT_MAX_WINDOWS);
    expect_int("Turbo offsets in primary t0=0,  first",  offs[0], 0);
    expect_int("Turbo offsets in primary t0=0,  second", offs[1], 6 * JS8_ALT_SR_HZ);
    js8_alt_slots_in_primary(15, 15, 6, offs, JS8_ALT_MAX_WINDOWS);
    expect_int("Turbo offsets in primary t0=15, first",  offs[0], 3 * JS8_ALT_SR_HZ);
    expect_int("Turbo offsets in primary t0=15, second", offs[1], 9 * JS8_ALT_SR_HZ);

    /* ⛔ EVERY WINDOW MUST LIE WHOLLY INSIDE THE CAPTURE. The mutation test in
     * js8_alt_window_harness.c segfaulted on its first run by running a Fast
     * window past the end of a 15 s buffer; the arithmetic that prevents that
     * is here, so the bound is asserted here too. */
    for (int t0 = 0; t0 < 60; t0 += 15) {
        for (int alt_s = 6; alt_s <= 10; alt_s += 4) {
            int n = js8_alt_slots_in_primary(t0, 15, alt_s, offs,
                                             JS8_ALT_MAX_WINDOWS);
            for (int i = 0; i < n; i++) {
                char lbl[72];
                snprintf(lbl, sizeof(lbl),
                         "alt %d s window %d at t0=%d ends inside the capture",
                         alt_s, i, t0);
                expect_true(lbl,
                            offs[i] + alt_s * JS8_ALT_SR_HZ <= 15 * JS8_ALT_SR_HZ,
                            true);
            }
        }
    }
    printf("\n");
}

/* ---- 2. the duty cycle must WALK the phase ------------------------------- */
static void test_duty_phase(void)
{
    printf("duty cycle (every %d primary slots):\n", JS8_ALT_DUTY_EVERY);

    /* ⛔ THE TEST THAT MATTERS. Over a long run, the alternate slots must
     * visit all four phases of the minute. With JS8_ALT_DUTY_EVERY == 4 this
     * comes back 1 and the feature is three-quarters blind. */
    bool phase_seen[4] = { false, false, false, false };
    int  n_due = 0;
    const int SLOTS = 4 * 60;                 /* one hour of 15 s slots */
    for (int n = 0; n < SLOTS; n++) {
        int64_t slot_sec = (int64_t)n * 15;
        if (!js8_alt_slot_due(slot_sec, 15)) continue;
        n_due++;
        phase_seen[(slot_sec % 60) / 15] = true;
    }
    int n_phases = 0;
    for (int i = 0; i < 4; i++) if (phase_seen[i]) n_phases++;
    expect_int("phases of the minute visited in an hour", n_phases, 4);

    /* And the rate is what the budget was costed at. */
    expect_int("alternate slots per hour", n_due, SLOTS / JS8_ALT_DUTY_EVERY);

    /* All four phases inside 20 slots - 5 minutes - not merely eventually. */
    memset(phase_seen, 0, sizeof(phase_seen));
    for (int n = 0; n < 20; n++) {
        int64_t slot_sec = (int64_t)n * 15;
        if (js8_alt_slot_due(slot_sec, 15)) phase_seen[(slot_sec % 60) / 15] = true;
    }
    n_phases = 0;
    for (int i = 0; i < 4; i++) if (phase_seen[i]) n_phases++;
    expect_int("phases visited in the first 20 slots", n_phases, 4);
    printf("\n");
}

/* ---- 3. selection: what is refused, and why ------------------------------ */
static void test_select(void)
{
    int offs[JS8_ALT_MAX_WINDOWS], n = 0;
    ftx_protocol_t alt = FTX_PROTOCOL_FT8;

    printf("selection:\n");

    /* Off is off, on a slot that is otherwise due. */
    expect_true("OFF selects nothing",
                js8_alt_select(FTX_PROTOCOL_JS8, JS8_ALT_OFF, 0,
                               &alt, offs, JS8_ALT_MAX_WINDOWS, &n), false);

    /* The primary is not JS8 - FT8 and FT4 have no submodes to look for. */
    expect_true("FT8 primary selects nothing",
                js8_alt_select(FTX_PROTOCOL_FT8, 1, 0,
                               &alt, offs, JS8_ALT_MAX_WINDOWS, &n), false);

    /* Asking for the submode we are already operating. */
    expect_true("Normal alternate under a Normal primary is refused",
                js8_alt_select(FTX_PROTOCOL_JS8, 0, 0,
                               &alt, offs, JS8_ALT_MAX_WINDOWS, &n), false);

    /* ⛔ SLOW IS EXCLUDED BY ARITHMETIC, not deferred: 30 s never fits in a
     * 15 s capture, in any phase. */
    expect_true("Slow alternate under a Normal primary is refused",
                js8_alt_select(FTX_PROTOCOL_JS8, 4, 0,
                               &alt, offs, JS8_ALT_MAX_WINDOWS, &n), false);

    /* A due slot with Fast selected. */
    expect_true("Fast on a due slot is selected",
                js8_alt_select(FTX_PROTOCOL_JS8, 1, 0,
                               &alt, offs, JS8_ALT_MAX_WINDOWS, &n), true);
    expect_int("  ... as FTX_PROTOCOL_JS8_FAST", (int)alt,
               (int)FTX_PROTOCOL_JS8_FAST);
    expect_int("  ... one window", n, 1);

    /* A slot that is not due. slot 1 (t=15 s) is never due at modulus 5. */
    expect_true("Fast on a slot that is not due is refused",
                js8_alt_select(FTX_PROTOCOL_JS8, 1, 15,
                               &alt, offs, JS8_ALT_MAX_WINDOWS, &n), false);

    /* Under a Slow primary everything shorter fits, including Normal. */
    expect_true("Normal under a Slow primary fits",
                js8_alt_fits(FTX_PROTOCOL_JS8_SLOW, FTX_PROTOCOL_JS8), true);
    expect_true("Fast under a Turbo primary does NOT fit",
                js8_alt_fits(FTX_PROTOCOL_JS8_TURBO, FTX_PROTOCOL_JS8_FAST), false);
    printf("\n");
}

/* ---- 4. the coverage ceiling --------------------------------------------
 *
 * Which of the six Fast slots in a minute can EVER be decoded out of a Normal
 * capture? Not a choice - a property of the two grids. Asserted so that
 * "Fast misses :10 and :40" is a recorded fact rather than a field report. */
static void test_coverage(void)
{
    printf("coverage ceiling (Fast slots reachable from a 15 s capture):\n");

    bool reachable[6];
    memset(reachable, 0, sizeof(reachable));
    int offs[JS8_ALT_MAX_WINDOWS];
    for (int t0 = 0; t0 < 60; t0 += 15) {
        int n = js8_alt_slots_in_primary(t0, 15, 10, offs, JS8_ALT_MAX_WINDOWS);
        for (int i = 0; i < n; i++) reachable[(t0 + offs[i] / JS8_ALT_SR_HZ) / 10] = true;
    }
    int n_reach = 0;
    for (int i = 0; i < 6; i++) {
        printf("    Fast slot at :%02d  %s\n", i * 10,
               reachable[i] ? "reachable" : "NEVER (straddles a Normal boundary)");
        if (reachable[i]) n_reach++;
    }
    expect_int("Fast slots reachable per minute", n_reach, 4);
    expect_true("  ... :10 is not one of them", reachable[1], false);
    expect_true("  ... :40 is not one of them", reachable[4], false);

    /* Turbo, both windows taken. */
    bool treach[10];
    memset(treach, 0, sizeof(treach));
    for (int t0 = 0; t0 < 60; t0 += 15) {
        int n = js8_alt_slots_in_primary(t0, 15, 6, offs, JS8_ALT_MAX_WINDOWS);
        for (int i = 0; i < n; i++) treach[(t0 + offs[i] / JS8_ALT_SR_HZ) / 6] = true;
    }
    int n_t = 0;
    for (int i = 0; i < 10; i++) if (treach[i]) n_t++;
    expect_int("Turbo slots reachable per minute (of 10)", n_t, 8);
    printf("\n");
}

/* ---- 5. MUTATION TESTS ---------------------------------------------------
 *
 * Everything above passes on a first clean run, which is when a harness is
 * least trustworthy: a js8_alt_slots_in_primary() that returned a plausible
 * number for anything, or a js8_alt_select() that said yes to everything,
 * would look identical from here. So require specific WRONG answers to be
 * rejected - the same discipline that made js8_alt_window_harness.c worth
 * believing, where a window shifted by half an alternate slot had to come
 * back silent and did.
 *
 * ⚠ These assert the NEGATIVE: each line names a plausible mis-implementation
 * and requires the code not to behave that way. */
static void test_mutations(void)
{
    int offs[JS8_ALT_MAX_WINDOWS];
    printf("mutation tests - plausible wrong answers must be rejected:\n");

    /* (a) "round the start DOWN to the alternate grid" - the off-by-one that
     * puts the window before the capture begins and reads garbage from the
     * previous slot. At t0=15 the first Fast slot at or after 15 is 20, NOT
     * 10, so the offset must be +5 s and can never be negative. */
    js8_alt_slots_in_primary(15, 15, 10, offs, JS8_ALT_MAX_WINDOWS);
    expect_true("(a) a window never starts before the capture", offs[0] >= 0, true);
    expect_true("(a) Fast at t0=15 is NOT at offset 0 (round-down bug)",
                offs[0] == 0, false);

    /* (b) "a window may end past the capture" - the condition written as
     * `t <= t0 + primary_s` instead of `t + alt_s <= t0 + primary_s`. That
     * version returns 2 Fast windows at t0=0, not 1. */
    expect_true("(b) Fast at t0=0 is not counted twice",
                js8_alt_slots_in_primary(0, 15, 10, offs,
                                         JS8_ALT_MAX_WINDOWS) == 2, false);

    /* (c) "Slow nearly fits" - 30 s in 15 s is zero, not one. Already checked
     * above; repeated as a mutation because an implementation that clamped a
     * partial window to the buffer would pass the count test by accident. */
    expect_int("(c) Slow in a 15 s primary stays at zero windows",
               js8_alt_slots_in_primary(0, 15, 30, offs,
                                        JS8_ALT_MAX_WINDOWS), 0);

    /* (d) THE PHASE LOCK. A duty of 4 - the obvious choice, and the one the
     * T132 row names - visits exactly one phase of the minute. Computed here
     * with a literal 4 rather than the constant, so this stays a statement
     * about the arithmetic even if the constant changes. */
    {
        bool seen[4] = { false, false, false, false };
        for (int n = 0; n < 240; n++) {
            int64_t s = (int64_t)n * 15;
            if ((s / 15) % 4 == 0) seen[(s % 60) / 15] = true;
        }
        int k = 0;
        for (int i = 0; i < 4; i++) if (seen[i]) k++;
        expect_int("(d) a duty of 4 would visit only this many phases", k, 1);
        expect_true("(d) ... so JS8_ALT_DUTY_EVERY must not be 4",
                    JS8_ALT_DUTY_EVERY == 4, false);
    }

    /* (e) "select says yes on every slot" - the duty check dropped. One
     * minute of Normal slots must contain fewer due slots than slots. */
    {
        int due = 0;
        for (int n = 0; n < 20; n++)
            if (js8_alt_select(FTX_PROTOCOL_JS8, 1, (int64_t)n * 15,
                               NULL, offs, JS8_ALT_MAX_WINDOWS, NULL)) due++;
        expect_int("(e) due slots in 20 (not 20, not 0)", due,
                   20 / JS8_ALT_DUTY_EVERY);
    }

    /* (f) the mark column must be BLANK for the primary, or an ordinary
     * single-submode session grows a column of letters it cannot explain. */
    expect_int("(f) the mark for the primary submode is a space",
               js8_alt_mark(FTX_PROTOCOL_JS8, FTX_PROTOCOL_JS8), ' ');
    expect_int("(f) the mark for Fast under Normal is 'F'",
               js8_alt_mark(FTX_PROTOCOL_JS8_FAST, FTX_PROTOCOL_JS8), 'F');
    expect_int("(f) the mark for Normal under a Fast primary is 'N'",
               js8_alt_mark(FTX_PROTOCOL_JS8, FTX_PROTOCOL_JS8_FAST), 'N');
    printf("\n");
}

int main(void)
{
    printf("T132: alternate-submode selection and duty cycle\n\n");
    test_windows();
    test_duty_phase();
    test_select();
    test_coverage();
    test_mutations();
    printf("%s\n", g_fail ? "FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
