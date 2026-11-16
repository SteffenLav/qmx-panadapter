/* RTTY page dialogs (R030): the log confirmation and the contest settings.
 * LVGL task only. Both leave the TX pane (STOP) uncovered. */
#pragma once
#include <stdbool.h>

/* Open the confirmation for the station being worked (rtty_tx_get_call()),
 * prefilled from rtty_qso_get(). Nothing is logged until Log is pressed. */
void rtty_log_modal_open(void);
bool rtty_log_modal_is_open(void);

/* A word tapped in the decoded text while the log dialog is open: filled
 * into the field it belongs in (report, callsign or exchange). */
void rtty_log_modal_fill_word(const char *w);

/* Contest mode on/off, name, next serial, sent exchange. Close saves. */
void rtty_contest_modal_open(void);
void rtty_contest_modal_close(void);

/* The log dialog only, without logging (the station changed elsewhere). */
void rtty_log_modal_close_log(void);

/* Both dialogs (leaving the page). The contest one saves as it closes. */
void rtty_log_modal_close(void);
