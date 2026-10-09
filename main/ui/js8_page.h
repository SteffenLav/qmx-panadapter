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

#include "js8_chat.h"

#define JS8_PAGE_COLS 80
#define JS8_PAGE_ROWS 24

/* Column geometry. One row of a message reads:
 *
 *   * 13:54:00 1188 LA7HKA    G4GHL LA7HKA HEARTBEAT SNR
 *   ^ ^        ^    ^         ^
 *   0 2        11   16        26
 *
 * The flag column is first so the live and truncated marks line up down the
 * left edge and can be found without reading the rows. */
#define JS8_PAGE_COL_TIME    2
#define JS8_PAGE_COL_HZ      11
#define JS8_PAGE_COL_SENDER  16
#define JS8_PAGE_W_SENDER    9
#define JS8_PAGE_COL_TEXT    26
#define JS8_PAGE_W_TEXT      (JS8_PAGE_COLS - JS8_PAGE_COL_TEXT)   /* 54 */

/* Row 0 is the title, row 1 the rule, so 22 rows carry messages. */
#define JS8_PAGE_ROW0        2
#define JS8_PAGE_BODY_ROWS   (JS8_PAGE_ROWS - JS8_PAGE_ROW0)       /* 22 */

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
 * `subtitle` is right-aligned on the title row - the band and dial the caller
 * knows and this module does not. May be NULL.
 *
 * Each line comes back NUL-terminated and exactly JS8_PAGE_COLS wide, trailing
 * spaces included, as the grid has them. Returns the number of body rows used.
 */
int js8_page_render(const js8_chat_msg_t *msgs, int n, const char *subtitle,
                    char lines[JS8_PAGE_ROWS][JS8_PAGE_COLS + 1]);

/* Rows one message needs at JS8_PAGE_W_TEXT per line, at least 1. Exposed so
 * the view can size a scroll region without rendering the page twice. */
int js8_page_rows_for(const js8_chat_msg_t *m);
