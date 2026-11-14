/* RTTY transmit core (R010) - the code stream, no timing, no radio.
 * Design: docs/rtty-tx-design.md.
 *
 * Text goes in (rtty_txc_push), 5-bit ITA2 codes come out (rtty_txc_next),
 * one per character slot: a short LTRS preamble, the text with the shift
 * codes it needs, LTRS "diddles" while the queue is empty, and -1 when the
 * over should end. The device sequencer (rtty_tx.c) turns each code into
 * start + 5 data + 1.5 stop bits of TA tones; the host harness turns them
 * into FSK and decodes them back.
 *
 * '\n' is sent as CR LF. Characters ITA2 cannot carry are dropped, not sent
 * as something else. USOS is always on: a space returns the receiver to
 * LTRS, so the encoder re-sends FIGS after a space in a run of figures.
 *
 * No ESP dependencies; the caller owns locking. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rtty_baudot.h"

#define RTTY_TXC_CAP       512   /* characters queued, not yet sent */
#define RTTY_TXC_PREAMBLE  2     /* LTRS codes before the first character */

typedef struct {
    char    q[RTTY_TXC_CAP];
    int     head, len;           /* ring */
    rtty_baudot_t enc;
    uint8_t pend[3];             /* codes of the character being sent */
    int     npend, ipend;
    int     preamble;            /* LTRS codes still to send first */
    int     idle;                /* diddles sent since the queue ran dry */
    bool    end;                 /* end the over once the queue is empty */
    uint32_t n_text;             /* characters taken from the queue */
} rtty_txc_t;

void rtty_txc_init(rtty_txc_t *c);

/* Append text. All or nothing: refused whole (false) when it does not fit.
 * A push clears a pending end, so typing on keeps the over going. */
bool rtty_txc_push(rtty_txc_t *c, const char *text);

/* End the over once everything queued has gone (a macro's last line, or the
 * operator's "send and receive"). */
void rtty_txc_end(rtty_txc_t *c);

/* Drop the queue. Returns how many characters were dropped. */
int  rtty_txc_cancel(rtty_txc_t *c);

/* Next code to send, 0..31, or -1: the over is over. idle_max = how many
 * diddles to send on an empty queue before giving up (the 3 s rule). */
int  rtty_txc_next(rtty_txc_t *c, int idle_max);

static inline int rtty_txc_queued(const rtty_txc_t *c) { return c->len + (c->npend - c->ipend); }
