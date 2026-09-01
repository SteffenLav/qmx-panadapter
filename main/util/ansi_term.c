#include "ansi_term.h"
#include <string.h>
#include <stdlib.h>

static void clear_all(ansi_term_t *t)
{
    for (int r = 0; r < ANSI_ROWS; r++) {
        for (int c = 0; c < ANSI_COLS; c++) {
            t->cell[r][c].ch      = ' ';
            t->cell[r][c].fg      = 0;
            t->cell[r][c].reverse = false;
        }
    }
}

void ansi_term_reset(ansi_term_t *t)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));
    clear_all(t);
    t->cursor_visible = true;
    t->dirty_seq      = 1;
}

/* Put one printable character at the cursor and advance.
 *
 * Off the right-hand edge we STOP rather than wrap. The QMX positions every run
 * of text explicitly, so a wrap would only ever be the result of us having
 * mis-parsed something - and silently corrupting the next row down is a much
 * harder fault to see than a line that stops short. */
static void put_char(ansi_term_t *t, char ch)
{
    if (t->cur_r < 0 || t->cur_r >= ANSI_ROWS) return;
    if (t->cur_c < 0 || t->cur_c >= ANSI_COLS) return;
    ansi_cell_t *cl = &t->cell[t->cur_r][t->cur_c];
    if (cl->ch != ch || cl->fg != t->cur_fg || cl->reverse != t->cur_reverse) {
        cl->ch      = ch;
        cl->fg      = t->cur_fg;
        cl->reverse = t->cur_reverse;
        t->dirty_seq++;
    }
    t->cur_c++;
}

/* Apply one SGR (ESC[...m) parameter. */
static void apply_sgr(ansi_term_t *t, int p)
{
    if (p == 0)                   { t->cur_fg = 0; t->cur_reverse = false; }
    else if (p == 7)              { t->cur_reverse = true; }
    else if (p == 27)             { t->cur_reverse = false; }
    else if (p >= 30 && p <= 37)  { t->cur_fg = (uint8_t)p; }
    else {
        /* Anything else (bold, background, bright 90-97, 256-colour) is still
           ignored on purpose - guessing at unmeasured behaviour is how a parser
           grows bugs nobody can reproduce.

           But it is no longer ignored SILENTLY. Samuel W7STF reported that the
           QMX+ Diagnostics screen "uses red" and that the green Press / <<< >>>
           button labels do not come through (#215), while every byte ever
           captured from the MENU screens uses only 0/7/27/33/37 - all handled.
           So the Diagnostics screen sends something else and we had no way to
           learn what: a dropped parameter left no trace, which is the same
           unfalsifiability trap as the silent USB patches (#189).

           Record the distinct offenders so /api/term can report them and the
           answer comes from the radio instead of from my imagination. */
        for (int i = 0; i < t->unk_n; i++) {
            if (t->unk[i] == p) { t->unk_count++; return; }   /* already known */
        }
        if (t->unk_n < ANSI_UNK_MAX) t->unk[t->unk_n++] = (int16_t)p;
        t->unk_count++;
    }
}

/* A complete CSI sequence: esc_buf holds the parameter text, `final` the letter. */
static void handle_csi(ansi_term_t *t, char final)
{
    const char *p = t->esc_buf;

    /* Private sequences (ESC[?25l / ESC[?25h) - cursor visibility only. */
    if (p[0] == '?') {
        int v = atoi(p + 1);
        if (v == 25) {
            bool vis = (final == 'h');
            if (vis != t->cursor_visible) { t->cursor_visible = vis; t->dirty_seq++; }
        }
        return;
    }

    switch (final) {
    case 'H':
    case 'f': {
        /* ESC[H = home; ESC[r;cH = position. Both 1-based on the wire. */
        int r = 1, c = 1;
        if (p[0]) {
            r = atoi(p);
            const char *semi = strchr(p, ';');
            c = semi ? atoi(semi + 1) : 1;
        }
        if (r < 1) r = 1;
        if (c < 1) c = 1;
        t->cur_r = r - 1;
        t->cur_c = c - 1;
        break;
    }
    case 'J':
        /* ESC[2J clears the whole screen; the radio only ever sends 2. A bare
           ESC[J (clear to end) is treated the same rather than ignored - both
           mean "this region is now blank", and clearing too much merely costs a
           repaint the radio is about to send anyway. */
        clear_all(t);
        t->dirty_seq++;
        break;
    case 'K': {
        /* Erase in line. Only the default (cursor to end of line) is used. */
        for (int c = t->cur_c; c < ANSI_COLS; c++) {
            if (t->cur_r >= 0 && t->cur_r < ANSI_ROWS) {
                t->cell[t->cur_r][c].ch      = ' ';
                t->cell[t->cur_r][c].fg      = 0;
                t->cell[t->cur_r][c].reverse = false;
            }
        }
        t->dirty_seq++;
        break;
    }
    case 'm': {
        /* Zero or more ';'-separated parameters. A bare ESC[m means reset. */
        if (!p[0]) { apply_sgr(t, 0); break; }
        const char *s = p;
        while (*s) {
            apply_sgr(t, atoi(s));
            const char *semi = strchr(s, ';');
            if (!semi) break;
            s = semi + 1;
        }
        break;
    }
    default:
        /* Unrecognised - swallowed, never printed. */
        break;
    }
}

void ansi_term_feed(ansi_term_t *t, const uint8_t *data, size_t len)
{
    if (!t || !data) return;
    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];

        if (t->esc_state == 1) {                 /* just saw ESC */
            if (ch == '[') { t->esc_state = 2; t->esc_len = 0; t->esc_buf[0] = '\0'; }
            else           { t->esc_state = 0; }   /* ESC + something else: drop it */
            continue;
        }
        if (t->esc_state == 2) {                 /* inside CSI */
            /* Parameter bytes are 0x30-0x3F, the final byte is 0x40-0x7E. */
            if (ch >= 0x40 && ch <= 0x7E) {
                t->esc_buf[t->esc_len < (int)sizeof(t->esc_buf) - 1 ? t->esc_len : (int)sizeof(t->esc_buf) - 1] = '\0';
                handle_csi(t, ch);
                t->esc_state = 0;
            } else if (t->esc_len < (int)sizeof(t->esc_buf) - 1) {
                t->esc_buf[t->esc_len++] = ch;
                t->esc_buf[t->esc_len]   = '\0';
            }
            /* Over-long parameter runs keep consuming until the final byte, so a
               malformed sequence cannot leak its tail into the display. */
            continue;
        }

        switch (ch) {
        case 0x1b: t->esc_state = 1; break;
        case '\r': t->cur_c = 0; break;
        case '\n':
            t->cur_c = 0;
            if (t->cur_r < ANSI_ROWS - 1) t->cur_r++;
            break;
        case '\b': if (t->cur_c > 0) t->cur_c--; break;
        case '\t': t->cur_c = (t->cur_c + 8) & ~7; break;
        default:
            if ((unsigned char)ch >= 32 && (unsigned char)ch < 127) put_char(t, ch);
            /* Other control bytes are dropped; the radio sends none of them. */
            break;
        }
    }
}

void ansi_term_row_text(const ansi_term_t *t, int row, char *out)
{
    if (!t || !out) return;
    if (row < 0 || row >= ANSI_ROWS) { out[0] = '\0'; return; }
    for (int c = 0; c < ANSI_COLS; c++) out[c] = t->cell[row][c].ch;
    out[ANSI_COLS] = '\0';
}
