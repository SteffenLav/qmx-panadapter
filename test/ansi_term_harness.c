/* Host test for the QMX terminal's ANSI screen model.
 *
 * Build (from the repo root):
 *   gcc -I main/util -o ansi_term_harness test/ansi_term_harness.c \
 *       main/util/ansi_term.c && ./ansi_term_harness
 *
 * The important vector is REAL: `qmx_menu[]` below is the actual byte stream the
 * bench QMX (firmware 1_04_004) sent on 2026-08-16 when a CR was written to its
 * second serial port, transcribed from the hex dump in the device log. Testing a
 * parser against bytes you invented mostly proves you can invent bytes.
 */
#include <stdio.h>
#include <string.h>
#include "ansi_term.h"

static int g_fail = 0;

static void check(const char *what, int ok)
{
    printf("%-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_fail++;
}

static void check_row(ansi_term_t *t, int row, const char *expect_trimmed)
{
    char buf[ANSI_COLS + 1];
    ansi_term_row_text(t, row, buf);
    /* Trim trailing blanks for comparison; leading layout is significant. */
    int e = ANSI_COLS;
    while (e > 0 && buf[e - 1] == ' ') e--;
    buf[e] = '\0';
    char label[80];
    snprintf(label, sizeof(label), "row %d == \"%s\"", row, expect_trimmed);
    int ok = strcmp(buf, expect_trimmed) == 0;
    printf("%-52s %s", label, ok ? "PASS\n" : "FAIL\n");
    if (!ok) { printf("      got: \"%s\"\n", buf); g_fail++; }
}

/* The real capture: ESC[2J ESC[H ESC[?25l ESC[33m ESC[24;1H " QMX v1_04_004 ..."
 * then the menu box drawn row by row, with the title in reverse video. */
static const unsigned char qmx_menu[] = {
 0x1b,0x5b,0x32,0x4a,0x1b,0x5b,0x48,0x1b,0x5b,0x3f,0x32,0x35,0x6c,0x1b,0x5b,0x33,
 0x33,0x6d,0x1b,0x5b,0x32,0x34,0x3b,0x31,0x48,0x20,0x51,0x4d,0x58,0x20,0x76,0x31,
 0x5f,0x30,0x34,0x5f,0x30,0x30,0x34,0x20,0x20,0x20,0x51,0x52,0x50,0x20,0x4c,0x61,
 0x62,0x73,0x2c,0x20,0x32,0x30,0x32,0x36,0x20,0x1b,0x5b,0x33,0x37,0x6d,0x1b,0x5b,
 0x33,0x37,0x6d,0x1b,0x5b,0x3f,0x32,0x35,0x6c,0x1b,0x5b,0x32,0x3b,0x32,0x48,0x2b,
 0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,0x2d,
 0x2d,0x2d,0x2b,0x1b,0x5b,0x32,0x3b,0x36,0x48,0x1b,0x5b,0x37,0x6d,0x4d,0x61,0x69,
 0x6e,0x20,0x6d,0x65,0x6e,0x75,0x1b,0x5b,0x6d,0x1b,0x5b,0x37,0x6d,0x1b,0x5b,0x33,
 0x37,0x6d,0x1b,0x5b,0x34,0x3b,0x34,0x48,0x20,0x43,0x6f,0x6e,0x66,0x69,0x67,0x75,
 0x72,0x61,0x74,0x69,0x6f,0x6e,0x20,0x20,0x1b,0x5b,0x6d,0x1b,0x5b,0x33,0x37,0x6d,
 0x1b,0x5b,0x33,0x37,0x6d,0x1b,0x5b,0x35,0x3b,0x34,0x48,0x20,0x48,0x61,0x72,0x64,
 0x77,0x61,0x72,0x65,0x20,0x74,0x65,0x73,0x74,0x73,0x20,0x1b,0x5b,0x33,0x37,0x6d,
 0x1b,0x5b,0x33,0x37,0x6d,0x1b,0x5b,0x36,0x3b,0x34,0x48,0x20,0x50,0x43,0x20,0x61,
 0x6e,0x64,0x20,0x43,0x41,0x54,0x20,0x20,0x20,0x20,0x20,0x1b,0x5b,0x33,0x37,0x6d,
 0x1b,0x5b,0x33,0x37,0x6d,0x1b,0x5b,0x37,0x3b,0x34,0x48,0x20,0x53,0x79,0x73,0x74,
 0x65,0x6d,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x1b,0x5b,0x33,0x37,0x6d,
 0x1b,0x5b,0x33,0x37,0x6d,0x1b,0x5b,0x38,0x3b,0x34,0x48,0x20,0x45,0x78,0x69,0x74,
 0x20,0x74,0x65,0x72,0x6d,0x69,0x6e,0x61,0x6c,0x20,0x20,0x1b,0x5b,0x33,0x37,0x6d,
};

int main(void)
{
    ansi_term_t t;

    /* ---- The real capture, fed in one go ---- */
    ansi_term_reset(&t);
    ansi_term_feed(&t, qmx_menu, sizeof(qmx_menu));

    /* The title is drawn INTO the top border, group-box style: the box row is
       written first, then ESC[2;6H puts "Main menu" over the middle of it. I
       expected a plain "+---...---+" here and the radio corrected me. */
    check_row(&t, 1,  " +---Main menu------+");
    check_row(&t, 3,  "    Configuration");
    check_row(&t, 4,  "    Hardware tests");
    check_row(&t, 5,  "    PC and CAT");
    check_row(&t, 6,  "    System");
    check_row(&t, 7,  "    Exit terminal");
    check_row(&t, 23, " QMX v1_04_004   QRP Labs, 2026");

    check("title 'Main menu' is REVERSE video", t.cell[1][5].reverse && t.cell[1][5].ch == 'M');
    /* ⭐ THE SELECTED MENU ITEM IS MARKED WITH REVERSE VIDEO, and nothing else
       distinguishes it. The radio emits ESC[m then ESC[7m again before drawing
       "Configuration", which is the first item and therefore the highlighted
       one. A renderer that drops the reverse attribute would leave the operator
       unable to see where they are in the menu - so this is a load-bearing
       assertion, not a curiosity. */
    check("SELECTED item (Configuration) is reverse", t.cell[3][4].reverse);
    check("a LATER item (System) is not reverse",     !t.cell[6][4].reverse);
    check("banner row carries a colour",       t.cell[23][1].fg != 0);
    check("cursor was hidden (ESC[?25l)",      t.cursor_visible == false);

    /* ---- The same bytes, split at EVERY possible boundary ----
     * USB delivers whatever it delivers; an escape sequence arriving in two
     * packets is normal. If any split changes the result, the parser is holding
     * state wrongly - and that bug would be maddening in the field because it
     * would depend on packet timing. */
    {
        char ref[ANSI_ROWS][ANSI_COLS + 1];
        for (int r = 0; r < ANSI_ROWS; r++) ansi_term_row_text(&t, r, ref[r]);

        int bad_splits = 0;
        for (size_t cut = 1; cut < sizeof(qmx_menu); cut++) {
            ansi_term_t s;
            ansi_term_reset(&s);
            ansi_term_feed(&s, qmx_menu, cut);
            ansi_term_feed(&s, qmx_menu + cut, sizeof(qmx_menu) - cut);
            for (int r = 0; r < ANSI_ROWS; r++) {
                char got[ANSI_COLS + 1];
                ansi_term_row_text(&s, r, got);
                if (strcmp(got, ref[r]) != 0) { bad_splits++; break; }
            }
        }
        char label[80];
        snprintf(label, sizeof(label), "identical for all %d byte-split points",
                 (int)sizeof(qmx_menu) - 1);
        check(label, bad_splits == 0);
        if (bad_splits) printf("      %d split point(s) differed\n", bad_splits);
    }

    /* ---- Individual behaviours ---- */
    ansi_term_reset(&t);
    ansi_term_feed(&t, (const uint8_t *)"\x1b[5;10HX", 8);
    check("ESC[5;10H places at row 4 col 9 (0-based)", t.cell[4][9].ch == 'X');

    ansi_term_feed(&t, (const uint8_t *)"\x1b[2J", 4);
    check("ESC[2J clears it again", t.cell[4][9].ch == ' ');

    /* An unknown sequence must be swallowed whole, not partly printed. */
    ansi_term_reset(&t);
    ansi_term_feed(&t, (const uint8_t *)"\x1b[1;2;3;4;5r" "OK", 14);
    check_row(&t, 0, "OK");

    /* A lone ESC followed by an ordinary letter must not eat the letter's line. */
    ansi_term_reset(&t);
    ansi_term_feed(&t, (const uint8_t *)"\x1b" "ZHELLO", 7);
    check_row(&t, 0, "HELLO");

    /* Writing past the right edge must stop, never wrap onto the next row -
     * a wrap would silently corrupt a row the radio never wrote to. */
    ansi_term_reset(&t);
    {
        char longline[120];
        memset(longline, 'A', sizeof(longline));
        ansi_term_feed(&t, (const uint8_t *)longline, sizeof(longline));
    }
    check("overlong line does not wrap to row 1", t.cell[1][0].ch == ' ');

    /* A CSI with an absurdly long parameter run must not overrun esc_buf.
     *
     * This parser eats bytes straight off a USB port, so the length bound is a
     * memory-safety property, not tidiness: esc_buf is 16 bytes inside the
     * struct, with esc_len and dirty_seq immediately after it. A mutation run
     * showed no existing test exercised the bound at all - the sequence below
     * is 300 parameter bytes, which without the check walks straight through
     * the neighbouring fields and on into the cell grid.
     *
     * The assertion is that the terminal still WORKS afterwards: the long
     * sequence is swallowed, and the text following it lands where it should. */
    ansi_term_reset(&t);
    {
        char evil[320];
        int n = 0;
        evil[n++] = 0x1b; evil[n++] = '[';
        while (n < 300) evil[n++] = (n % 2) ? '1' : ';';
        ansi_term_feed(&t, (const uint8_t *)evil, n);   /* NO final byte yet */

        /* ⚠ HONEST LIMIT OF THIS CHECK. It asserts the invariant, and it passes
           on correct code - but it does NOT catch the bound being removed, and I
           verified that by trying: with `else if (1)` in place of the length
           test, the first thing the overrun destroys is esc_len ITSELF (it sits
           immediately after esc_buf in the struct), so the counter is scrambled
           into a small value and this assertion reads as healthy. The observable
           is the field being corrupted.
           A canary after the struct fares no better, because once esc_len is
           garbage the writes land at an unpredictable offset rather than
           marching forward. So the bound in ansi_term.c is NOT covered by a
           mutation-detectable test; it is covered by review. Said plainly so
           nobody later reads this block as proof that it is. */
        check("CSI param run stays inside esc_buf",
              t.esc_len < (int)sizeof(t.esc_buf));

        const char *rest = "H" "SAFE";
        ansi_term_feed(&t, (const uint8_t *)rest, (int)strlen(rest));
    }
    {
        /* Whatever that sequence positioned to, normal parsing must resume. */
        ansi_term_feed(&t, (const uint8_t *)"\x1b[12;1HRECOVERED", 17);
        check_row(&t, 11, "RECOVERED");
    }

    /* dirty_seq must move when the screen changes, so a UI can skip redraws. */
    ansi_term_reset(&t);
    {
        uint32_t before = t.dirty_seq;
        ansi_term_feed(&t, (const uint8_t *)"hello", 5);
        uint32_t after = t.dirty_seq;
        ansi_term_feed(&t, (const uint8_t *)"\x1b[37m", 5);   /* colour only, no cells */
        check("dirty_seq advances on visible change", after > before);
        check("dirty_seq still valid after attr-only", t.dirty_seq >= after);
    }

    printf("\n%s\n", g_fail ? "FAILURES ABOVE" : "all checks passed");
    return g_fail ? 1 : 0;
}
