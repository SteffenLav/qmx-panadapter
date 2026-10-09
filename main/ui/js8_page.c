#include "js8_page.h"

#include <stdio.h>
#include <string.h>

static void blank(char *line)
{
    memset(line, ' ', JS8_PAGE_COLS);
    line[JS8_PAGE_COLS] = 0;
}

/* Copy without the NUL a snprintf would write into the next column. The grid
 * is space-padded, so every field is placed, never printed. */
static void put(char *line, int col, const char *s, int width)
{
    /* ⛔ STOP AT THE TERMINATOR, do not keep indexing `s`.
     *
     * This read `s[i] ? s[i] : ' '` for the full width, which steps PAST the
     * NUL of a short string and reads whatever follows it. Caught on the first
     * rendered page, 2026-10-09: the 5-character unknown-sender placeholder
     * was padded to 9, and the four extra bytes came out of the adjacent
     * string literal in .rodata, so the row read "- - - JS8 QSL". Every field
     * here is padded, so this would have corrupted any of them. */
    bool end = false;
    for (int i = 0; i < width && col + i < JS8_PAGE_COLS; i++) {
        if (!end && !s[i]) end = true;
        line[col + i] = end ? ' ' : s[i];
    }
}

/* HH:MM:SS from a UTC second, by arithmetic rather than gmtime().
 *
 * gmtime() would drag in the C library's time zone handling for a value that
 * is already UTC, and on this target it is not reentrant. The page only ever
 * shows a time of day, so the date is not needed and a day boundary inside the
 * list simply shows the clock wrapping, which is what a clock does. */
static void hhmmss(int64_t utc, char out[9])
{
    int64_t s = utc % 86400;
    if (s < 0) s += 86400;
    snprintf(out, 9, "%02d:%02d:%02d",
             (int)(s / 3600), (int)((s / 60) % 60), (int)(s % 60));
}

/* Where to break `s` so the first line is at most `width`. Returns the number
 * of characters to print, and sets *skip to how many to step past (the space
 * is consumed by the break, a hard break consumes nothing). */
static int wrap_at(const char *s, int width, int *skip)
{
    int len = (int)strlen(s);
    if (len <= width) { *skip = len; return len; }

    for (int i = width; i > 0; i--) {
        if (s[i] == ' ') { *skip = i + 1; return i; }
    }
    /* One token longer than the line. Breaking mid-word is ugly and losing the
     * tail is worse: JS8 free text carries callsigns and locators that are
     * exactly this shape. */
    *skip = width;
    return width;
}

int js8_page_rows_for(const js8_chat_msg_t *m)
{
    if (!m) return 1;
    const char *p = m->text;
    int rows = 0;
    do {
        int skip, n = wrap_at(p, JS8_PAGE_W_TEXT, &skip);
        (void)n;
        p += skip;
        rows++;
    } while (*p && rows < JS8_PAGE_BODY_ROWS);
    return rows ? rows : 1;
}

int js8_page_render(const js8_chat_msg_t *msgs, int n,
                    char lines[JS8_PAGE_ROWS][JS8_PAGE_COLS + 1])
{
    for (int r = 0; r < JS8_PAGE_ROWS; r++) blank(lines[r]);

    if (n <= 0 || !msgs) {
        /* Say which band is being listened to, not "no data". An empty page
         * with no explanation reads as a broken decoder, and free text really
         * is rare: one run in 82 slots on the bench on 2026-10-09. */
        put(lines[JS8_PAGE_ROW0], JS8_PAGE_COL_TIME,
            "nothing heard yet - JS8 traffic appears here as it decodes", 58);
        return 0;
    }

    int row = JS8_PAGE_ROW0;
    for (int i = 0; i < n && row < JS8_PAGE_ROWS; i++) {
        const js8_chat_msg_t *m = &msgs[i];

        /* Live beats truncated in the flag column. A run that is still
         * arriving will show '~' as soon as it stops, so nothing is hidden
         * permanently, and '*' is the mark that decays. */
        lines[row][0] = m->active     ? JS8_PAGE_FLAG_LIVE
                      : m->truncated  ? JS8_PAGE_FLAG_TRUNC
                                      : JS8_PAGE_FLAG_NONE;

        char t[9];
        hhmmss(m->first_utc, t);
        put(lines[row], JS8_PAGE_COL_TIME, t, 8);

        char hz[6];
        snprintf(hz, sizeof(hz), "%4d", m->freq_hz);
        put(lines[row], JS8_PAGE_COL_HZ, hz, 4);

        put(lines[row], JS8_PAGE_COL_SENDER,
            m->sender[0] ? m->sender : JS8_PAGE_UNKNOWN_SENDER,
            JS8_PAGE_W_SENDER);

        /* do/while, so a message whose text is empty still takes its row.
         * "6 frames, no text" is a real state of a free-text run - every frame
         * decoded to nothing - and it must be visible, not skipped. */
        const char *p = m->text;
        do {
            if (row >= JS8_PAGE_ROWS) break;
            int skip, len = wrap_at(p, JS8_PAGE_W_TEXT, &skip);
            put(lines[row], JS8_PAGE_COL_TEXT, p, len);
            p += skip;
            row++;
        } while (*p);
    }
    return row - JS8_PAGE_ROW0;
}
