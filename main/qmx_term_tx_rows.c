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
    { "Diagnostics","the Diagnostics screen can key at full PA voltage with no SWR protection" },
    { "Beacon",     "the radio's own beacon (Virtual U3S) transmits unattended" },
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
