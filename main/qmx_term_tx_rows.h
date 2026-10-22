#pragma once
#include <stdbool.h>
#include <stddef.h>

/* ⛔ WHICH ROWS OF THE RADIO'S OWN MENU PUT RF OUT (T65)
 *
 * Radio Menus holds a terminal session on the QMX's second serial port and
 * forwards keystrokes to it - arrows, Enter, and ANY single character - from
 * the browser and from the Tab5. Several items in the radio's Hardware tests
 * menu key the transmitter, and `qmx_term.h` has said so in prose since the
 * feature shipped:
 *
 *   "its siblings include SWR sweep, LPF sweep and PA mod. test, which KEY THE
 *    TRANSMITTER"
 *
 * Nothing enforced it. T64 closed the same class on the CAT port; this is the
 * terminal port, and it is the worse of the two - from the Diagnostics screen a
 * single `t` transmits AT FULL PA VOLTAGE WITH NO SWR PROTECTION (operating
 * manual 1_04_001 §8.6.10: "this transmit is done at the full PA voltage and
 * there is no SWR protection").
 *
 * ⛔ REFUSED, NOT BAND-GUARDED, and that is deliberate. The sweeps emit across
 * a whole filter passband - the RF filter sweep covers 20-40 MHz - so there is
 * no single frequency to hand tx_band_guard_allows(), and most of what they
 * emit is outside every amateur allocation by design. These are dummy-load
 * tools; the manual prints "USE DUMMY LOAD" in red beside each one. The radio's
 * own front panel still reaches all of them.
 *
 * ⚠ THIS LIST IS NOT PROVABLY COMPLETE. It is built from the 1_04_001
 * operating manual, section 8.6, cross-checked against every "dummy load"
 * warning in it - not from reading the menu off a radio. A label this does not
 * match is as unguarded as everything was before. Matching is therefore on the
 * broadest safe substring ("sweep" catches all five sweep tools) so that a
 * reworded label in a later firmware still trips it.
 */

/* row_text: one row of the terminal screen, as ansi_term_row_text() gives it.
 * why:      optional; set to a short reason when the row transmits.
 *
 * Returns true when entering this row can cause RF.
 */
bool qmx_term_row_transmits(const char *row_text, const char **why);

/* The Diagnostics screen's own transmit keys, which are not menu rows at all.
 * `t` keys at full PA voltage with no SWR protection and `p` asserts PTT, so
 * both are refused while that screen is up. Any other character is a field
 * edit somewhere and passes.
 *
 * key: the single character, as qmx_term_key() received it.
 */
bool qmx_term_key_transmits_on_diagnostics(char key, const char **why);
