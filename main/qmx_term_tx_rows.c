/* See the header for why this exists and why it refuses rather than guards.
 *
 * No ESP dependencies: test/qmx_term_tx_rows_harness.c compiles it on the host.
 */
#include "qmx_term_tx_rows.h"

#include <ctype.h>
#include <string.h>

/* Case-insensitive substring. The terminal pads and indents its rows, and a
 * later firmware may capitalise a label differently, so neither spacing nor
 * case may decide whether the transmitter is allowed to key. */
static bool contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

/* Every match is a tool the 1_04_001 operating manual marks "USE DUMMY LOAD",
 * or that it describes as driving the PA. The reason travels with the entry so
 * a refusal can say which one it is - a bare "refused" would send the operator
 * looking for a fault.
 *
 * "sweep" on its own covers Audio filter sweep, RF filter sweep, Image sweep,
 * SWR sweep and LPF sweep: EVERY sweep tool in this radio emits, either from
 * the internal signal generator into the RF port or from the PA itself. A
 * broad match is the safe direction here. */
static const struct { const char *needle; const char *why; } TX_ROWS[] = {
    { "sweep",      "the sweep tools drive the signal generator or the PA into the antenna socket" },
    { "ADC I/Q",    "Test ADC I/Q injects a signal into the RF front end" },
    { "SSB Cal",    "SSB Calibration transmits and needs a dummy load" },
    { "PA mod",     "the PA modulator test drives the PA" },
    /* ⛔ "Diagnostics" IS NOT IN THIS LIST, AND THAT IS DELIBERATE.
     *
     * It was, from T65 (v1.20.4) until 2026-10-24, and it was wrong. ENTERING
     * the Diagnostics screen emits nothing - it is a read-out. What transmits
     * is `t` and `p` ONCE THAT SCREEN IS UP (manual 1_04_001 §8.6.10), and
     * those are refused by qmx_term_key_transmits_on_diagnostics(), which is a
     * separate check that does not depend on this list at all. So the row
     * entry blocked nothing that the key guard was not already blocking.
     *
     * What it did block was the screen itself. Randy N4OPI reads his remote
     * station's battery voltage there regularly and reported it dead on two
     * Tab5s on 2026-10-24: Hardware tests > Diagnostics > Enter, and nothing
     * happens. A guard that costs a read-only screen and buys no safety is a
     * defect, not caution.
     *
     * ⚠ Do not "restore" it. If a future firmware makes entry itself key, the
     * evidence for that goes here first. */
    /* ⛔ BOTH SPELLINGS, AND THE SECOND ONE IS THE REAL MENU.
     *
     * "Beacon" came from the operating manual's LCD main-menu list. On
     * 2026-10-22 I opened the terminal on a 1_04_010 QMX+ and the row is
     * "Virtual U3S" - so the list I had just shipped would have let Enter
     * through on the radio's own unattended beacon. Exactly the limitation
     * this file's header warns about, found the first time a real menu was
     * read. Keep both: the LCD and the terminal word it differently. */
    { "Beacon",     "the radio's own beacon transmits unattended" },
    { "U3S",        "Virtual U3S is the radio's own beacon - it transmits unattended" },
};

bool qmx_term_row_transmits(const char *row_text, const char **why)
{
    if (why) *why = NULL;
    if (!row_text) return false;
    for (unsigned i = 0; i < sizeof(TX_ROWS) / sizeof(TX_ROWS[0]); i++) {
        if (contains_ci(row_text, TX_ROWS[i].needle)) {
            if (why) *why = TX_ROWS[i].why;
            return true;
        }
    }
    return false;
}

bool qmx_term_key_transmits_on_diagnostics(char key, const char **why)
{
    if (why) *why = NULL;
    /* Manual §8.6.10: "p Enable PTT", "t Enable PTT and transmit". Upper case
     * too - the manual's own prose says "When you press P or T". */
    if (key == 't' || key == 'T') {
        if (why) *why = "t transmits at full PA voltage with no SWR protection";
        return true;
    }
    if (key == 'p' || key == 'P') {
        if (why) *why = "p asserts PTT";
        return true;
    }
    return false;
}
