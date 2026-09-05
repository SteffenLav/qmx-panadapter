// Host test for cw_decode_parse_tb(). Links the REAL function from
// main/cw_decode.c - the ESP-side ring is behind #ifdef ESP_PLATFORM, so this
// compiles the same file the firmware does rather than a copy that can drift.
//
//   gcc -std=c11 -Wall -Wextra -I main -o cw_decode_harness \
//       test/cw_decode_harness.c main/cw_decode.c && ./cw_decode_harness
//
// The response format is TBtnns; from the QMX CAT manual (present in 1_03 and
// 1_04). What is worth testing here is not the happy path but the ways a
// response can lie about itself: nn disagreeing with the payload, a payload
// containing the terminator, a truncated read. Every one of those, taken at
// face value, would either corrupt the decoded text or swallow the start of
// the next response.

#include "cw_decode.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(const char *what, const char *resp,
                   int want_n, const char *want_text, int want_tx)
{
    char out[CW_DECODE_MAX_CHUNK + 1];
    int tx = -99;
    int n = cw_decode_parse_tb(resp, out, sizeof(out), &tx);

    int ok = (n == want_n);
    if (ok && want_n >= 0) ok = (strcmp(out, want_text) == 0) && (tx == want_tx);

    if (!ok) {
        fails++;
        printf("  FAIL %-34s resp=\"%s\"\n", what, resp);
        printf("       got n=%d text=\"%s\" tx=%d ; want n=%d text=\"%s\" tx=%d\n",
               n, out, tx, want_n, want_text ? want_text : "", want_tx);
    } else {
        printf("  ok   %-34s -> n=%d \"%s\"\n", what, n, n > 0 ? out : "");
    }
}

int main(void)
{
    printf("cw_decode_parse_tb\n");

    /* The ordinary cases. */
    expect("idle, nothing decoded",      "TB000;",              0,  "",        0);
    expect("one character",              "TB001E;",             1,  "E",       0);
    expect("a callsign",                 "TB005DL8UG;",         5,  "DL8UG",   0);
    expect("with spaces",                "TB008CQ CQ DE;",      8,  "CQ CQ DE", 0);
    expect("transmitting, 3 left",       "TB302ES;",            2,  "ES",      3);
    expect("9 = more than 9 remain",     "TB901K;",             1,  "K",       9);

    /* Punctuation the manual lists is legitimate decoded text and must survive
       intact - a filter that "cleaned" it would eat real characters. */
    expect("punctuation is text",        "TB006?.,\"`(;",       6,  "?.,\"`(", 0);
    expect("slash and question",         "TB004G/M?;",          4,  "G/M?",    0);

    /* Malformed: every one of these must be refused, not partially accepted. */
    expect("not a TB response",          "FA00014074000;",     -1,  NULL,      0);
    expect("empty string",               "",                   -1,  NULL,      0);
    expect("truncated before count",     "TB0;",               -1,  NULL,      0);
    expect("count is not numeric",       "TBxyzA;",            -1,  NULL,      0);
    expect("t is not numeric",           "TBx01A;",            -1,  NULL,      0);
    expect("payload shorter than nn",    "TB005AB;",           -1,  NULL,      0);
    expect("no terminator at all",       "TB003ABC",           -1,  NULL,      0);
    expect("payload longer than nn",     "TB002ABC;",          -1,  NULL,      0);
    expect("terminator inside payload",  "TB004AB;C;",         -1,  NULL,      0);
    expect("count beyond the radio max", "TB099AB;",           -1,  NULL,      0);
    /* The two below look redundant against the cases above and are not: each
       isolates ONE check. Mutation testing found that every earlier "malformed"
       case was being rejected by a DIFFERENT rule, so removing the prefix test
       or the count bound changed nothing the harness could see.

       nn=99 WITH 99 real characters - long enough to satisfy the length check,
       so only the upper bound can reject it. */
    expect("oversized count, full payload",
           "TB099"
           "ABCDEFGHIJKLMNOPQRSTUVWXYZ"   /* 26 */
           "ABCDEFGHIJKLMNOPQRSTUVWXYZ"   /* 52 */
           "ABCDEFGHIJKLMNOPQRSTUVWXYZ"   /* 78 */
           "ABCDEFGHIJKLMNOPQRSTU"        /* 99 - must be EXACTLY nn, or the
                                             length check rejects it first and
                                             the bound is never reached */
           ";",                                                   -1,  NULL,   0);
    /* Well-formed in every respect EXCEPT the two-letter code, so only the
       prefix test can reject it. */
    expect("well-formed but not TB",      "XY001A;",            -1,  NULL,     0);

    /* A caller with a small buffer must be truncated safely, never overrun. */
    {
        char small[4];
        int n = cw_decode_parse_tb("TB008ABCDEFGH;", small, sizeof(small), NULL);
        if (n != 3 || strcmp(small, "ABC") != 0) {
            fails++;
            printf("  FAIL small buffer -> n=%d \"%s\" (want 3 \"ABC\")\n", n, small);
        } else {
            printf("  ok   small buffer truncates safely -> \"%s\"\n", small);
        }
    }

    /* ---- noise squelch ------------------------------------------------ */
    printf("\ncw_squelch_push\n");
    {
        struct { const char *what; const char *in; const char *want; } cases[] = {
          /* The reported symptom: a long E/T run with nothing else is noise. */
          { "pure noise run is dropped",   "TTTTTTTTETTTTTTT",      ""            },
          { "noise then real text",        "TTTTTTTTETTTTTTTCQ",    "CQ"          },
          /* Real text keeps its E and T - this is the half that must not break. */
          { "TEST survives",               "TEST K",                "TEST K"      },
          { "a callsign with T and E",     "OZ1LAV DE K",           "OZ1LAV DE K" },
          { "TT inside a word",            "BETTER K",              "BETTER K"    },
          { "short E/T run released",      "ETET K",                "ETET K"      },
          /* Spaced-out noise is still noise - a space must not release a run. */
          { "spaced noise is dropped",     "T T T T T T E T CQ",    "CQ"          },
          { "run resets after real text",  "CQ TTTTTTTT DE K",      "CQ DE K"     },
        };
        for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            cw_squelch_t st;
            memset(&st, 0, sizeof(st));
            char got[128]; size_t g = 0;
            for (const char *q = cases[i].in; *q; q++) {
                char rel[CW_SQUELCH_HOLD + 2];
                int m = cw_squelch_push(&st, *q, rel, sizeof(rel));
                for (int k = 0; k < m && g < sizeof(got)-1; k++) got[g++] = rel[k];
            }
            got[g] = 0;
            if (strcmp(got, cases[i].want) != 0) {
                fails++;
                printf("  FAIL %-30s in=\"%s\"  got \"%s\" want \"%s\"\n",
                       cases[i].what, cases[i].in, got, cases[i].want);
            } else {
                printf("  ok   %-30s -> \"%s\"\n", cases[i].what, got);
            }
        }
    }

    /* ---- speed --------------------------------------------------------- */
    printf("\ncw_wpm_estimate\n");
    {
        struct { const char *what; int chars; unsigned ms; int want; } w[] = {
          /* PARIS: five characters to a word. */
          { "100 chars in 60 s = 20 wpm",  100, 60000, 20 },
          { "50 chars in 60 s = 10 wpm",    50, 60000, 10 },
          { "25 chars in 15 s = 20 wpm",    25, 15000, 20 },
          { "nothing yet",                   0, 60000,  0 },
          { "window too short to judge",    10,   500,  0 },
          { "absurd rate is capped",    1000000,  1000, 99 },
        };
        for (size_t i = 0; i < sizeof(w)/sizeof(w[0]); i++) {
            int got = cw_wpm_estimate(w[i].chars, w[i].ms);
            if (got != w[i].want) {
                fails++;
                printf("  FAIL %-30s got %d want %d\n", w[i].what, got, w[i].want);
            } else {
                printf("  ok   %-30s -> %d\n", w[i].what, got);
            }
        }
    }

    printf(fails ? "\nFAILED (%d)\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
