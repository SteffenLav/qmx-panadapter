// Host test for the CW transmit queue (main/cw_txq.c), compiled from the same
// file the firmware builds:
//
//   gcc -std=c11 -Wall -Wextra -I main -o cw_txq_harness
//       test/cw_txq_harness.c main/cw_txq.c && ./cw_txq_harness
//
// What matters is the release rule, because it decides what keys the radio:
// never a second chunk while the radio may still be keying the first, never a
// release on a TB that could predate the send, and a stall that DROPS rather
// than sends.

#include "cw_txq.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", what, __LINE__); fails++; } \
    else          printf("ok:   %s\n", what); } while (0)

#define MS(x) ((int64_t)(x) * 1000)

int main(void)
{
    cw_txq_t q;
    char out[CW_TXQ_MAX_CHUNK + 1];

    // 1. A single message goes as ONE chunk, exactly as the old path did.
    cw_txq_init(&q);
    CHECK(cw_txq_push(&q, "CQ CQ DE OZ1LAV K"), "push macro");
    CHECK(cw_txq_take(&q, MS(0), out, sizeof out) == 17 &&
          strcmp(out, "CQ CQ DE OZ1LAV K") == 0, "macro taken whole");
    CHECK(cw_txq_busy(&q), "busy while in flight");

    // 2. Type-ahead: words pushed while keying wait for the radio to empty.
    CHECK(cw_txq_push(&q, "TNX "), "type-ahead word 1");
    CHECK(cw_txq_push(&q, "FER "), "type-ahead word 2");
    CHECK(cw_txq_take(&q, MS(100), out, sizeof out) == 0, "nothing while in flight");
    cw_txq_on_tb(&q, 9, MS(600));
    CHECK(cw_txq_take(&q, MS(600), out, sizeof out) == 0, "nothing while t=9");

    // 3. A t=0 inside the settle window may predate the KY: must NOT release.
    cw_txq_init(&q);
    cw_txq_push(&q, "A ");
    cw_txq_take(&q, MS(1000), out, sizeof out);
    cw_txq_push(&q, "B ");
    cw_txq_on_tb(&q, 0, MS(1100));
    CHECK(cw_txq_take(&q, MS(1100), out, sizeof out) == 0, "t=0 inside settle ignored");
    cw_txq_on_tb(&q, 0, MS(1400));
    CHECK(cw_txq_take(&q, MS(1400), out, sizeof out) == 2 && strcmp(out, "B ") == 0,
          "t=0 after settle releases the next chunk");

    // 4. Everything queued at the refill goes in one chunk (fewest gaps).
    cw_txq_init(&q);
    cw_txq_push(&q, "GM ");
    cw_txq_take(&q, 0, out, sizeof out);
    cw_txq_push(&q, "UR ");
    cw_txq_push(&q, "RST ");
    cw_txq_push(&q, "599 ");
    cw_txq_on_tb(&q, 0, MS(500));
    CHECK(cw_txq_take(&q, MS(500), out, sizeof out) == 11 &&
          strcmp(out, "UR RST 599 ") == 0, "queued words batch into one chunk");

    // 5. Longer than the KY buffer: cut at a word boundary, space kept.
    cw_txq_init(&q);
    char longmsg[200] = "";
    for (int i = 0; i < 20; i++) strcat(longmsg, "WORD ");    // 100 chars
    CHECK(cw_txq_push(&q, longmsg), "push 100 chars");
    int n = cw_txq_take(&q, 0, out, sizeof out);
    CHECK(n == 80 && out[79] == ' ' && cw_txq_queued(&q) == 20,
          "first chunk 80 chars ending on a space, 20 left");
    cw_txq_on_tb(&q, 0, MS(400));
    n = cw_txq_take(&q, MS(400), out, sizeof out);
    CHECK(n == 20 && strncmp(out, "WORD", 4) == 0, "remainder starts on a word");

    // 6. Cut point that is not exactly at the limit.
    cw_txq_init(&q);
    char m2[200];
    memset(m2, 'X', 78); m2[78] = ' '; strcpy(m2 + 79, "ABCDEF");   // 85 chars
    cw_txq_push(&q, m2);
    n = cw_txq_take(&q, 0, out, sizeof out);
    CHECK(n == 79 && strcmp(q.buf, "ABCDEF") == 0, "cut after the space at 78");

    // 7. No space at all: hard cut at the buffer size.
    cw_txq_init(&q);
    char m3[120]; memset(m3, 'Z', 100); m3[100] = '\0';
    cw_txq_push(&q, m3);
    CHECK(cw_txq_take(&q, 0, out, sizeof out) == 80, "spaceless run cut hard at 80");

    // 8. Cancel drops the queue, not the flight.
    cw_txq_init(&q);
    cw_txq_push(&q, "ONE ");
    cw_txq_take(&q, 0, out, sizeof out);
    cw_txq_push(&q, "TWO THREE ");
    CHECK(cw_txq_cancel(&q) == 10 && cw_txq_queued(&q) == 0, "cancel drops 10 queued");
    CHECK(q.in_flight, "in-flight chunk untouched by cancel");
    cw_txq_on_tb(&q, 0, MS(500));
    CHECK(cw_txq_take(&q, MS(500), out, sizeof out) == 0, "nothing left to send after cancel");

    // 9. Capacity: all or nothing.
    cw_txq_init(&q);
    char big[CW_TXQ_CAP + 1]; memset(big, 'Q', CW_TXQ_CAP - 2); big[CW_TXQ_CAP - 2] = '\0';
    CHECK(cw_txq_push(&q, big), "fill to CAP-2");
    CHECK(!cw_txq_push(&q, "ABC"), "3 more refused whole");
    CHECK(cw_txq_queued(&q) == CW_TXQ_CAP - 2, "refused push left nothing behind");

    // 10. Stall: no TB after a send -> queue DROPPED, nothing released.
    cw_txq_init(&q);
    cw_txq_push(&q, "K ");
    cw_txq_take(&q, 0, out, sizeof out);                  // 2 chars in flight
    cw_txq_push(&q, "MORE ");
    int64_t limit = CW_TXQ_STALL_US_BASE + 2 * CW_TXQ_STALL_US_PER_CHAR;
    CHECK(cw_txq_take(&q, limit - 1, out, sizeof out) == 0, "no release before stall limit");
    CHECK(cw_txq_take(&q, limit, out, sizeof out) == -1, "stall reported");
    CHECK(!cw_txq_busy(&q), "stall dropped the queue and the flight");

    // 11. A refused send (band guard) clears the flight without a TB.
    cw_txq_init(&q);
    cw_txq_push(&q, "X ");
    cw_txq_take(&q, 0, out, sizeof out);
    cw_txq_abort_flight(&q);
    cw_txq_push(&q, "Y ");
    CHECK(cw_txq_take(&q, MS(10), out, sizeof out) == 2, "abort_flight lets the next go");

    // 12. Unknown t-field (-1) never releases.
    cw_txq_init(&q);
    cw_txq_push(&q, "A ");
    cw_txq_take(&q, 0, out, sizeof out);
    cw_txq_push(&q, "B ");
    cw_txq_on_tb(&q, -1, MS(1000));
    CHECK(cw_txq_take(&q, MS(1000), out, sizeof out) == 0, "t=-1 does not release");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
