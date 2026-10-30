/* Host test for main/ui/js8_page.c - the JS8 conversation page layout.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main -I main/ui -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -o js8_page_harness test/js8_page_harness.c main/ui/js8_page.c \
 *       main/js8_alt.c components/ft8_lib/ft8/constants.c \
 *       components/ft8_lib/ft8/js8_tables.c -lm \
 *       && ./js8_page_harness
 *
 * The page is a pure function, so every way it can be wrong is reachable here:
 * a column off by one, a message wrapping into the sender field, a long word
 * losing its tail, a row count that disagrees with what was drawn. None of
 * those make the board misbehave, which is exactly why they would survive on
 * the glass.
 */
#include <stdio.h>
#include <string.h>

#include "js8_page.h"
#include "js8_alt.h"

/* The sub-mode the station is operating in these tests. T132 marks a row
 * only when it was heard on something ELSE. */
#define PRIMARY  ((uint8_t)FTX_PROTOCOL_JS8)

static int fails;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static char L[JS8_PAGE_ROWS][JS8_PAGE_COLS + 1];
static int  M[JS8_PAGE_ROWS];

/* A fixed 'now' so AGE is a constant in the tests, not a clock. */
#define NOW 1791554100

static js8_chat_msg_t mk(int64_t first, int hz, const char *sender,
                         const char *text, bool active, bool trunc)
{
    js8_chat_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = JS8_CHAT_FREETEXT;
    /* The primary sub-mode, so the T132 mark column stays blank in every
     * existing case - those tests are about the OTHER columns and a letter
     * appearing in column 1 would fail them for the wrong reason. */
    m.proto = (uint8_t)PRIMARY;
    m.first_utc = first;
    m.last_utc = first;
    m.freq_hz = hz;
    m.frames = 1;
    m.snr_db = JS8_CHAT_NO_SNR;
    m.active = active;
    m.truncated = trunc;
    snprintf(m.sender, sizeof(m.sender), "%s", sender);
    snprintf(m.text, sizeof(m.text), "%s", text);
    return m;
}

/* The grid is space-padded, so a field is compared by position and length. */
static bool at(int row, int col, const char *want)
{
    return strncmp(&L[row][col], want, strlen(want)) == 0;
}

static void t_no_title_row(void)
{
    printf("the grid holds messages only - the view draws the heading\n");
    js8_chat_msg_t m = mk(1791554040, 1188, "OZ1LAV", "FIRST", false, false);
    js8_page_render(&m, 1, NOW, PRIMARY, L, M);
    /* ⛔ The page drew "JS8 - conversation" into row 0 and the view drew the
     * same words in its own header, one line apart. Seen on the glass. */
    CHECK(strstr(L[0], "conversation") == NULL, "row 0 still carries a title: '%s'", L[0]);
    CHECK(L[0][0] != '-', "row 0 is still a rule");
    CHECK(at(0, JS8_PAGE_COL_TEXT, "FIRST"), "the first message is not on row 0: '%s'", L[0]);
}

static void t_every_line_is_exactly_the_grid_width(void)
{
    printf("every line is exactly %d characters\n", JS8_PAGE_COLS);
    js8_chat_msg_t m = mk(1791554040, 1188, "", "QSL", true, false);
    js8_page_render(&m, 1, NOW, PRIMARY, L, M);
    for (int r = 0; r < JS8_PAGE_ROWS; r++)
        CHECK((int)strlen(L[r]) == JS8_PAGE_COLS, "row %d is %d chars",
              r, (int)strlen(L[r]));
}

static void t_columns(void)
{
    printf("the fields land in their columns\n");
    /* 1791554040 = 2026-10-09 13:54:00 UTC - the first free-text decode this
     * project made, off the bench. */
    js8_chat_msg_t m = mk(1791554040, 1188, "", "QSL", true, false);
    js8_page_render(&m, 1, NOW, PRIMARY, L, M);
    int r = JS8_PAGE_ROW0;
    CHECK(L[r][0] == JS8_PAGE_FLAG_LIVE, "flag is '%c', want '*'", L[r][0]);
    /* 1791554100 - 1791554040 = 60 s. AGE is measured from last_utc, which
     * mk() sets equal to first_utc. */
    CHECK(at(r, JS8_PAGE_COL_AGE, " 60s"), "age column: '%.4s'",
          &L[r][JS8_PAGE_COL_AGE]);
    CHECK(at(r, JS8_PAGE_COL_HZ, "1188"), "hz column: '%.4s'", &L[r][JS8_PAGE_COL_HZ]);
    CHECK(at(r, JS8_PAGE_COL_SENDER, JS8_PAGE_UNKNOWN_SENDER),
          "sender column: '%.9s'", &L[r][JS8_PAGE_COL_SENDER]);
    /* ⛔ The rest of the field must be SPACES. A field shorter than its column
     * is padded, and the first version of put() kept indexing past the NUL, so
     * the padding came out of the next string literal in .rodata and this row
     * read "- - - JS8 QSL". Checking only the prefix did not see it. */
    for (int c = JS8_PAGE_COL_SENDER + (int)strlen(JS8_PAGE_UNKNOWN_SENDER);
         c < JS8_PAGE_COL_TEXT; c++)
        CHECK(L[r][c] == ' ', "column %d after a short sender is '%c', not a space",
              c, L[r][c]);
    CHECK(at(r, JS8_PAGE_COL_TEXT, "QSL"), "text column: '%.3s'",
          &L[r][JS8_PAGE_COL_TEXT]);
}

static void t_empty_says_why(void)
{
    printf("an empty page explains itself\n");
    js8_page_render(NULL, 0, NOW, PRIMARY, L, M);
    CHECK(strstr(L[JS8_PAGE_ROW0], "nothing decoded yet") != NULL,
          "empty page row reads '%s'", L[JS8_PAGE_ROW0]);
}

static void t_wrap_on_a_space(void)
{
    printf("long text wraps on a space and indents to the text column\n");
    /* 60 characters: longer than the 54-wide text column, with a space at 54. */
    const char *txt = "GOOD MORNING OM THANKS FOR THE CALL AND THE REPORT";
    js8_chat_msg_t m = mk(0, 1500, "OZ1LAV", txt, false, false);
    int used = js8_page_render(&m, 1, NOW, PRIMARY, L, M);
    CHECK(used == 2, "used %d rows, want 2", used);

    int r = JS8_PAGE_ROW0;
    /* Nothing of the first line may cross into the sender field. */
    CHECK(L[r][JS8_PAGE_COL_TEXT - 1] == ' ', "no gap before the text column");
    /* The continuation row carries no time, no Hz and no sender. */
    for (int c = 0; c < JS8_PAGE_COL_TEXT; c++)
        CHECK(L[r + 1][c] == ' ', "continuation row has '%c' at column %d",
              L[r + 1][c], c);
    CHECK(L[r + 1][JS8_PAGE_COL_TEXT] != ' ', "continuation row has no text");

    /* No character of the message may be lost at the break. */
    char joined[256];
    int n = 0;
    for (int c = JS8_PAGE_COL_TEXT; c < JS8_PAGE_COLS; c++) joined[n++] = L[r][c];
    while (n && joined[n - 1] == ' ') n--;
    joined[n++] = ' ';
    for (int c = JS8_PAGE_COL_TEXT; c < JS8_PAGE_COLS; c++) joined[n++] = L[r + 1][c];
    while (n && joined[n - 1] == ' ') n--;
    joined[n] = 0;
    CHECK(strcmp(joined, txt) == 0, "rejoined text differs:\n   got  '%s'\n   want '%s'",
          joined, txt);
}

static void t_long_word_keeps_its_tail(void)
{
    printf("a word longer than the column keeps its tail on the next row\n");
    char word[80];
    memset(word, 'A', 70);
    word[70] = 0;
    js8_chat_msg_t m = mk(0, 1500, "X", word, false, false);
    int used = js8_page_render(&m, 1, NOW, PRIMARY, L, M);
    CHECK(used == 2, "used %d rows, want 2", used);
    int r = JS8_PAGE_ROW0;
    CHECK(L[r][JS8_PAGE_COLS - 1] == 'A', "first row did not fill the column");
    /* 70 - JS8_PAGE_W_TEXT left over. */
    {
        int tail = 70 - JS8_PAGE_W_TEXT;
        for (int c = 0; c < tail; c++)
            CHECK(L[r + 1][JS8_PAGE_COL_TEXT + c] == 'A', "tail char %d missing", c);
        CHECK(L[r + 1][JS8_PAGE_COL_TEXT + tail] == ' ', "tail is longer than %d", tail);
    }
}

static void t_flags(void)
{
    printf("the flag column marks live and truncated\n");
    js8_chat_msg_t m[3] = {
        mk(0, 1000, "A", "LIVE", true,  false),
        mk(0, 2000, "B", "CUT",  false, true),
        mk(0, 3000, "C", "DONE", false, false),
    };
    js8_page_render(m, 3, NOW, PRIMARY, L, M);
    CHECK(L[JS8_PAGE_ROW0 + 0][0] == JS8_PAGE_FLAG_LIVE,  "row 0 flag '%c'", L[JS8_PAGE_ROW0][0]);
    CHECK(L[JS8_PAGE_ROW0 + 1][0] == JS8_PAGE_FLAG_TRUNC, "row 1 flag '%c'", L[JS8_PAGE_ROW0+1][0]);
    CHECK(L[JS8_PAGE_ROW0 + 2][0] == JS8_PAGE_FLAG_NONE,  "row 2 flag '%c'", L[JS8_PAGE_ROW0+2][0]);

    /* Live wins over truncated: a run still arriving shows '*', and shows '~'
     * as soon as it stops. */
    js8_chat_msg_t both = mk(0, 1000, "A", "X", true, true);
    js8_page_render(&both, 1, NOW, PRIMARY, L, M);
    CHECK(L[JS8_PAGE_ROW0][0] == JS8_PAGE_FLAG_LIVE, "live did not win: '%c'",
          L[JS8_PAGE_ROW0][0]);
}

static void t_empty_text_still_takes_a_row(void)
{
    printf("a run that decoded no text still takes a row\n");
    js8_chat_msg_t m[2] = {
        mk(100, 1000, "A", "", false, false),
        mk(200, 2000, "B", "SECOND", false, false),
    };
    int used = js8_page_render(m, 2, NOW, PRIMARY, L, M);
    CHECK(used == 2, "used %d rows, want 2", used);
    CHECK(at(JS8_PAGE_ROW0, JS8_PAGE_COL_SENDER, "A"), "first row lost its sender");
    CHECK(at(JS8_PAGE_ROW0 + 1, JS8_PAGE_COL_TEXT, "SECOND"), "second message moved up");
}

static void t_overflow_stops_at_the_last_row(void)
{
    printf("more messages than rows stops at the last row\n");
    js8_chat_msg_t m[JS8_CHAT_MAX_MSGS];
    for (int i = 0; i < JS8_CHAT_MAX_MSGS; i++)
        m[i] = mk(i, 1000 + i, "X", "HELLO", false, false);
    int used = js8_page_render(m, JS8_CHAT_MAX_MSGS, NOW, PRIMARY, L, M);
    CHECK(used == JS8_PAGE_BODY_ROWS, "used %d rows, want %d", used, JS8_PAGE_BODY_ROWS);
    for (int r = 0; r < JS8_PAGE_ROWS; r++)
        CHECK((int)strlen(L[r]) == JS8_PAGE_COLS, "row %d is %d chars after overflow",
              r, (int)strlen(L[r]));
}

static void t_row_map_names_the_message_each_row_belongs_to(void)
{
    printf("the row map says which message each row is part of\n");
    js8_chat_msg_t m[2] = {
        mk(NOW - 10, 1000, "A",
           "GOOD MORNING OM THANKS FOR THE CALL AND THE REPORT", false, false),
        mk(NOW - 20, 2000, "B", "SHORT", false, false),
    };
    int used = js8_page_render(m, 2, NOW, PRIMARY, L, M);
    CHECK(used == 3, "used %d rows, want 3 (2 wrapped + 1)", used);
    CHECK(M[0] == 0 && M[1] == 0, "the wrapped rows are %d,%d, want 0,0", M[0], M[1]);
    CHECK(M[2] == 1, "row 2 maps to %d, want 1", M[2]);
    for (int r = 3; r < JS8_PAGE_ROWS; r++)
        CHECK(M[r] == -1, "empty row %d maps to %d, want -1", r, M[r]);
}

/* ⛔ THE BAND COLUMN, by LITERAL position - not through JS8_PAGE_COL_BAND.
 *
 * Every other check in this file compares against the column defines, which is
 * right for catching an off-by-one in the renderer but is blind to the whole
 * column being moved or dropped: change the define and the test follows it.
 * The band column was added because the page could not say which band a
 * conversation ran on, so the thing worth pinning is that it is THERE and that
 * it does not eat its neighbours. Hence the hard-coded 12, 17 and 27.
 *
 * Mutation-tested 2026-10-11: deleting the put() in js8_page.c fails this,
 * and so does widening the column to 5. */
static void t_band_column(void)
{
    printf("the band column says which band the conversation ran on\n");
    js8_chat_msg_t m = mk(1791554040, 1188, "HB9BV", "F6HCM HB9BV HEARTBEAT SNR",
                          false, false);
    snprintf(m.band, sizeof(m.band), "40m");
    m.dial_hz = 7078000;
    js8_page_render(&m, 1, NOW, PRIMARY, L, M);

    const int r = JS8_PAGE_ROW0;
    CHECK(at(r, 12, "40m "), "band is not at column 12: '%.5s'", &L[r][12]);
    CHECK(at(r, 17, "HB9BV"), "sender is not at column 17: '%.9s'", &L[r][17]);
    CHECK(at(r, 27, "F6HCM HB9BV HEARTBEAT SNR"),
          "text is not at column 27: '%.25s'", &L[r][27]);
    /* The gap either side, so the band cannot run into age/offset or sender. */
    CHECK(L[r][11] == ' ', "no gap before the band column");
    CHECK(L[r][16] == ' ', "no gap after the band column");
    /* The common structured row must still fit on ONE line after the cut from
     * 42 to 37 - that was the argument for affording the column at all. */
    CHECK(M[r + 1] == -1, "a 25-character message wrapped onto a second row");

    /* ⚠ FOUR WIDE. "160m" is the longest real band string and must not be
     * clipped to "160" - that reads as a different band. */
    js8_chat_msg_t w = mk(1791554040, 1188, "HB9BV", "X", false, false);
    snprintf(w.band, sizeof(w.band), "160m");
    js8_page_render(&w, 1, NOW, PRIMARY, L, M);
    CHECK(at(JS8_PAGE_ROW0, 12, "160m"), "160m was clipped: '%.4s'",
          &L[JS8_PAGE_ROW0][12]);
    CHECK(L[JS8_PAGE_ROW0][16] == ' ', "160m ran into the sender column");

    /* An entry stamped before the dial was known must not read as a band. */
    js8_chat_msg_t u = mk(1791554040, 1188, "HB9BV", "X", false, false);
    js8_page_render(&u, 1, NOW, PRIMARY, L, M);
    CHECK(at(JS8_PAGE_ROW0, 12, JS8_PAGE_UNKNOWN_BAND),
          "an unknown band is blank rather than marked: '%.4s'",
          &L[JS8_PAGE_ROW0][12]);
}

static void t_rows_for_agrees_with_render(void)
{
    printf("js8_page_rows_for agrees with what render draws\n");
    const char *cases[] = {
        "",
        "SHORT",
        "GOOD MORNING OM THANKS FOR THE CALL THE WEATHER HERE IS FINE TODAY",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        js8_chat_msg_t m = mk(0, 1000, "X", cases[i], false, false);
        int used = js8_page_render(&m, 1, NOW, PRIMARY, L, M);
        int want = js8_page_rows_for(&m);
        CHECK(used == want, "case %u: render used %d, rows_for said %d", i, used, want);
    }
}

/* ---- T132: the sub-mode mark, column 1 --------------------------------
 *
 * ⛔ BLANK FOR THE PRIMARY. The column is new and the page is drawn on every
 * JS8 session, almost all of which run one sub-mode; a letter appearing there
 * unexplained would be a regression for every operator who never turned the
 * alternate decode on.
 *
 * ⛔ AND IT MUST NOT COST THE OTHER COLUMNS A CHARACTER. The grid was exactly
 * full when BAND took five columns off the message and the operator noticed.
 * Column 1 was already spare; this checks it still is, by requiring AGE to
 * start where it always did. */
static void t_submode_mark(void)
{
    printf("T132 sub-mode mark (column %d)\n", JS8_PAGE_COL_SUBMODE);

    js8_chat_msg_t m = mk(NOW - 60, 1188, "OZ1LAV", "QSL", false, false);

    /* Heard on the sub-mode we are operating: blank. */
    js8_page_render(&m, 1, NOW, PRIMARY, L, M);
    CHECK(L[JS8_PAGE_ROW0][JS8_PAGE_COL_SUBMODE] == ' ',
          "the primary sub-mode is marked '%c', must be blank",
          L[JS8_PAGE_ROW0][JS8_PAGE_COL_SUBMODE]);
    CHECK(at(JS8_PAGE_ROW0, JS8_PAGE_COL_AGE, " 60s"),
          "the mark column moved AGE: '%.4s'", &L[JS8_PAGE_ROW0][JS8_PAGE_COL_AGE]);

    /* Heard on Fast while we operate Normal. */
    m.proto = (uint8_t)FTX_PROTOCOL_JS8_FAST;
    js8_page_render(&m, 1, NOW, PRIMARY, L, M);
    CHECK(L[JS8_PAGE_ROW0][JS8_PAGE_COL_SUBMODE] == 'F',
          "a Fast message is marked '%c', want 'F'",
          L[JS8_PAGE_ROW0][JS8_PAGE_COL_SUBMODE]);
    CHECK(at(JS8_PAGE_ROW0, JS8_PAGE_COL_AGE, " 60s"),
          "the mark column moved AGE: '%.4s'", &L[JS8_PAGE_ROW0][JS8_PAGE_COL_AGE]);
    CHECK(at(JS8_PAGE_ROW0, JS8_PAGE_COL_SENDER, "OZ1LAV"),
          "the mark column moved CALL: '%.9s'", &L[JS8_PAGE_ROW0][JS8_PAGE_COL_SENDER]);
    CHECK(at(JS8_PAGE_ROW0, JS8_PAGE_COL_TEXT, "QSL"),
          "the mark column moved MESSAGE: '%.3s'", &L[JS8_PAGE_ROW0][JS8_PAGE_COL_TEXT]);

    /* The flag column is NOT the mark column: a live Fast run shows both. */
    m.active = true;
    js8_page_render(&m, 1, NOW, PRIMARY, L, M);
    CHECK(L[JS8_PAGE_ROW0][0] == JS8_PAGE_FLAG_LIVE,
          "the mark overwrote the live flag: '%c'", L[JS8_PAGE_ROW0][0]);
    CHECK(L[JS8_PAGE_ROW0][JS8_PAGE_COL_SUBMODE] == 'F',
          "the live flag overwrote the mark: '%c'",
          L[JS8_PAGE_ROW0][JS8_PAGE_COL_SUBMODE]);

    /* ⛔ ONE MARK PER MESSAGE, not one per wrapped row. A long free-text run
     * takes several rows and a letter repeated down them would read as
     * several separate alternate-sub-mode messages. */
    js8_chat_msg_t w = mk(NOW, 1000, "X",
        "GOOD MORNING OM THANKS FOR THE CALL THE WEATHER HERE IS FINE TODAY",
        false, false);
    w.proto = (uint8_t)FTX_PROTOCOL_JS8_TURBO;
    int used = js8_page_render(&w, 1, NOW, PRIMARY, L, M);
    CHECK(used > 1, "the wrap case did not wrap (used %d)", used);
    CHECK(L[JS8_PAGE_ROW0][JS8_PAGE_COL_SUBMODE] == 'T',
          "first row mark '%c', want 'T'", L[JS8_PAGE_ROW0][JS8_PAGE_COL_SUBMODE]);
    for (int r = JS8_PAGE_ROW0 + 1; r < JS8_PAGE_ROW0 + used; r++)
        CHECK(L[r][JS8_PAGE_COL_SUBMODE] == ' ',
              "continuation row %d is marked '%c'", r,
              L[r][JS8_PAGE_COL_SUBMODE]);
}

int main(void)
{
    t_no_title_row();
    t_every_line_is_exactly_the_grid_width();
    t_columns();
    t_empty_says_why();
    t_wrap_on_a_space();
    t_long_word_keeps_its_tail();
    t_flags();
    t_empty_text_still_takes_a_row();
    t_overflow_stops_at_the_last_row();
    t_row_map_names_the_message_each_row_belongs_to();
    t_band_column();
    t_rows_for_agrees_with_render();
    t_submode_mark();

    printf(fails ? "\nFAIL (%d)\n" : "\nPASS\n", fails);
    return fails ? 1 : 0;
}
