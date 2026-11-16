/* RTTY QSO exchange capture - see rtty_qso.h. R030. */
#include "rtty_qso.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char   s_buf[RTTY_QSO_BUF + 1];
static size_t s_len;

void rtty_qso_reset(void)
{
    s_len = 0;
    s_buf[0] = 0;
}

void rtty_qso_feed(const char *s, size_t n)
{
    if (!s || !n) return;
    if (n >= RTTY_QSO_BUF) { s += n - RTTY_QSO_BUF; n = RTTY_QSO_BUF; }
    if (s_len + n > RTTY_QSO_BUF) {   /* drop the oldest */
        size_t drop = s_len + n - RTTY_QSO_BUF;
        memmove(s_buf, s_buf + drop, s_len - drop);
        s_len -= drop;
    }
    memcpy(s_buf + s_len, s, n);
    s_len += n;
    s_buf[s_len] = 0;
}

bool rtty_qso_get(const char *mycall, char *rst, size_t rst_sz,
                  char *exch, size_t exch_sz, bool *to_me)
{
    return rtty_qso_parse(s_buf, mycall, rst, rst_sz, exch, exch_sz, to_me);
}

/* ---- parser ---------------------------------------------------------------- */

/* A lost FIGS shift prints the letter on the same Baudot key as the digit:
 * Q W E R T Y U I O P = 1 2 3 4 5 6 7 8 9 0 (ITA2). */
static char figs_of(char c)
{
    static const char L[] = "QWERTYUIOP", D[] = "1234567890";
    const char *p = strchr(L, c);
    return (p && c) ? D[p - L] : 0;
}

#define TOK_MAX 128
#define TOK_LEN 16

typedef struct {
    char t[TOK_LEN];
    bool damaged;      /* held punctuation other than '/': a decode error */
} tok_t;

/* Space, '-', ',' and '.' separate. '-' is a real separator in contests
 * ("599-998-998"); any other punctuation inside a word marks it damaged. */
static bool is_sep(char c)
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '-' || c == ',' || c == '.';
}

static int tokenize(const char *s, tok_t *tk, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s && is_sep(*s)) s++;
        if (!*s) break;
        tok_t *t = &tk[n];
        int l = 0;
        t->damaged = false;
        while (*s && !is_sep(*s)) {
            char c = (char)toupper((unsigned char)*s++);
            if (!isalnum((unsigned char)c) && c != '/') t->damaged = true;
            if (l < TOK_LEN - 1) t->t[l++] = c;
            else t->damaged = true;   /* too long to be anything we want */
        }
        t->t[l] = 0;
        n++;
    }
    return n;
}

/* 599 / 5NN, letter-shift forms included (TOO, TNN). A noise letter glued
 * in front ("H599", "Q599") is allowed: the RST is the last three chars. */
static bool is_rst(const tok_t *t)
{
    if (t->damaged) return false;
    size_t l = strlen(t->t);
    if (l < 3) return false;
    const char *r = t->t + l - 3;
    for (size_t i = 0; i < l - 3; i++)
        if (!isalpha((unsigned char)t->t[i])) return false;
    char d[3];
    for (int i = 0; i < 3; i++) {
        char c = r[i];
        char f = figs_of(c);
        d[i] = isdigit((unsigned char)c) ? c : (f ? f : c);
    }
    return d[0] == '5' && (d[1] == '9' || d[1] == 'N') && (d[2] == '9' || d[2] == 'N');
}

/* Words that follow a report but are not an exchange. Checked BEFORE the
 * letter-shift mapping: "TU" is all QWERTYUIOP and would read as 57. */
static bool is_stopword(const char *w)
{
    static const char *const S[] = { "TU", "DE", "K", "BK", "KN", "SK", "NR", "ES", "QRZ",
                                     "CQ", "TEST", "AGN", "PSE", "UR", "RST", "TNX", "OK", NULL };
    for (int i = 0; S[i]; i++) if (!strcmp(w, S[i])) return true;
    return false;
}

/* A received exchange: a serial or zone (digits, letter-shift forms mapped),
 * or a short letter group (state, section). False = not an exchange. */
static bool exch_of(const tok_t *t, char *out, size_t out_sz)
{
    if (t->damaged || is_stopword(t->t) || is_rst(t)) return false;
    size_t l = strlen(t->t);
    if (l == 0 || l > 6) return false;
    bool all_figs = true, any_digit = false, all_alpha = true;
    for (size_t i = 0; i < l; i++) {
        char c = t->t[i];
        if (!figs_of(c)) all_figs = false;
        if (isdigit((unsigned char)c)) any_digit = true;
        if (!isalpha((unsigned char)c)) all_alpha = false;
    }
    char b[TOK_LEN];
    if (all_figs) {
        for (size_t i = 0; i < l; i++) b[i] = figs_of(t->t[i]);
        b[l] = 0;
    } else if (any_digit || (all_alpha && l >= 2 && l <= 3)) {
        snprintf(b, sizeof b, "%s", t->t);
    } else {
        return false;
    }
    snprintf(out, out_sz, "%s", b);
    return true;
}

/* Our call, alone or with noise letters glued in front ("WDXZUA3LID"): a
 * missed space before a call is the commonest damage in the WAE decodes. */
static bool ends_with_call(const char *tok, const char *call)
{
    size_t lt = strlen(tok), lc = strlen(call);
    return lc && lt >= lc && !strcmp(tok + lt - lc, call);
}

rtty_word_t rtty_qso_word(const char *w, char *out, size_t out_sz)
{
    if (out && out_sz) out[0] = 0;
    if (!w) return RTTY_WORD_NONE;
    tok_t t;
    if (tokenize(w, &t, 1) != 1) return RTTY_WORD_NONE;
    if (is_rst(&t)) { snprintf(out, out_sz, "599"); return RTTY_WORD_RST; }
    return exch_of(&t, out, out_sz) ? RTTY_WORD_EXCH : RTTY_WORD_NONE;
}

bool rtty_qso_parse(const char *text, const char *mycall, char *rst, size_t rst_sz,
                    char *exch, size_t exch_sz, bool *to_me)
{
    if (rst && rst_sz) rst[0] = 0;
    if (exch && exch_sz) exch[0] = 0;
    if (to_me) *to_me = false;
    if (!text) return false;

    static tok_t tk[TOK_MAX];   /* static: 2 KB is too much for an LVGL-task stack */
    int n = tokenize(text, tk, TOK_MAX);

    int best_score = -1;
    char best[TOK_LEN] = "";
    bool best_me = false;
    for (int i = 0; i < n; i++) {
        if (!is_rst(&tk[i])) continue;
        char c1[TOK_LEN] = "", c2[TOK_LEN] = "";
        bool v1 = i + 1 < n && exch_of(&tk[i + 1], c1, sizeof c1);
        bool v2 = i + 2 < n && exch_of(&tk[i + 2], c2, sizeof c2);
        /* The repeat stands in only for a DAMAGED first copy: "599 599 NAME
         * BOB" must not read BOB as the exchange. */
        const char *pick = v1 ? c1 : ((v2 && tk[i + 1].damaged) ? c2 : NULL);
        if (!pick) continue;
        bool me = false;
        if (mycall && mycall[0])
            for (int k = i - 1; k >= 0 && k >= i - 3; k--)
                if (ends_with_call(tk[k].t, mycall)) { me = true; break; }
        bool repeated = v1 && v2 && !strcmp(c1, c2);
        int score = (me ? 4 : 0) + (repeated ? 2 : 0);
        if (score >= best_score) {   /* >=: the latest of equals wins */
            best_score = score;
            snprintf(best, sizeof best, "%s", pick);
            best_me = me;
        }
    }
    if (best_score < 0) return false;
    if (rst && rst_sz) snprintf(rst, rst_sz, "599");
    if (exch && exch_sz) snprintf(exch, exch_sz, "%s", best);
    if (to_me) *to_me = best_me;
    return true;
}
