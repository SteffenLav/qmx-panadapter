/* Host test: the decoded-CW grid WRAPS over the chosen number of rows.
 *
 *   sh test/build_cw_grid_rows.sh
 *
 * ⛔ A SCRIPT, NOT A gcc LINE: the ESP half of cw_decode.c is behind
 * #ifdef ESP_PLATFORM, so this builds it with -DESP_PLATFORM against
 * test/shim/, and the shim itself must be compiled separately (see
 * test/shim/README.md).
 *
 * ⛔ WHY THIS EXISTS. Bryan N0LUF asked for a 2-or-4-line choice, and the
 * count is the WRAP MODULUS, not a draw count. Get that wrong and nothing
 * errors: the text keeps wrapping over four rows while two are drawn, so half
 * of every pass is decoded into rows nobody can see and the visible half
 * appears to jump. That is a silent, on-air-only failure, which is exactly
 * the kind this project has been caught by before - so the modulus is pinned
 * here rather than reasoned about.
 *
 * The SLOT MAPPING in ui.c (fewer rows must leave the TOP slots unused, so the
 * strip keeps its bottom edge) is LVGL and is NOT covered here. It has to be
 * seen on the glass.
 */
#include <stdio.h>
#include <string.h>

#include "cw_decode.h"
#include "esp_timer.h"

static int fails;

static void expect(const char *what, int got, int want)
{
    if (got == want) { printf("  ok   %s\n", what); return; }
    printf("  FAIL %s: got %d want %d\n", what, got, want);
    fails++;
}

/* Push n characters through the REAL path: a "TBtnns;" response per chunk,
 * which is what cat.c feeds it. t = TX pending, nn = how many characters
 * follow - the parser reads that length, so it has to be right.
 *
 * ⚠ 'A', not 'E'. E and T are the single-element characters the squelch
 * treats as probable noise (is_noise_char), so a run of them is HELD and
 * never reaches the grid. The first version of this used 'E' and every cursor
 * assertion read 0 - the harness was feeding the decoder nothing and the
 * decoder was right to drop it. */
static void feed(int n)
{
    while (n > 0) {
        int k = (n > 32) ? 32 : n;
        char body[33], resp[64];
        memset(body, 'A', (size_t)k);
        body[k] = '\0';
        snprintf(resp, sizeof(resp), "TB0%02d%s;", k, body);
        cw_decode_feed(resp);
        shim_now_us += 1000;        /* 1 ms apart: no transcript line break */
        n -= k;
    }
}

/* How many columns of the returned line are non-blank. The line is the grid
 * with a two-column gap laid over the write position, so a full grid reads as
 * cells-2 non-blank. */
static int ink(void)
{
    char buf[CW_GRID_CELLS + 1];
    size_t n = cw_decode_line(buf, sizeof(buf));
    int c = 0;
    for (size_t i = 0; i < n; i++) if (buf[i] != ' ') c++;
    return c;
}

static size_t line_len(void)
{
    char buf[CW_GRID_CELLS + 1];
    return cw_decode_line(buf, sizeof(buf));
}

/* THE MODULUS. With r rows chosen the line is r*COLS long and the write
 * position must come back to 0 after exactly that many characters - not after
 * CW_LINE_ROWS*COLS. */
static void test_wrap_modulus(int rows)
{
    printf("wrap modulus at %d rows\n", rows);
    cw_decode_set_rows(rows);
    const int cells = rows * CW_LINE_COLS;

    expect("line length", (int)line_len(), cells);
    expect("cursor starts at 0", cw_decode_line_col(), 0);

    feed(cells - 1);
    expect("cursor one short of the end", cw_decode_line_col(), cells - 1);

    feed(1);
    expect("cursor wrapped to 0, not past it", cw_decode_line_col(), 0);

    feed(3);
    expect("and carries on from there", cw_decode_line_col(), 3);
}

/* Nothing may be written beyond the rows in use. A modulus taken over the
 * whole buffer would leave text in rows 3 and 4 while two were drawn - the
 * silent half of the defect this guards. */
static void test_nothing_past_the_rows(void)
{
    printf("nothing is written past the rows in use\n");
    cw_decode_set_rows(4);
    feed(4 * CW_LINE_COLS);            /* fill all four */
    cw_decode_set_rows(2);             /* wipes, by design */

    char buf[CW_GRID_CELLS + 1];
    memset(buf, '#', sizeof(buf));     /* poison, so a short copy shows */
    size_t n = cw_decode_line(buf, sizeof(buf));
    expect("only two rows are returned", (int)n, 2 * CW_LINE_COLS);
    int dirty = 0;
    for (size_t i = 0; i < n; i++) if (buf[i] != ' ') dirty++;
    expect("and they are blank after the change", dirty, 0);

    /* Fill the two rows twice over; the third and fourth must stay untouched,
       which the API can only show as the line never growing. */
    feed(6 * CW_LINE_COLS);
    expect("still two rows after overfilling", (int)line_len(), 2 * CW_LINE_COLS);
    expect("cursor still inside them", cw_decode_line_col() < 2 * CW_LINE_COLS, 1);
}

/* Changing the count WIPES - the documented behaviour, because old text in
 * rows that are no longer reachable (or no longer blank) is a ghost. */
static void test_change_wipes(void)
{
    printf("changing the count wipes the window\n");
    cw_decode_set_rows(4);
    feed(100);
    expect("there is text", ink() > 0, 1);
    cw_decode_set_rows(2);
    expect("wiped on 4 -> 2", ink(), 0);

    feed(100);
    expect("there is text again", ink() > 0, 1);
    cw_decode_set_rows(4);
    expect("wiped on 2 -> 4", ink(), 0);

    /* Setting the SAME count must not wipe - the setter is called on every
       settings write, and losing the window on an unrelated save would be a
       defect of its own. */
    feed(50);
    int before = ink();
    cw_decode_set_rows(4);
    expect("setting the same count keeps the text", ink(), before);
}

/* Out-of-range values are clamped, not honoured. settings.c refuses anything
 * but 2 and 4, but this is the last line of defence and a 0 here would be a
 * division by zero in the modulus. */
static void test_clamping(void)
{
    printf("clamping\n");
    cw_decode_set_rows(0);
    expect("0 clamps to 1", cw_decode_rows(), 1);
    cw_decode_set_rows(-5);
    expect("negative clamps to 1", cw_decode_rows(), 1);
    cw_decode_set_rows(99);
    expect("99 clamps to the ceiling", cw_decode_rows(), CW_LINE_ROWS);

    cw_decode_set_rows(1);
    feed(CW_LINE_COLS + 5);            /* must not divide by zero or run off */
    expect("one row still wraps", cw_decode_line_col(), 5);
}

int main(void)
{
    cw_decode_init();
    expect("defaults to the compile-time maximum", cw_decode_rows(), CW_LINE_ROWS);

    test_wrap_modulus(4);
    cw_decode_clear();
    test_wrap_modulus(2);
    test_nothing_past_the_rows();
    test_change_wipes();
    test_clamping();

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}

/* MUTATION TESTS - run 2026-10-17 by test/mutate_cw_grid_rows.py. Each applied
 * alone to main/cw_decode.c, rebuilt, run, reverted. All KILLED.
 *
 *  1. The wrap modulus goes back to CW_GRID_CELLS in cw_decode_feed()
 *       -> "cursor wrapped to 0, not past it" FAILS at 2 rows: the cursor
 *          reads 144, i.e. the text has carried on into row 3 of a two-row
 *          strip. That is the silent defect, in the open.
 *  2. cw_decode_line() returns CW_GRID_CELLS again
 *       -> "line length" FAILS: 288 where 144 is in use.
 *  3. cw_decode_set_rows() does not wipe
 *       -> "and they are blank after the change" FAILS: 142 columns of a
 *          four-row pass still showing in a two-row strip.
 *  4. cw_decode_set_rows() wipes even when the count is unchanged
 *       -> "setting the same count keeps the text" FAILS. Worth guarding:
 *          settings.c calls the setter on every write of that key.
 *  5. The low clamp removed (rows < 1)
 *       -> "0 clamps to 1" FAILS, and the modulus would divide by zero.
 *  6. The high clamp removed (rows > CW_LINE_ROWS)
 *       -> "99 clamps to the ceiling" FAILS, and the grid would be written
 *          past the end of s_line.
 */
