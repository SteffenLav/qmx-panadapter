#pragma once
/* A very small ANSI/VT100 screen model, sized for the QMX's terminal.
 *
 * Portable on purpose - no ESP or LVGL dependencies - so test/ansi_term_harness.c
 * can drive the real parser. The parser is the risky part of the terminal
 * feature: everything else is opening a USB port and drawing a grid.
 *
 * SCOPE IS DELIBERATELY THE MEASURED SUBSET. Captured from a QMX 1_04_004 on
 * 2026-08-16 (994 bytes for one full repaint of the main menu), the radio uses
 * exactly these:
 *
 *     ESC[2J        clear screen          ESC[?25l   hide cursor
 *     ESC[H         cursor home           ESC[33m    yellow
 *     ESC[<r>;<c>H  cursor position       ESC[37m    white
 *     ESC[7m        reverse video         ESC[m      reset attributes
 *
 * No scrolling regions, no character sets, no mouse, no wrapping games. Anything
 * unrecognised is SWALLOWED rather than printed - a stray escape must never
 * appear as literal junk in the middle of the operator's menu.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ANSI_COLS 80
#define ANSI_ROWS 24

/* Colour is stored as the SGR number the radio sent (30-37), not as RGB: the
 * renderer decides what "yellow" looks like on its own display, and the model
 * stays honest about what the radio actually said. 0 means "default". */
/* How many DISTINCT unhandled SGR parameters to remember (#215). Small on
 * purpose: the point is to learn which codes exist, not to log a stream. */
#define ANSI_UNK_MAX 8

typedef struct {
    char    ch;
    uint8_t fg;       /* 0 = default, else the 30-37 SGR code */
    bool    reverse;  /* ESC[7m - the QMX marks the selected menu row this way */
} ansi_cell_t;

typedef struct {
    ansi_cell_t cell[ANSI_ROWS][ANSI_COLS];
    int  cur_r, cur_c;          /* 0-based */
    uint8_t cur_fg;
    bool cur_reverse;
    bool cursor_visible;
    /* Escape-sequence assembly state. Private, but exposed so the whole struct
     * can live in a caller-owned buffer (PSRAM on the device). */
    int  esc_state;             /* 0 = text, 1 = seen ESC, 2 = inside CSI */
    char esc_buf[16];
    int  esc_len;
    uint32_t dirty_seq;         /* bumped on every visible change - lets a UI
                                   redraw only when something actually moved */
    /* SGR parameters this parser does not implement, recorded rather than
     * dropped silently (#215). Every byte ever captured from the QMX's MENU
     * screens uses only 0/7/27/33/37, but Samuel W7STF reports the Diagnostics
     * screen "uses red" and shows green button labels we never render - so it
     * sends something else, and a silently discarded parameter left no way to
     * find out what. Reported through /api/term so the answer comes from the
     * radio, not from a guess. */
    int16_t  unk[ANSI_UNK_MAX];
    uint8_t  unk_n;             /* distinct codes recorded */
    uint32_t unk_count;         /* total occurrences, including repeats */
} ansi_term_t;

/* Blank the screen, home the cursor, reset attributes. */
void ansi_term_reset(ansi_term_t *t);

/* Feed received bytes. Safe to call with any split across sequence boundaries -
 * an escape sequence arriving in two USB packets is normal and must not be
 * mis-parsed, which is exactly what the harness checks. */
void ansi_term_feed(ansi_term_t *t, const uint8_t *data, size_t len);

/* One row as a NUL-terminated string, for logging and for the web UI. `out`
 * must hold at least ANSI_COLS + 1 bytes. Trailing blanks are kept, so columns
 * line up in a plain-text dump. */
void ansi_term_row_text(const ansi_term_t *t, int row, char *out);
