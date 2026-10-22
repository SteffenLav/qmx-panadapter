/* Host harness for qmx_term_row_transmits() - T65.
 *
 *   gcc -I main -o /tmp/qtr test/qmx_term_tx_rows_harness.c main/qmx_term_tx_rows.c
 *   /tmp/qtr
 *
 * The rows below are written the way the radio's terminal draws them - indented
 * inside a box, with the padding - because that padding is exactly what a
 * naive strcmp would trip over.
 *
 * Asserts rather than prints: a false PASS here keys a transmitter, and on the
 * Diagnostics screen that is at full PA voltage with no SWR protection.
 *
 * ⚠ The REFUSE list is from the 1_04_001 operating manual §8.6, cross-checked
 * against every "dummy load" warning in it. It is not from reading the menu off
 * a radio, so a label it does not match is unguarded - which is why the
 * matching is broad.
 */
#include <stdio.h>
#include <string.h>

#include "qmx_term_tx_rows.h"

struct row { const char *text; bool tx; const char *note; };
struct key { char k; bool tx; const char *note; };

int main(void)
{
    static const struct row rows[] = {
      /* --- every sweep tool emits, from the sig gen or from the PA -------- */
      {"|  Audio filter sweep    |", true,  "sig gen into the RF port"},
      {"|  RF filter sweep       |", true,  "sweeps 20-40 MHz"},
      {"|  Image sweep           |", true,  ""},
      {"|  SWR sweep             |", true,  "keys at Tune % of full power"},
      {"|  LPF sweep             |", true,  "transmits at reduced power"},
      {"   AF filter sweep",         true,  "the LCD menu's wording for the same tool"},

      /* --- the rest of the dummy-load tools ------------------------------ */
      {"|  Test ADC I/Q          |", true,  "injects into the RF front end"},
      {"|  SSB Calibration       |", true,  "manual: YOU MUST ATTACH A 50-OHM DUMMY LOAD"},
      {"|  PA mod. Test          |", true,  "drives the PA modulator"},
      {"|  Diagnostics           |", true,  "its Transmitter block keys at full PA volts"},
      {"|  Beacon                |", true,  "the LCD menu's wording"},
      {"|  Virtual U3S           |", true,  "what the TERMINAL actually shows - read off a 1_04_010 QMX+ on 2026-10-22"},

      /* --- a reworded label in a later firmware must still trip ----------- */
      {"|  SWR Sweep             |", true,  "capitalised differently"},
      {"|  swr sweep             |", true,  "lower case"},
      {"|  Sweep (SWR)           |", true,  "reordered"},

      /* --- and what must NOT be refused: the whole point of Radio Menus --- */
      {"|  GPS viewer            |", false, "we navigate here ourselves"},
      {"|  Microphone test       |", false, "no transmit"},
      {"|  Hardware tests        |", false, "the submenu itself is just a door"},
      {"|  Band config.          |", false, "the terminal spells it with a dot"},
      {"|  Display/controls      |", false, "real row, must stay allowed"},
      {"|  Protection            |", false, "real row"},
      {"|  Factory reset         |", false, "destructive, but it does not transmit"},
      {"|  Presets               |", false, "real row"},
      {"|  Messages              |", false, "real row"},
      {"|  RX outside band     ON|", false, "a System config VALUE row"},
      {"|  Band limits         None|", false, "gates TX on the radio, emits nothing"},
      {"|  System config         |", false, ""},
      {"|  CAT command test      |", false, ""},
      {"|  CAT monitor           |", false, ""},
      {"|  CPU Monitor           |", false, ""},
      {"|  Log file              |", false, ""},
      {"|  Input Analysis        |", false, ""},
      {"|  Exit terminal         |", false, "closing the session must never be refused"},
      {"+---Main menu-----------+", false, "a box title"},
      {"",                           false, "blank row"},
    };

    static const struct key keys[] = {
      {'t', true,  "full PA voltage, no SWR protection"},
      {'T', true,  "the manual's own prose says P or T"},
      {'p', true,  "asserts PTT"},
      {'P', true,  ""},
      {'+', false, "changes band"},
      {'-', false, ""},
      {'0', false, "a digit typed into a field"},
      {'a', false, ""},
      {'.', false, "the hidden gridline-colour key"},
    };

    int fails = 0;

    printf("MENU ROWS\n");
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const char *why = NULL;
        bool got = qmx_term_row_transmits(rows[i].text, &why);
        bool bad = got != rows[i].tx;
        printf("  %-28s %-8s %s%s\n",
               rows[i].text[0] ? rows[i].text : "(blank)",
               got ? "REFUSE" : "allow",
               why ? why : rows[i].note,
               bad ? "   <<< WRONG" : "");
        if (bad) { fails++; printf("      expected %s\n", rows[i].tx ? "REFUSE" : "allow"); }
    }

    printf("\nDIAGNOSTICS SCREEN KEYS\n");
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *why = NULL;
        bool got = qmx_term_key_transmits_on_diagnostics(keys[i].k, &why);
        bool bad = got != keys[i].tx;
        printf("  '%c'  %-8s %s%s\n", keys[i].k, got ? "REFUSE" : "allow",
               why ? why : keys[i].note, bad ? "   <<< WRONG" : "");
        if (bad) { fails++; printf("      expected %s\n", keys[i].tx ? "REFUSE" : "allow"); }
    }

    printf("\n%u case(s), %d wrong\n",
           (unsigned)(sizeof(rows) / sizeof(rows[0]) + sizeof(keys) / sizeof(keys[0])),
           fails);
    return fails ? 1 : 0;
}
