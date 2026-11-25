// Host harness for main/ft8_rcvd_hold.c - links the REAL functions, not a copy.
//
//   gcc -std=c11 -Wall -Wextra -I../main -o ft8_rcvd_hold.exe
//       ft8_rcvd_hold_harness.c ../main/ft8_rcvd_hold.c
//
// Replays the two QSOs of 2026-11-25 that logged with no RST_RCVD, with the
// slot times taken from the firmware log (uptime ms; the decode lands ~15.4 s
// after its slot starts):
//
//   R1BDX  'OZ1LAV R1BDX RR73' decoded 261398 -> slot 246000
//          our 73 on air in slot 261000, TX idle (hold starts) ~273600
//          'OZ1LAV R1BDX R-12' decoded 291404 -> slot 276000, again 321404 -> 306000
//          WANT: logged once, RST_RCVD -12.
//   EO80F  '<OZ1LAV> EO80F RR73' to our CQ, our 73, then nothing from them.
//          WANT: not logged once the window closes.
//
// Plus the cases the rule must not break: a QSO WITH a report is not held, a
// Field Day exchange is not held, a second held QSO coexists with the first,
// and nothing outside the window or addressed elsewhere completes a hold.

#include "ft8_rcvd_hold.h"

#include <stdio.h>
#include <string.h>

static int fails;
static const char *MY = "OZ1LAV";
#define SLOT_MS 15000

static void check(const char *what, bool ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

/* The same split ft8_qso.c's split_msg3() makes: two tokens with hash
 * brackets removed, then the rest. */
static void split(const char *text, char *to, char *from, char *rest)
{
    char t[64];
    snprintf(t, sizeof(t), "%s", text);
    char *a = strtok(t, " "), *b = strtok(NULL, " "), *c = strtok(NULL, "");
    snprintf(to,   16, "%s", a ? a : "");
    snprintf(from, 16, "%s", b ? b : "");
    snprintf(rest, 20, "%s", c ? c : "");
    for (char *p = to; *p; p++) if (*p == '<' || *p == '>') memmove(p, p + 1, strlen(p)), p--;
    for (char *p = from; *p; p++) if (*p == '<' || *p == '>') memmove(p, p + 1, strlen(p)), p--;
}

/* One decoded slot: feed its messages, then expire. Counts what was logged
 * and what was dropped, as rcvd_hold_scan() does on the device. */
static int  n_logged, n_dropped;
static ft8_rcvd_hold_t last_logged, last_dropped;

static void slot(int64_t slot_ms, const char *const *msgs, int n)
{
    for (int i = 0; i < n; i++) {
        char to[16], from[16], rest[20];
        split(msgs[i], to, from, rest);
        ft8_rcvd_hold_t r;
        if (ft8_rcvd_hold_on_msg(MY, to, from, rest, slot_ms, &r)) {
            n_logged++; last_logged = r;
            printf("    slot %lld: '%s' -> LOG %s rst_rcvd=%s\n",
                   (long long)slot_ms, msgs[i], r.call, r.rst_rcvd);
        }
    }
    ft8_rcvd_hold_t g;
    while (ft8_rcvd_hold_expire(slot_ms, &g)) {
        n_dropped++; last_dropped = g;
        printf("    slot %lld: window closed -> NOT logged %s\n", (long long)slot_ms, g.call);
    }
}

static ft8_rcvd_hold_t rec(const char *call, const char *rst_sent)
{
    ft8_rcvd_hold_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.call, sizeof(r.call), "%s", call);
    snprintf(r.rst_sent, sizeof(r.rst_sent), "%s", rst_sent);
    snprintf(r.mode, sizeof(r.mode), "FT8");
    r.freq_hz = 14074000;
    return r;
}

int main(void)
{
    printf("ft8_rcvd_hold harness (FT8_RCVD_HOLD_SLOTS=%d)\n", FT8_RCVD_HOLD_SLOTS);

    printf("\n1. report parsing\n");
    char o[8];
    check("'R-12' -> -12", ft8_rcvd_hold_parse_report("R-12", o, sizeof o) && !strcmp(o, "-12"));
    check("'R+05' -> +05", ft8_rcvd_hold_parse_report("R+05", o, sizeof o) && !strcmp(o, "+05"));
    check("'-08'  -> -08", ft8_rcvd_hold_parse_report("-08", o, sizeof o) && !strcmp(o, "-08"));
    check("'+5'   -> +5",  ft8_rcvd_hold_parse_report("+5", o, sizeof o) && !strcmp(o, "+5"));
    check("'RR73' rejected", !ft8_rcvd_hold_parse_report("RR73", o, sizeof o));
    check("'RRR'  rejected", !ft8_rcvd_hold_parse_report("RRR", o, sizeof o));
    check("'73'   rejected", !ft8_rcvd_hold_parse_report("73", o, sizeof o));
    check("'KO59' rejected", !ft8_rcvd_hold_parse_report("KO59", o, sizeof o));
    check("'-123' rejected", !ft8_rcvd_hold_parse_report("-123", o, sizeof o));
    check("''     rejected", !ft8_rcvd_hold_parse_report("", o, sizeof o));

    printf("\n2. which finished QSOs are held\n");
    check("no RST_RCVD           -> held", ft8_rcvd_hold_needed("", false));
    check("RST_RCVD -12          -> logged now", !ft8_rcvd_hold_needed("-12", false));
    check("Field Day, exch held  -> logged now", !ft8_rcvd_hold_needed("", true));

    printf("\n3. R1BDX: RR73 with no report, R-12 one slot after our 73\n");
    ft8_rcvd_hold_reset(); n_logged = n_dropped = 0;
    check("R1BDX finish is held", ft8_rcvd_hold_needed("", false));
    ft8_rcvd_hold_t r1 = rec("R1BDX", "-06");
    ft8_rcvd_hold_add(&r1, 273600, SLOT_MS, NULL);
    { const char *m[] = { "CQ DL1ABC JO62", "OZ1LAV R1BDX R-12" }; slot(276000, m, 2); }
    { const char *m[] = { "CQ OZ1LAV JO65" };                      slot(291000, m, 1); }
    { const char *m[] = { "OZ1LAV R1BDX R-12" };                   slot(306000, m, 1); }
    { const char *m[] = { "CQ OZ1LAV JO65" };                      slot(321000, m, 1); }
    { const char *m[] = { "CQ OZ1LAV JO65" };                      slot(336000, m, 1); }
    check("R1BDX logged exactly once", n_logged == 1);
    check("R1BDX RST_RCVD = -12", !strcmp(last_logged.rst_rcvd, "-12"));
    check("R1BDX RST_SENT kept (-06)", !strcmp(last_logged.rst_sent, "-06"));
    check("nothing dropped", n_dropped == 0);
    check("nothing left held", !ft8_rcvd_hold_any());

    printf("\n4. EO80F: unsolicited RR73, never a report\n");
    ft8_rcvd_hold_reset(); n_logged = n_dropped = 0;
    check("EO80F finish is held", ft8_rcvd_hold_needed("", false));
    ft8_rcvd_hold_t r2 = rec("EO80F", "-02");
    ft8_rcvd_hold_add(&r2, 1000000 + 12600, SLOT_MS, NULL);
    { const char *m[] = { "<OZ1LAV> EO80F RR73" }; slot(1015000, m, 1); }
    { const char *m[] = { "CQ OZ1LAV JO65" };      slot(1030000, m, 1); }
    { const char *m[] = { "CQ EO80F KN98" };       slot(1045000, m, 1); }
    { const char *m[] = { "CQ OZ1LAV JO65" };      slot(1060000, m, 1); }
    check("still held inside the window", ft8_rcvd_hold_is_held("EO80F"));
    { const char *m[] = { "CQ EO80F KN98" };       slot(1075000, m, 1); }
    check("EO80F NOT logged", n_logged == 0);
    check("EO80F dropped once the window closed", n_dropped == 1 && !strcmp(last_dropped.call, "EO80F"));
    { const char *m[] = { "OZ1LAV EO80F -10" };    slot(1090000, m, 1); }
    check("a report after the window logs nothing", n_logged == 0);

    printf("\n5. the next QSO during a hold\n");
    ft8_rcvd_hold_reset(); n_logged = n_dropped = 0;
    ft8_rcvd_hold_t a = rec("R1BDX", "-06"), b = rec("SP9XYZ", "-11");
    ft8_rcvd_hold_add(&a, 273600, SLOT_MS, NULL);
    ft8_rcvd_hold_add(&b, 303600, SLOT_MS, NULL);   /* next contact finished 2 slots later */
    check("two QSOs held at once", ft8_rcvd_hold_is_held("R1BDX") && ft8_rcvd_hold_is_held("SP9XYZ"));
    { const char *m[] = { "DL1ABC R1BDX -03", "OZ1LAV SP9XYZ R-07" }; slot(306000, m, 2); }
    check("R1BDX's report to ANOTHER station completes nothing for R1BDX", ft8_rcvd_hold_is_held("R1BDX"));
    check("SP9XYZ logged with -07", n_logged == 1 && !strcmp(last_logged.rst_rcvd, "-07"));
    check("dropping a superseded hold works", ft8_rcvd_hold_drop("R1BDX") && !ft8_rcvd_hold_any());

    printf("\n6. a newer exchange with the same call replaces, never doubles\n");
    ft8_rcvd_hold_reset(); n_logged = n_dropped = 0;
    ft8_rcvd_hold_add(&a, 273600, SLOT_MS, NULL);
    ft8_rcvd_hold_add(&a, 333600, SLOT_MS, NULL);
    { const char *m[] = { "OZ1LAV R1BDX R-12" }; slot(336000, m, 1); }
    { const char *m[] = { "OZ1LAV R1BDX R-12" }; slot(366000, m, 1); }
    check("logged once", n_logged == 1 && n_dropped == 0);

    printf("\n7. FT4 slots (7.5 s): the window is 4 x 7.5 s\n");
    ft8_rcvd_hold_reset(); n_logged = n_dropped = 0;
    ft8_rcvd_hold_add(&a, 100000, 7500, NULL);
    { const char *m[] = { "OZ1LAV R1BDX R-12" }; slot(130000, m, 1); }
    check("report at deadline is outside", n_logged == 0 && n_dropped == 1);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
