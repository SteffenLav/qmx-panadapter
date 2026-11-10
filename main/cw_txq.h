// CW transmit queue - type-ahead for the CW page (Ralph DL1HR, 2026-11-10)
// and the cancel point T145 (Michael KZ4LY) asked for.
//
// ⭐ WHAT IT IS FOR. Before this, a CW send was ONE "KY <text>;" holding the
// whole message, and a second send before the first drained REPLACED it. So
// nobody could type while the radio keyed, and nothing could stop a message.
// Now text is APPENDED here, and handed to the radio in chunks, each only once
// the radio's TB t-field says the previous one has finished.
//
// ⛔ WHY "WAIT UNTIL THE RADIO IS EMPTY" AND NOT "TOP IT UP".
// Nobody has measured what the QMX does with a KY that arrives while it is
// still keying an earlier one - append to its 80-char buffer, replace, or
// refuse. The comment in cat.c that called the buffer "circular" was written
// before anyone sent two. So the default release rule works whichever is true:
// the next chunk goes only when the t-field reads 0 (CW_TXQ_LOW_WATER). The
// cost is a gap of up to one TB poll (500 ms) plus a CAT round trip at each
// refill. Once a bench test shows that a mid-send KY APPENDS, raise
// CW_TXQ_LOW_WATER (and cut CW_TXQ_MAX_CHUNK to one word for T145's
// word-by-word cancel) - nothing else changes.
//
// ⚠ What a cancel stops: text NOT YET HANDED to the radio. A chunk already in
// the radio's buffer runs to the end - KY has no abort, measured 2026-11-02
// (memory: project_cw_ky_abort_rx_does_not_work).
//
// No ESP dependencies: test/cw_txq_harness.c compiles this file on the host.
// The caller owns the locking (cat.c holds s_cw_send_mux around every call).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CW_TXQ_CAP        256      // queued, not yet handed to the radio
#define CW_TXQ_MAX_CHUNK  80       // the QMX's KY buffer
#define CW_TXQ_LOW_WATER  0        // release when t-field <= this - see above
// A TB reading taken sooner than this after a KY can predate it and read 0
// for a buffer that is about to be full. 300 ms is the same settle the KY;
// readback after a send already uses.
#define CW_TXQ_SETTLE_US  300000
// No qualifying TB for this long after a send means the poll or the link is
// gone. The queue is then DROPPED, not released: sending more into a radio we
// cannot see is the wrong direction to fail. 3 s per character is 5 WPM with
// margin; +10 s covers a slow first poll.
#define CW_TXQ_STALL_US_PER_CHAR 3000000
#define CW_TXQ_STALL_US_BASE     10000000

typedef struct {
    char    buf[CW_TXQ_CAP + 1];
    int     len;
    bool    in_flight;       // a chunk is with the radio, not yet seen finished
    int     flight_len;
    int64_t sent_us;
} cw_txq_t;

void cw_txq_init(cw_txq_t *q);

// Append text. All or nothing: a push that does not fit is refused whole, so
// a word is never cut in half. Returns false when refused.
bool cw_txq_push(cw_txq_t *q, const char *text);

// Drop everything not yet handed to the radio. Returns how many characters
// were dropped. Does not touch the in-flight chunk.
int  cw_txq_cancel(cw_txq_t *q);

// Every parsed TB reply. t = the t-field (0..9, 9 means "9 or more").
void cw_txq_on_tb(cw_txq_t *q, int t, int64_t now_us);

// Called by the poll task. When a chunk may go now, copies it into out (NUL
// terminated), marks it in flight and returns its length; otherwise 0.
// Returns -1 when the stall timeout fired and the queue was dropped.
int  cw_txq_take(cw_txq_t *q, int64_t now_us, char *out, size_t out_sz);

// The chunk just taken was NOT sent (band guard refused, link error). Clears
// the in-flight mark so the queue does not wait for a TB that means nothing.
void cw_txq_abort_flight(cw_txq_t *q);

static inline int  cw_txq_queued(const cw_txq_t *q) { return q->len; }
static inline bool cw_txq_busy(const cw_txq_t *q)   { return q->in_flight || q->len > 0; }
