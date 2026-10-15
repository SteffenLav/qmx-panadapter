/* Host test: the JS8 submode table and the ONE refusal rule.
 *
 * Build and run, from the repo root - tools/run_js8_submode_table.sh is
 * exactly this:
 *
 *   mkdir -p build/hosttest && cp -r test/stub/. build/hosttest/
 *   cp main/js8_submode.c main/js8_submode.h build/hosttest/
 *   gcc -O2 -Wall -Wextra -I build/hosttest  *       -o build/hosttest/js8_submode_table_harness  *       test/js8_submode_table_harness.c build/hosttest/js8_submode.c
 *   ./build/hosttest/js8_submode_table_harness
 *
 * ⚠ THE COPY IS NOT COSMETIC. For a quoted #include, GCC searches the
 * directory of the INCLUDING FILE first, ahead of every -I. Compiled where it
 * lives, main/js8_submode.c would pick up the real main/ft8_tx.h and
 * main/ft8_test.h and drag in half the firmware - which is exactly what the
 * first attempt did. Copying it next to the stubs is what puts the stubs
 * first. The copy is byte-identical: this tests the real file, not a
 * transcription of it.
 *
 * WHY THIS EXISTS. Until 2026-10-15 the submode's value list and its
 * "refused while a QSO or a burst is live" rule were written out three times:
 * the settings drawer (ui.c), /api/cmd (webserver.c) and - as the JS8 screen
 * got its own picker - very nearly a third. They are now in main/js8_submode.c
 * and all three controls call it, which makes that one function a single point
 * of failure for every way an operator can change the submode. This is the
 * test that it behaves.
 *
 * The stubs in test/stub/ shadow the firmware headers on the include path, so
 * the real js8_submode.c compiles unchanged on the host. They are stubs of the
 * things it CALLS, never of the thing under test.
 *
 * ⛔ WHAT THIS IS NOT EVIDENCE FOR. It says nothing about whether Fast, Turbo
 * or Slow work on the air - see test/js8_submode_harness.c for the codec
 * round-trip, and note that none of the three has ever been exchanged with a
 * real JS8Call station.
 */
#include <stdio.h>
#include <string.h>

#include "js8_submode.h"
#include "ft8_qso.h"
#include "ft8_tx.h"
#include "ft8_test.h"
#include "storage/settings.h"
#include "ui/ft8_screen_view.h"

/* ---- the stubbed world --------------------------------------------- */
static uint8_t         g_setting = 0;
static bool            g_qso_busy = false;
static ft8_tx_status_t g_tx = FT8_TX_IDLE;
static int             g_label_repaints = 0;

void    settings_set_js8_submode(uint8_t v) { g_setting = v; }
uint8_t settings_get_js8_submode(void)      { return g_setting; }
bool    ft8_qso_is_busy(char *o, size_t n)  { (void)o; (void)n; return g_qso_busy; }
ft8_tx_status_t ft8_tx_get_status(char *o, size_t n, int *p)
{ (void)o; (void)n; (void)p; return g_tx; }
void ft8_screen_view_refresh_mode_label(void) { g_label_repaints++; }
ftx_protocol_t ft8_op_mode_protocol(void) { return FTX_PROTOCOL_JS8; }
const char *ftx_protocol_label(ftx_protocol_t p) { (void)p; return "JS8"; }

/* ---- the test ------------------------------------------------------- */
static int fails = 0;
#define CHECK(cond, ...) do {                                   \
        if (cond) { printf("  ok   "); }                        \
        else      { printf("  FAIL "); fails++; }               \
        printf(__VA_ARGS__); printf("\n");                      \
    } while (0)

int main(void)
{
    printf("js8_submode_table_harness\n");

    /* 1. The table. JS8Call's NSUBMODE numbering is NOT dense - 3 does not
     *    exist and 8 is Ultra, which this firmware does not implement - so
     *    index and value must stay apart. */
    printf("\n1. the table\n");
    CHECK(js8_submode_count() == 4, "four submodes offered");
    const uint8_t want[4] = { 0, 1, 2, 4 };
    for (int i = 0; i < 4; i++)
        CHECK(js8_submode_value(i) == want[i],
              "index %d -> value %u", i, (unsigned)want[i]);
    CHECK(js8_submode_value(-1) == 0 && js8_submode_value(99) == 0,
          "an index out of range reads as Normal, not as memory");
    for (int i = 0; i < 4; i++)
        CHECK(js8_submode_index_of(want[i]) == i,
              "value %u -> index %d (round trip)", (unsigned)want[i], i);
    CHECK(js8_submode_index_of(3) == 0 && js8_submode_index_of(8) == 0,
          "NSUBMODE 3 and 8 are not ours: they read as Normal");

    /* 2. The labels carry the cycle length, which is the number an operator
     *    can act on, and the LVGL option string is built from the same rows -
     *    a drawer whose list disagreed with the picker's is the whole reason
     *    this table exists. */
    printf("\n2. the labels\n");
    CHECK(strcmp(js8_submode_label(0), "Normal - 15 s") == 0, "index 0 label");
    CHECK(strcmp(js8_submode_label(2), "Turbo - 6 s") == 0,  "index 2 label");
    const char *opts = js8_submode_options_lvgl();
    CHECK(strcmp(opts, "Normal - 15 s\nFast - 10 s\nTurbo - 6 s\nSlow - 30 s") == 0,
          "the LVGL option string is the same four labels, in order");
    CHECK(js8_submode_options_lvgl() == opts,
          "built once and cached, not rebuilt per call");

    /* 3. The refusal rule - the part three controls now share. */
    printf("\n3. the one refusal rule\n");
    const char *why;

    g_setting = 0; g_qso_busy = false; g_tx = FT8_TX_IDLE;
    why = (const char *)1;
    CHECK(js8_submode_set(2, &why), "idle: Turbo is accepted");
    CHECK(g_setting == 2, "and it actually reached the setting");
    CHECK(why == NULL, "and no reason is left behind on success");

    g_setting = 0;
    CHECK(!js8_submode_set(3, &why), "NSUBMODE 3 is refused, not rounded to Normal");
    CHECK(g_setting == 0, "and nothing was written");
    CHECK(why && strstr(why, "submode"), "the reason names the value: \"%s\"", why);

    /* ⛔ ARMED COUNTS, NOT JUST KEYED. An armed request already carries tones
     * built for the OLD submode. */
    g_setting = 0; g_qso_busy = false; g_tx = FT8_TX_ARMED;
    CHECK(!js8_submode_set(1, &why), "ARMED refuses - not only KEYED");
    CHECK(g_setting == 0, "and nothing was written");
    CHECK(why && strstr(why, "busy"), "the reason says busy: \"%s\"", why);

    g_tx = FT8_TX_KEYED;
    CHECK(!js8_submode_set(1, &why), "KEYED refuses");
    g_tx = FT8_TX_IDLE; g_qso_busy = true;
    CHECK(!js8_submode_set(1, &why), "a live QSO refuses");
    g_qso_busy = false;

    /* 4. The MODE label is the only thing on the screen that can explain why
     *    JS8 has stopped decoding, so a successful change must repaint it and
     *    a refused one must not pretend anything happened. */
    printf("\n4. the label follows the setting\n");
    g_label_repaints = 0; g_setting = 0;
    (void)js8_submode_set(4, &why);
    CHECK(g_label_repaints == 1, "accepted -> the MODE label is repainted once");
    g_label_repaints = 0;
    g_qso_busy = true;
    (void)js8_submode_set(0, &why);
    CHECK(g_label_repaints == 0, "refused  -> the label is left alone");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
