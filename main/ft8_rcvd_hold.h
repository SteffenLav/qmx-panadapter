// ft8_rcvd_hold - a finished QSO with no received report is HELD, not logged.
//
// Rule (operator, 2026-11-25): the ADIF log never carries an FT8/FT4/JS8 QSO
// without RST_RCVD. Two QSOs that day broke it:
//
//   R1BDX  answered our CQ with a grid, took our -08 and sent RR73 without an
//          R-report. We sent 73 and logged "-06/". Their R-12 then arrived in
//          the next slot, and again one cycle later, and was thrown away.
//   EO80F  sent an unsolicited RR73 to our CQ. No report was exchanged in
//          either direction, but we logged "-02/" - and -02 was only our own
//          SNR of them, never sent.
//
// So a QSO that reaches its end with no report from the partner is held for
// FT8_RCVD_HOLD_SLOTS slots. A report from that partner addressed to us inside
// the window ("R-12", "R+05", "-12", "+05") completes the record and it is
// logged then. If the window closes with nothing, the QSO is NOT logged and the
// caller says why. R1BDX logs with -12; EO80F is not logged.
//
// Portable: no ESP-IDF dependencies and the clock is a PARAMETER, so
// test/ft8_rcvd_hold_harness.c links these functions, not a copy. The caller
// owns locking.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 4 slots = TWO of the partner's transmit slots, because the partner sends on
 * alternate slots only. FT8: 60 s; FT4: 30 s.
 *  - R1BDX sent its R-12 in the first of its slots after our 73 and again in
 *    the second, so 4 slots catches the first and survives one missed decode.
 *  - A station that has sent RR73 and still sends no report in two of its own
 *    slots has moved on; holding longer only delays the "not logged" line.
 *  - The hold does not make the QSO machine busy, so its length costs no
 *    operating time - it only bounds how late a report is still believed. */
#define FT8_RCVD_HOLD_SLOTS  4
/* More than one held QSO at once is normal on a CQ run: the next caller is
 * worked while the last one's window is still open. */
#define FT8_RCVD_HOLD_MAX    4

typedef struct {
    char     call[16];
    char     rst_sent[8];
    char     rst_rcvd[8];        /* filled when the report arrives */
    char     their_grid[8];
    char     mode[8];
    char     sig[8];
    char     sig_info[16];
    uint32_t freq_hz;
    int64_t  qso_time;           /* epoch s - when the exchange ended */
    int64_t  deadline_ms;        /* epoch ms - a slot starting at or after this is outside */
} ft8_rcvd_hold_t;

void ft8_rcvd_hold_reset(void);

/* True when a finished QSO must be HELD instead of logged now: no received
 * report. A Field Day QSO carries the class/section exchange instead of a
 * numeric report and never has one, so it is exempt when that exchange is in
 * hand. */
bool ft8_rcvd_hold_needed(const char *rst_rcvd, bool have_fd_exch);

/* "R-12" -> "-12", "R+05" -> "+05", "-12" -> "-12". False for RR73, RRR, 73,
 * a grid, or anything else. */
bool ft8_rcvd_hold_parse_report(const char *rest, char *out, size_t out_len);

/* Hold `rec` until now_ms + slots * slot_ms. A call already held is REPLACED
 * (the newer exchange supersedes it). With the table full the oldest entry is
 * evicted and copied to *evicted (returns true) so the caller can report it. */
bool ft8_rcvd_hold_add(const ft8_rcvd_hold_t *rec, int64_t now_ms, int slot_ms,
                       ft8_rcvd_hold_t *evicted);

/* Remove `call` without logging - a later exchange with them was logged with
 * a report, so this one is superseded. True if one was held. */
bool ft8_rcvd_hold_drop(const char *call);

bool ft8_rcvd_hold_is_held(const char *call);

/* True when anything is held - lets the caller skip the per-slot scan. */
bool ft8_rcvd_hold_any(void);

/* Feed one decoded message from the slot that started at slot_ms: `to` and
 * `from` are the first two tokens with hash brackets removed, `rest` the
 * remainder. When it is a report from a held partner to `my_call` inside the
 * window, the entry leaves the table with rst_rcvd filled in, is copied to
 * *out, and the function returns true: log it. */
bool ft8_rcvd_hold_on_msg(const char *my_call, const char *to, const char *from,
                          const char *rest, int64_t slot_ms, ft8_rcvd_hold_t *out);

/* Once per decoded slot, after every message of that slot was fed: removes ONE
 * entry whose window has closed and copies it to *out. Call until false. */
bool ft8_rcvd_hold_expire(int64_t slot_ms, ft8_rcvd_hold_t *out);
