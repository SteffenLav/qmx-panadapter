// Host harness for R030 RTTY exchange capture. Links the REAL main/rtty_qso.c.
//
//   gcc -std=c11 -O2 -Wall -Wextra -I main -o rtty_qso_harness test/rtty_qso_harness.c main/rtty_qso.c
//   ./rtty_qso_harness                  # fixed cases, exit 0 = pass
//   ./rtty_qso_harness --lines f.txt    # print the parse of every line in f
//
// The cases are decoded text from bench1/bench2 during the WAE RTTY contest,
// 2026-11-14/15 ("rtty: rx:" lines in the serial captures), unedited. The
// expected exchange is what a person reads in the line.

#include "rtty_qso.h"
#include <stdio.h>
#include <string.h>

typedef struct { const char *text, *mycall, *exch; int to_me; } tcase_t;

static const tcase_t C[] = {
    /* plain run exchanges */
    { "ZS DLUALM  DL7ALM 599 219 219 DL7ALM YE)$)7ALM TU DE UA3LID QRZ?", "DL7ALM", "219", 1 },
    { "DXZAOX IV3UWU  IV3UWA 599 221 221 IV3UWA EP", "IV3UWA", "221", 1 },
    /* a pair after our call beats a later pair to someone else */
    { "IP2T IP2TXMLRLAZ IP2T  IP2T 599 225 225 IP2T WDXZUA3LID 599 159 159 DE IP2TQVHL", "UA3LID", "159", 1 },
    { "OM5M OM5M CQ ARPOZVOM5M DE RY2A RY2ARTRRY2A  599 1198 1198 R N 599 075 075 RYJW", "RY2A", "1198", 1 },
    /* lost FIGS shift on the report: TNN = 5NN */
    { "EAWF1,TNN 963 963 EA4GOY AAW", "", "963", 0 },
    /* '-' separators */
    { "II8N 599 789-789 II8N.##II8N 599 789-789 II8NHWH", "", "789", 0 },
    /* damaged first copy, the repeat is used */
    { "OH9VD  599 7)U 1207 JMOM5M", "", "1207", 0 },
    /* noise letter glued to the RST */
    { "YM DL3YNXK/$DF5LM H599 1203 1203 HYZDFTU OM5M QRZ", "", "1203", 0 },
    /* a whole number in letter shift */
    { "K1ABC 599 QWE QWE", "", "123", 0 },
    /* first copy cut short, first one still taken */
    { "EA4GOY  EA4GOY 599 226 26 EAWF1", "", "226", 0 },
    /* ragchew report, no exchange */
    { "OZ1LAV DE K1ABC TNX FER CALL UR RST 599 599 NAME BOB", "OZ1LAV", NULL, 0 },
    /* TU after the report is not an exchange */
    { "599 TU DE OM5M", "", NULL, 0 },
    /* state after zone: first token taken */
    { "OZ1LAV 599 05 MA 05 MA", "OZ1LAV", "05", 1 },
    { "nothing here CQ CQ TEST", "", NULL, 0 },
};

static int run_cases(void)
{
    int fail = 0, n = (int)(sizeof C / sizeof C[0]);
    for (int i = 0; i < n; i++) {
        char rst[8], ex[16];
        bool me;
        bool ok = rtty_qso_parse(C[i].text, C[i].mycall, rst, sizeof rst, ex, sizeof ex, &me);
        bool pass = C[i].exch ? (ok && !strcmp(ex, C[i].exch) && !strcmp(rst, "599") &&
                                 (int)me == C[i].to_me)
                              : !ok;
        printf("%s  [%s] -> %s%s%s\n", pass ? "PASS" : "FAIL", C[i].text,
               ok ? ex : "(none)", ok && me ? " to-me" : "", C[i].exch ? "" : "  (want none)");
        if (!pass) fail++;
    }
    /* the stateful path: feed in pieces, reset clears */
    rtty_qso_reset();
    rtty_qso_feed("OZ1LAV 5", 8);
    rtty_qso_feed("99 042 0", 8);
    rtty_qso_feed("42 K1ABC", 8);
    char rst[8], ex[16];
    bool me;
    bool ok = rtty_qso_get("OZ1LAV", rst, sizeof rst, ex, sizeof ex, &me);
    bool pass = ok && !strcmp(ex, "042") && me;
    printf("%s  feed in pieces -> %s\n", pass ? "PASS" : "FAIL", ok ? ex : "(none)");
    if (!pass) fail++;
    rtty_qso_reset();
    pass = !rtty_qso_get("OZ1LAV", rst, sizeof rst, ex, sizeof ex, &me);
    printf("%s  reset clears\n", pass ? "PASS" : "FAIL");
    if (!pass) fail++;
    /* overflow keeps the newest */
    for (int i = 0; i < 100; i++) rtty_qso_feed("XXXXXXXXX ", 10);
    rtty_qso_feed("599 777 777", 11);
    ok = rtty_qso_get("", rst, sizeof rst, ex, sizeof ex, &me);
    pass = ok && !strcmp(ex, "777");
    printf("%s  overflow keeps the newest -> %s\n", pass ? "PASS" : "FAIL", ok ? ex : "(none)");
    if (!pass) fail++;
    /* tap-to-fill: one word */
    static const struct { const char *w; rtty_word_t k; const char *out; } W[] = {
        { "599", RTTY_WORD_RST, "599" }, { "TOO", RTTY_WORD_RST, "599" }, { "5NN", RTTY_WORD_RST, "599" },
        { "1340", RTTY_WORD_EXCH, "1340" }, { "QWE", RTTY_WORD_EXCH, "123" }, { "MA", RTTY_WORD_EXCH, "MA" },
        { "TU", RTTY_WORD_NONE, "" }, { "7)U", RTTY_WORD_NONE, "" }, { "DKILR", RTTY_WORD_NONE, "" },
    };
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++) {
        char o[16];
        rtty_word_t k = rtty_qso_word(W[i].w, o, sizeof o);
        bool p = k == W[i].k && !strcmp(o, W[i].out);
        printf("%s  word '%s' -> %d '%s'\n", p ? "PASS" : "FAIL", W[i].w, (int)k, o);
        if (!p) fail++;
    }
    printf("%d failed\n", fail);
    return fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--lines")) {
        FILE *f = fopen(argv[2], "r");
        if (!f) { perror(argv[2]); return 2; }
        char ln[1024];
        int hits = 0, lines = 0;
        while (fgets(ln, sizeof ln, f)) {
            lines++;
            char rst[8], ex[16];
            bool me;
            if (rtty_qso_parse(ln, "", rst, sizeof rst, ex, sizeof ex, &me)) {
                hits++;
                ln[strcspn(ln, "\r\n")] = 0;
                printf("%-8s | %s\n", ex, ln);
            }
        }
        fclose(f);
        fprintf(stderr, "%d of %d lines gave an exchange\n", hits, lines);
        return 0;
    }
    return run_cases();
}
