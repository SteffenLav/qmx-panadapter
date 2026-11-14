/* RTTY transmit core (R010). See rtty_tx_core.h. */
#include "rtty_tx_core.h"
#include <string.h>

void rtty_txc_init(rtty_txc_t *c)
{
    memset(c, 0, sizeof(*c));
    rtty_baudot_init(&c->enc, true);
    c->preamble = RTTY_TXC_PREAMBLE;
}

/* What one queued byte will cost, so a push can be refused whole. */
static int slots(char ch) { return ch == '\n' ? 2 : 1; }

bool rtty_txc_push(rtty_txc_t *c, const char *text)
{
    int need = 0;
    for (const char *p = text; *p; p++) need += slots(*p);
    if (need > RTTY_TXC_CAP - c->len) return false;
    for (const char *p = text; *p; p++) {
        char ch = *p;
        if (ch == '\r') continue;                     /* '\n' brings its own CR */
        if (ch == '\n') {
            c->q[(c->head + c->len++) % RTTY_TXC_CAP] = '\r';
            c->q[(c->head + c->len++) % RTTY_TXC_CAP] = '\n';
            continue;
        }
        c->q[(c->head + c->len++) % RTTY_TXC_CAP] = ch;
    }
    if (need > 0) { c->end = false; c->idle = 0; }
    return true;
}

void rtty_txc_end(rtty_txc_t *c) { c->end = true; }

int rtty_txc_cancel(rtty_txc_t *c)
{
    int n = c->len;
    c->len = 0;
    return n;
}

int rtty_txc_next(rtty_txc_t *c, int idle_max)
{
    if (c->preamble > 0) {
        c->preamble--;
        c->enc.figs = false;                          /* LTRS sets the receiver too */
        return RTTY_CODE_LTRS;
    }
    for (;;) {
        if (c->ipend < c->npend) return c->pend[c->ipend++];
        if (c->len == 0) break;
        char ch = c->q[c->head];
        c->head = (c->head + 1) % RTTY_TXC_CAP;
        c->len--;
        uint8_t out[2];
        int n = rtty_baudot_encode(&c->enc, ch, out);
        if (n <= 0) continue;                         /* no ITA2 code: dropped */
        memcpy(c->pend, out, (size_t)n);
        c->npend = n; c->ipend = 0;
        c->idle = 0;
        c->n_text++;
    }
    /* Queue empty. */
    if (c->end || c->idle >= idle_max) return -1;
    c->idle++;
    c->enc.figs = false;
    return RTTY_CODE_LTRS;                            /* diddle */
}
