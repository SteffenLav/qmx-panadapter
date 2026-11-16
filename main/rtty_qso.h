/* RTTY QSO exchange capture (R030). Pure C - no ESP-IDF, no LVGL - so the
 * host harness (test/rtty_qso_harness.c) links this file as it is.
 *
 * The RTTY page feeds the decoded RX text here from the moment a callsign is
 * picked; rtty_qso_get() then offers the report and exchange that station
 * sent. It is a SUGGESTION for the log confirmation dialog, never a logged
 * fact: RTTY decode errors damage numbers, and the operator confirms.
 *
 * What it understands (from the 2026-11-14/15 WAE RTTY contest decodes):
 *   "OZ1LAV 599 075 075"      RST then the exchange, usually sent twice
 *   "TNN 963 963"             a lost FIGS shift: T=5, so TNN is 5NN
 *   "QWE"                     the same for a whole number: 123
 *   "599-998-998"             '-' or any non-alphanumeric splits tokens
 *   "599 7)U 1207"            a damaged first copy: the repeat is used
 *   "H599"                    a noise letter glued to the RST
 * A pair that follows our own callsign wins over any other pair, since a
 * busy run station's text also holds the reports it sent to others. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define RTTY_QSO_BUF 512   /* RX characters kept since the call was picked */

/* Forget everything (a new call was picked, or the QSO was logged). */
void rtty_qso_reset(void);

/* Decoded RX characters, in order. Keeps the last RTTY_QSO_BUF. */
void rtty_qso_feed(const char *s, size_t n);

/* Best report + exchange seen since the reset. rst gets "599" (5NN and
 * letter-shift forms are written as 599), exch the received exchange
 * upper-case. *to_me (optional) = the pair followed our callsign. False =
 * nothing found; rst and exch are then "". */
bool rtty_qso_get(const char *mycall, char *rst, size_t rst_sz,
                  char *exch, size_t exch_sz, bool *to_me);

/* One tapped word, for the log dialog's tap-to-fill: a report (written as
 * "599"), or an exchange (letter-shift forms mapped to digits). A callsign
 * is NOT judged here - the page checks the call shape first. */
typedef enum { RTTY_WORD_NONE, RTTY_WORD_RST, RTTY_WORD_EXCH } rtty_word_t;
rtty_word_t rtty_qso_word(const char *w, char *out, size_t out_sz);

/* The parser itself, on any text. Same outputs as rtty_qso_get(). */
bool rtty_qso_parse(const char *text, const char *mycall, char *rst, size_t rst_sz,
                    char *exch, size_t exch_sz, bool *to_me);
