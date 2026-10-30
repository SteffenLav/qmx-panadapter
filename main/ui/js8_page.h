#pragma once

/* The JS8 conversation page, composed as TEXT.
 *
 * Same shape as gps_page.c and for the same reason: a PURE function of a
 * snapshot, no LVGL, no clock, no store. The layout is the part that can be
 * wrong without the hardware noticing - a column off by one, a message that
 * wraps into the sender field, a truncation marker lost off the right edge -
 * and test/js8_page_harness.c pins it down on a PC instead of on the glass.
 *
 * The grid is 80x24 in qmx_mono_25, the grid the Tab5 already renders for the
 * radio's own screens and for the GPS page.
 */

#include <stdbool.h>
#include <stdint.h>

#include "js8_chat.h"

/* The JS8 screen's right pane is 960 x 580 px and qmx_mono_25 advances 15 px
 * per column over 27 px rows. The pane spends 30 px on its own column header -
 * the view draws that, not this module - leaving 550 px, so the grid is
 * exactly 64 x 20. It was 80 x 24 while this was a full-screen overlay. */
#define JS8_PAGE_COLS 64
#define JS8_PAGE_ROWS 20

/* Column geometry. One row of a message reads:
 *
 *   *  12s 1275 40m  HB9BV    F6HCM HB9BV HEARTBEAT SNR
 *   ^  ^   ^    ^    ^        ^
 *   0  2   7    12   17       27
 *
 * The flag column is first so the live and truncated marks line up down the
 * left edge and can be found without reading the rows.
 *
 * ⛔ BAND IS A COLUMN, and it cost the text column 5 characters (42 -> 37).
 * The grid was exactly full, so there was nowhere else for it to come from.
 * It is worth the 5: the offset column next to it is the AUDIO offset, so
 * before this the page could not say which band a conversation ran on, and a
 * session that worked several bands rendered as one unattributable list.
 * Operator asked for it 2026-10-11. "F6HCM HB9BV HEARTBEAT SNR" is 26
 * characters, so the common structured row still fits on one line; long free
 * text wraps one row sooner than it used to.
 *
 * ⚠ FOUR WIDE, not three. "160m" and "70cm" are both 4.
 *
 * ⭐ AGE, NOT A WALL CLOCK. The Stations list next to this one shows AGE and
 * no clock, and 64 columns do not hold both: clock + age + offset + sender
 * would leave 33 for the message, and "F6HCM HB9BV HEARTBEAT SNR" is 26 on
 * its own. Operator, 2026-10-09: "age in sec needs to be there". */
/* ⛔ THE SUBMODE MARK, column 1 - the one character between the flag column
 * and AGE that the layout already left blank. T132 decodes ONE alternate JS8
 * submode on a duty cycle as well as the one we are operating, so a row no
 * longer implies the primary submode and the page has to say which speed
 * heard the station.
 *
 * ⭐ IT COSTS THE MESSAGE COLUMN NOTHING. The grid was exactly full when BAND
 * took five characters off the text (see above), and the operator noticed; a
 * second column would have taken more. Column 1 was already spare, and it is
 * where the eye scans for the flag anyway.
 *
 * BLANK for the primary submode, so an ordinary single-submode session shows
 * no new column at all. 'N' Normal, 'F' Fast, 'T' Turbo, 'S' Slow - see
 * js8_alt_mark(). */
#define JS8_PAGE_COL_SUBMODE 1
#define JS8_PAGE_W_SUBMODE   1

#define JS8_PAGE_COL_AGE     2
#define JS8_PAGE_W_AGE       4
#define JS8_PAGE_COL_HZ      7
/* ⚠ The Hz width was a bare 4 in js8_page.c's put() call while every other
 * column had a define. The conversation header centres its titles over these
 * widths, so a literal here is a drift waiting to happen. */
#define JS8_PAGE_W_HZ        4
#define JS8_PAGE_COL_BAND    12
#define JS8_PAGE_W_BAND      4
#define JS8_PAGE_COL_SENDER  17
#define JS8_PAGE_W_SENDER    9
#define JS8_PAGE_COL_TEXT    27
#define JS8_PAGE_W_TEXT      (JS8_PAGE_COLS - JS8_PAGE_COL_TEXT)   /* 37 */

/* Shown in the band column when the dial was not known at the time - an old
 * stored entry, or CAT down in that slot. Must not read as a band. */
#define JS8_PAGE_UNKNOWN_BAND    "-"

/* ⛔ NO TITLE ROW. The first version drew "JS8 - conversation" and a rule into
 * rows 0 and 1, and the view draws its own header above the grid with the
 * Close button in it - so the page said its own name twice, one line apart.
 * Seen on the glass 2026-10-09, not by the tests, because both headings were
 * correct in isolation. The dial moved into the view's header with the title;
 * this module is now only the message grid, and gained two rows for it. */
#define JS8_PAGE_ROW0        0
#define JS8_PAGE_BODY_ROWS   JS8_PAGE_ROWS                         /* 24 */

/* Flag column. */
#define JS8_PAGE_FLAG_LIVE      '*'   /* frames still arriving            */
#define JS8_PAGE_FLAG_TRUNC     '~'   /* text was cut - see below         */
#define JS8_PAGE_FLAG_NONE      ' '

/* Shown in the sender field when a free-text run was never attributed. A data
 * frame carries no callsign, so this is the normal case, not an error - it
 * must not read as one, and it must not read as an empty column either. */
#define JS8_PAGE_UNKNOWN_SENDER  "- - -"

/* Render `n` messages, NEWEST FIRST, into `lines`.
 *
 * ⭐ NEWEST AT THE TOP, which is not how a conversation reads. The page is
 * opened to see what just happened, and this way the newest message needs no
 * scrolling. Flipping it is one loop here and nothing else.
 *
 * Each line comes back NUL-terminated and exactly JS8_PAGE_COLS wide, trailing
 * spaces included, as the grid has them. Returns the number of rows used.
 */
/* `now_utc` is what AGE is measured against - the caller's clock, because this
 * module has none.
 *
 * `row_msg`, when given, receives the message index each row belongs to, or -1
 * for a row with no message. ⛔ THE PANE NEEDS THIS TO BE TAPPABLE: a message
 * can wrap onto several rows, so a tap at row r cannot be turned into a
 * message by arithmetic. Operator, 2026-10-09: the TX target is whatever row
 * you tap, not the newest message. */
/* `primary_proto` is the ftx_protocol_t the station is OPERATING on, cast to
 * uint8_t so this module keeps no dependency on the decoder. It decides only
 * which rows get a submode mark - a message heard on it is unmarked. */
int js8_page_render(const js8_chat_msg_t *msgs, int n, int64_t now_utc,
                    uint8_t primary_proto,
                    char lines[JS8_PAGE_ROWS][JS8_PAGE_COLS + 1],
                    int row_msg[JS8_PAGE_ROWS]);

/* Rows one message needs at JS8_PAGE_W_TEXT per line, at least 1. Exposed so
 * the view can size a scroll region without rendering the page twice. */
int js8_page_rows_for(const js8_chat_msg_t *m);
