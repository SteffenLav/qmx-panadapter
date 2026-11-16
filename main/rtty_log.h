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
 * Callers: the RTTY page (LVGL task) and the web page (httpd task); the
 * list and the serial are under a mutex. */
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
 * changes, plus the card archive's part read at boot (rtty_log_scan_archive).
 * Without a card at boot, QSOs rotated off at an earlier boot are not seen. */
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

/* What the Tab5's QSO card shows, handed to the web page so it shows the
 * same: set by the RTTY page each tick, read by the httpd. */
typedef struct {
    char call[16];      /* the station it is for; "" = none picked */
    char rst[8], exch[16];
    bool heard, to_me, dupe, worked;
} rtty_log_heard_t;
void rtty_log_set_heard(const rtty_log_heard_t *h);
void rtty_log_get_heard(rtty_log_heard_t *h);

/* The contest settings changed (name, on/off): rebuild the dupe list on next use. */
void rtty_log_contest_changed(void);

/* Boot rotation moves the OLDEST records to the card archive until the device
 * log is down to ROT_LOW_BYTES (96 KB, ~370 contest QSOs). A long contest with
 * a reboot in it therefore has QSOs only the archive holds. sd_archive.c calls
 * this once per boot, right after the rotation, while the card is not yet
 * parked and under its mutex: it reads the archive for this contest's RTTY
 * QSOs, only when contest mode is on with a name. SD task. */
void rtty_log_scan_archive(const char *arc_path);
