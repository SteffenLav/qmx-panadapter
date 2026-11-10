// See cw_txq.h for why the release rule is "wait until the radio is empty".
#include "cw_txq.h"

#include <string.h>

void cw_txq_init(cw_txq_t *q)
{
    memset(q, 0, sizeof(*q));
}

bool cw_txq_push(cw_txq_t *q, const char *text)
{
    if (!text || !text[0]) return true;
    size_t n = strlen(text);
    if ((size_t)q->len + n > CW_TXQ_CAP) return false;
    memcpy(q->buf + q->len, text, n);
    q->len += (int)n;
    q->buf[q->len] = '\0';
    return true;
}

int cw_txq_cancel(cw_txq_t *q)
{
    int dropped = q->len;
    q->len = 0;
    q->buf[0] = '\0';
    return dropped;
}

void cw_txq_on_tb(cw_txq_t *q, int t, int64_t now_us)
{
    if (!q->in_flight) return;
    if (now_us - q->sent_us < CW_TXQ_SETTLE_US) return;   // may predate the KY
    if (t >= 0 && t <= CW_TXQ_LOW_WATER) q->in_flight = false;
}

void cw_txq_abort_flight(cw_txq_t *q)
{
    q->in_flight = false;
}

int cw_txq_take(cw_txq_t *q, int64_t now_us, char *out, size_t out_sz)
{
    if (q->in_flight) {
        int64_t limit = CW_TXQ_STALL_US_BASE +
                        (int64_t)q->flight_len * CW_TXQ_STALL_US_PER_CHAR;
        if (now_us - q->sent_us < limit) return 0;
        q->in_flight = false;
        cw_txq_cancel(q);
        return -1;
    }
    if (q->len == 0 || !out || out_sz < 2) return 0;

    int max = CW_TXQ_MAX_CHUNK;
    if ((size_t)max > out_sz - 1) max = (int)(out_sz - 1);
    int n = q->len;
    if (n > max) {
        /* Cut at the last space inside the limit, keeping the space with the
         * chunk so the next one does not start mid-gap. A run with no space at
         * all (a pasted 100-character "word") is cut hard - it would be cut by
         * the radio's buffer anyway. */
        int cut = -1;
        for (int i = max - 1; i > 0; i--) {
            if (q->buf[i] == ' ') { cut = i + 1; break; }
        }
        n = (cut > 0) ? cut : max;
    }
    memcpy(out, q->buf, (size_t)n);
    out[n] = '\0';
    memmove(q->buf, q->buf + n, (size_t)(q->len - n));
    q->len -= n;
    q->buf[q->len] = '\0';

    q->in_flight  = true;
    q->flight_len = n;
    q->sent_us    = now_us;
    return n;
}
