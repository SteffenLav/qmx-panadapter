// See ft8_rcvd_hold.h for the rule and the two QSOs behind it.

#include "ft8_rcvd_hold.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    bool            used;
    ft8_rcvd_hold_t r;
} slot_t;

static slot_t s_tab[FT8_RCVD_HOLD_MAX];

void ft8_rcvd_hold_reset(void)
{
    memset(s_tab, 0, sizeof(s_tab));
}

bool ft8_rcvd_hold_needed(const char *rst_rcvd, bool have_fd_exch)
{
    if (have_fd_exch) return false;
    return !(rst_rcvd && rst_rcvd[0]);
}

bool ft8_rcvd_hold_parse_report(const char *rest, char *out, size_t out_len)
{
    if (!rest || !out || out_len < 4) return false;
    const char *p = rest;
    if (*p == 'R') p++;                       /* "R-12" carries the same value */
    if (*p != '+' && *p != '-') return false;
    /* One or two digits, then the end: "-12", "+5". Anything longer is not a
     * report (and "RR73"/"RRR"/"73" never reach here: no sign). */
    if (!(p[1] >= '0' && p[1] <= '9')) return false;
    size_t n = 2;
    if (p[2] >= '0' && p[2] <= '9') n = 3;
    if (p[n] != '\0') return false;
    if (n + 1 > out_len) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static int find(const char *call)
{
    if (!call || !call[0]) return -1;
    for (int i = 0; i < FT8_RCVD_HOLD_MAX; i++)
        if (s_tab[i].used && strcmp(s_tab[i].r.call, call) == 0) return i;
    return -1;
}

bool ft8_rcvd_hold_add(const ft8_rcvd_hold_t *rec, int64_t now_ms, int slot_ms,
                       ft8_rcvd_hold_t *evicted)
{
    if (!rec || !rec->call[0]) return false;
    bool ev = false;
    int idx = find(rec->call);
    if (idx < 0) {
        for (int i = 0; i < FT8_RCVD_HOLD_MAX; i++)
            if (!s_tab[i].used) { idx = i; break; }
    }
    if (idx < 0) {
        /* Full: the entry closest to its deadline is the one that would have
         * expired first anyway. */
        idx = 0;
        for (int i = 1; i < FT8_RCVD_HOLD_MAX; i++)
            if (s_tab[i].r.deadline_ms < s_tab[idx].r.deadline_ms) idx = i;
        if (evicted) *evicted = s_tab[idx].r;
        ev = true;
    }
    s_tab[idx].used = true;
    s_tab[idx].r    = *rec;
    s_tab[idx].r.rst_rcvd[0] = '\0';
    s_tab[idx].r.deadline_ms = now_ms + (int64_t)FT8_RCVD_HOLD_SLOTS * slot_ms;
    return ev;
}

bool ft8_rcvd_hold_drop(const char *call)
{
    int idx = find(call);
    if (idx < 0) return false;
    s_tab[idx].used = false;
    return true;
}

bool ft8_rcvd_hold_is_held(const char *call)
{
    return find(call) >= 0;
}

bool ft8_rcvd_hold_any(void)
{
    for (int i = 0; i < FT8_RCVD_HOLD_MAX; i++)
        if (s_tab[i].used) return true;
    return false;
}

bool ft8_rcvd_hold_on_msg(const char *my_call, const char *to, const char *from,
                          const char *rest, int64_t slot_ms, ft8_rcvd_hold_t *out)
{
    if (!my_call || !my_call[0] || !to || !from || !rest) return false;
    if (strcmp(to, my_call) != 0) return false;        /* not addressed to us */
    int idx = find(from);
    if (idx < 0) return false;
    if (slot_ms >= s_tab[idx].r.deadline_ms) return false;   /* too late */
    char rpt[8];
    if (!ft8_rcvd_hold_parse_report(rest, rpt, sizeof(rpt))) return false;
    snprintf(s_tab[idx].r.rst_rcvd, sizeof(s_tab[idx].r.rst_rcvd), "%s", rpt);
    if (out) *out = s_tab[idx].r;
    s_tab[idx].used = false;
    return true;
}

bool ft8_rcvd_hold_expire(int64_t slot_ms, ft8_rcvd_hold_t *out)
{
    for (int i = 0; i < FT8_RCVD_HOLD_MAX; i++) {
        if (!s_tab[i].used || slot_ms < s_tab[i].r.deadline_ms) continue;
        if (out) *out = s_tab[i].r;
        s_tab[i].used = false;
        return true;
    }
    return false;
}
