/* RTTY QSO logging and contest mode (R030). Design: TODO-RTTY.md R030.
 *
 * Nothing here logs on its own. rtty_log_record() is called only from the
 * confirmation dialog's Log button, after the operator has seen and can edit
 * every field: RTTY decode errors damage numbers (a lost FIGS shift turns
 * 599 001 into TOO QPP), so a parsed exchange is never logged unseen.
 *
 * Contest mode (settings_rtty_contest_t) changes the four macros
 * (rtty_tx_macro), adds CONTEST_ID / STX_STRING / SRX_STRING to the record,
 * and keeps a dupe list. The serial advances when a QSO is LOGGED, not when
 * it is sent: a repeated Exch sends the same number.
 *
 * All functions: LVGL task (the RTTY page), except the read-only
 * rtty_log_sent_exch(), which rtty_tx_macro() also calls from the httpd. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The exchange we send in contest mode: the serial as %03u, or the fixed
 * text set in the contest settings (a zone, a state). */
void rtty_log_sent_exch(char *out, size_t out_sz);

/* Dupe in contest mode: this call is already logged on this band IN THIS
 * CONTEST (CONTEST_ID match, mode RTTY). Outside contest mode: false. The
 * list is built from the device log on first use after the contest name
 * changes; QSOs rotated to the card archive at a boot are not seen. */
bool rtty_log_is_dupe(const char *call, uint32_t freq_hz);

/* Worked before on this band, any time, any mode (outside contest mode the
 * card shows it as information, not as a dupe). */
bool rtty_log_worked_on_band(const char *call, uint32_t freq_hz);

/* Write the record: MODE RTTY, the dial frequency, now. NULL or "" fields are
 * left out. In contest mode it also writes CONTEST_ID, adds the call to the
 * dupe list and advances the serial when stx is the serial. False = not
 * logged (no CAT frequency, or storage full); err says why. */
bool rtty_log_record(const char *call, const char *rst_sent, const char *rst_rcvd,
                     const char *stx, const char *srx, char *err, size_t err_sz);

/* The contest settings changed (name, on/off): rebuild the dupe list on next use. */
void rtty_log_contest_changed(void);
