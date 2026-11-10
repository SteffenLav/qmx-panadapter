// log_free_text_harness.c - the web log window's free-text cleaner, on the host.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main -o test/log_free_text_harness.exe test/log_free_text_harness.c
//   ./test/log_free_text_harness.exe
//
// WHY: the text goes into the ADIF log, one record per line, and five readers
// find fields by searching the line. A newline, a "<CALL:" in a remark or half
// a UTF-8 character would each corrupt a record without any error.
#include <stdio.h>
#include <string.h>
#include "util/log_free_text.h"

static int fails;
static void want(const char *in, size_t cap, const char *expect, const char *what)
{
    char out[64];
    memset(out, 'X', sizeof out);
    log_free_text(out, cap, in);
    int ok = strcmp(out, expect) == 0 && strlen(out) < cap;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { printf("       got \"%s\" want \"%s\"\n", out, expect); fails++; }
}

int main(void)
{
    want("  Gyula  ", 25, "Gyula", "trims both ends");
    want("", 25, "", "empty stays empty");
    want(NULL, 25, "", "NULL is empty");
    want("a\r\nb\tc", 25, "abc", "control characters dropped");
    want("see <CALL:5>W1AW>", 25, "see  CALL:5 W1AW", "angle brackets become spaces");
    want("ABCDEFGHIJ", 6, "ABCDE", "capped at cap-1 bytes");
    // "Győr": G y C5 91 r - a cap that lands inside the 2-byte letter
    want("Gy\xC5\x91r", 4, "Gy", "does not end in half a 2-byte character");
    want("Gy\xC5\x91r", 5, "Gy\xC5\x91", "keeps a whole 2-byte character");
    want("Gy\xC5\x91r", 25, "Gy\xC5\x91r", "UTF-8 passes through");
    // 3-byte (E2 82 AC) and 4-byte (F0 9F 98 80) characters cut at every point
    want("a\xE2\x82\xAC", 3, "a", "3-byte character cut after 1 byte");
    want("a\xE2\x82\xAC", 4, "a", "3-byte character cut after 2 bytes");
    want("a\xE2\x82\xAC", 5, "a\xE2\x82\xAC", "3-byte character whole");
    want("a\xF0\x9F\x98\x80", 5, "a", "4-byte character cut after 3 bytes");
    want("a\xF0\x9F\x98\x80", 6, "a\xF0\x9F\x98\x80", "4-byte character whole");
    want("ab   ", 4, "ab", "trailing spaces after the cap go");
    want("x", 1, "", "a 1-byte buffer holds nothing");
    printf(fails ? "\nlog_free_text_harness: %d FAILED\n" : "\nlog_free_text_harness: all checks passed\n", fails);
    return fails ? 1 : 0;
}
