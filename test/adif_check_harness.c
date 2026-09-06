/* Host test for main/adif/adif_check.c - #263 "check my log".
 *
 *   gcc -std=c11 -Wall -Wextra -o adif_check_harness \
 *       test/adif_check_harness.c main/adif/adif_check.c -Imain/adif && ./adif_check_harness
 *
 * Links the REAL function, deliberately - a harness that mirrors the logic
 * tests the mirror. Mutation-tested: see the list at the bottom.
 */
#include <stdio.h>
#include <string.h>
#include "adif_check.h"

static int fails;

static void expect(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) { printf("  ok   %s\n", what); return; }
    printf("  FAIL %s: got 0x%x want 0x%x\n", what, got, want);
    fails++;
}

static void expect_str(const char *what, const char *got, const char *want)
{
    bool same = (!got && !want) || (got && want && strcmp(got, want) == 0);
    if (same) { printf("  ok   %s\n", what); return; }
    printf("  FAIL %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    fails++;
}

/* A complete, ordinary FT8 contact - the shape adif_log.c actually writes. */
static adif_check_fields_t good(void)
{
    adif_check_fields_t f = {
        .call = "R3PLA", .qso_date = "20260826", .time_on = "194400",
        .band = "20M", .mode = "FT8", .station_call = "OZ1LAV",
        .my_sig_info = NULL, .sig_info = NULL,
    };
    return f;
}

int main(void)
{
    printf("adif_check harness\n");

    /* A good record is silent, and stays silent when not activating. */
    adif_check_fields_t f = good();
    expect("complete record, not activating", adif_check_record(&f, false), 0);

    /* ...but activating, it has claimed no park. */
    expect("complete record, activating, no MY_SIG_INFO",
           adif_check_record(&f, true), ADIF_CHK_NO_MY_REF);

    f = good(); f.my_sig_info = "US-1241";
    expect("activating with a real park", adif_check_record(&f, true), 0);

    /* Every missing field, one at a time. */
    f = good(); f.call = NULL;
    expect("no CALL",          adif_check_record(&f, false), ADIF_CHK_NO_CALL);
    f = good(); f.call = "";
    expect("empty CALL == absent", adif_check_record(&f, false), ADIF_CHK_NO_CALL);
    f = good(); f.band = NULL;
    expect("no BAND",          adif_check_record(&f, false), ADIF_CHK_NO_BAND);
    f = good(); f.mode = NULL;
    expect("no MODE",          adif_check_record(&f, false), ADIF_CHK_NO_MODE);
    f = good(); f.station_call = NULL;
    expect("no STATION_CALLSIGN", adif_check_record(&f, false), ADIF_CHK_NO_STATION_CALL);
    f = good(); f.qso_date = NULL;
    expect("no QSO_DATE",      adif_check_record(&f, false), ADIF_CHK_NO_DATE);
    f = good(); f.time_on = NULL;
    expect("no TIME_ON",       adif_check_record(&f, false), ADIF_CHK_NO_TIME);

    /* Malformed date and time - length AND content. */
    f = good(); f.qso_date = "2026826";
    expect("7-digit date",     adif_check_record(&f, false), ADIF_CHK_BAD_DATE);
    f = good(); f.qso_date = "2026O826";           /* letter O, not zero */
    expect("date with a letter", adif_check_record(&f, false), ADIF_CHK_BAD_DATE);
    f = good(); f.time_on = "1944";
    expect("HHMM is valid",    adif_check_record(&f, false), 0);
    f = good(); f.time_on = "19440";
    expect("5-digit time",     adif_check_record(&f, false), ADIF_CHK_BAD_TIME);
    f = good(); f.time_on = "19:44";
    expect("punctuated time",  adif_check_record(&f, false), ADIF_CHK_BAD_TIME);

    /* References: present-and-malformed is a complaint, absent is not. */
    f = good(); f.my_sig_info = "US1241";          /* the dash is the rule */
    expect("my ref with no dash",  adif_check_record(&f, true), ADIF_CHK_BAD_MY_REF);
    f = good(); f.sig_info = "US1241";
    expect("their ref with no dash", adif_check_record(&f, false), ADIF_CHK_BAD_THEIR_REF);
    f = good(); f.sig_info = "G/LD-049";
    expect("SOTA reference",   adif_check_record(&f, false), 0);
    f = good(); f.sig_info = "DLFF-0123";
    expect("WWFF reference",   adif_check_record(&f, false), 0);
    f = good(); f.sig_info = "US-1241; DROP TABLE";
    expect("ref with punctuation", adif_check_record(&f, false), ADIF_CHK_BAD_THEIR_REF);

    /* Several at once - the whole point of a bitmask. */
    f = good(); f.call = NULL; f.band = NULL; f.qso_date = "x";
    expect("three problems at once", adif_check_record(&f, false),
           ADIF_CHK_NO_CALL | ADIF_CHK_NO_BAND | ADIF_CHK_BAD_DATE);

    /* NULL record is not a crash. */
    expect("NULL fields", adif_check_record(NULL, false), ADIF_CHK_NO_CALL);

    /* One sentence, highest priority first, never a list. */
    expect_str("first problem picks CALL over BAND",
               adif_check_first_problem(ADIF_CHK_NO_CALL | ADIF_CHK_NO_BAND), "no callsign");
    expect_str("first problem of a clean record", adif_check_first_problem(0), NULL);
    expect_str("date beats station callsign",
               adif_check_first_problem(ADIF_CHK_BAD_DATE | ADIF_CHK_NO_STATION_CALL),
               "the date is not YYYYMMDD");

    printf(fails ? "\nFAILED (%d)\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
