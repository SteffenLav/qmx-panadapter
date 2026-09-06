#include "adif_check.h"

#include "esp_log.h"

#include <ctype.h>
#include <string.h>

static bool empty(const char *s) { return !s || !s[0]; }

static bool all_digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

// A programme reference is US-1241 (POTA), G/LD-049 (SOTA) or DLFF-0123 (WWFF).
// The DASH is the discriminator, which is the same rule the log editor enforces
// on a typed reference and the same reason a bare "US1241" is refused there: the
// value goes out as a claim that a specific park was worked, and a missing field
// is honest where a wrong one is not.
static bool looks_like_ref(const char *s)
{
    if (empty(s)) return false;
    if (!strchr(s, '-')) return false;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '/') return false;
    return true;
}

uint32_t adif_check_record(const adif_check_fields_t *f, bool activating)
{
    uint32_t bad = 0;
    if (!f) return ADIF_CHK_NO_CALL;

    if (empty(f->call))         bad |= ADIF_CHK_NO_CALL;
    if (empty(f->band))         bad |= ADIF_CHK_NO_BAND;
    if (empty(f->mode))         bad |= ADIF_CHK_NO_MODE;
    if (empty(f->station_call)) bad |= ADIF_CHK_NO_STATION_CALL;

    if (empty(f->qso_date))                                  bad |= ADIF_CHK_NO_DATE;
    else if (strlen(f->qso_date) != 8 || !all_digits(f->qso_date, 8))
                                                             bad |= ADIF_CHK_BAD_DATE;

    // TIME_ON is HHMM or HHMMSS - ADIF allows both, and this project writes 6.
    if (empty(f->time_on))                                   bad |= ADIF_CHK_NO_TIME;
    else {
        size_t n = strlen(f->time_on);
        if ((n != 4 && n != 6) || !all_digits(f->time_on, n)) bad |= ADIF_CHK_BAD_TIME;
    }

    // A reference is only wrong if it is PRESENT and malformed. Absent is a
    // different complaint, and only when the operator says they are activating.
    if (!empty(f->my_sig_info) && !looks_like_ref(f->my_sig_info)) bad |= ADIF_CHK_BAD_MY_REF;
    if (!empty(f->sig_info)    && !looks_like_ref(f->sig_info))    bad |= ADIF_CHK_BAD_THEIR_REF;
    if (activating && empty(f->my_sig_info))                       bad |= ADIF_CHK_NO_MY_REF;

    return bad;
}

const char *adif_check_first_problem(uint32_t flags)
{
    // Ordered by how badly the record is hurt, not by bit position: a record
    // with no callsign cannot be credited to anyone, a malformed reference
    // credits the wrong place, a missing one credits nowhere.
    if (flags & ADIF_CHK_NO_CALL)         return "no callsign";
    if (flags & ADIF_CHK_NO_DATE)         return "no date";
    if (flags & ADIF_CHK_BAD_DATE)        return "the date is not YYYYMMDD";
    if (flags & ADIF_CHK_NO_TIME)         return "no time";
    if (flags & ADIF_CHK_BAD_TIME)        return "the time is not HHMM or HHMMSS";
    if (flags & ADIF_CHK_NO_STATION_CALL) return "no STATION_CALLSIGN - the log does not say who worked them";
    if (flags & ADIF_CHK_NO_BAND)         return "no band";
    if (flags & ADIF_CHK_NO_MODE)         return "no mode";
    if (flags & ADIF_CHK_BAD_MY_REF)      return "your reference does not look like a park or summit reference";
    if (flags & ADIF_CHK_BAD_THEIR_REF)   return "their reference does not look like a park or summit reference";
    if (flags & ADIF_CHK_NO_MY_REF)       return "no reference on this contact, so it counts towards no activation";
    return NULL;
}

/* ---- on-device self-test (see the header for why it exists) ---------------
 * Deliberately the SAME cases as test/adif_check_harness.c. If one is changed,
 * change both - or better, run the harness once a host compiler exists here and
 * delete this. */
static const char *SELFTEST_TAG = "adif_chk";

int adif_check_selftest(void)
{
    int bad = 0;
    #define CASE(desc, expr, want) do {                                                uint32_t got_ = (expr);                                                        if (got_ != (uint32_t)(want)) {                                                    ESP_LOGE(SELFTEST_TAG, "FAIL %s: got 0x%lx want 0x%lx", desc,                            (unsigned long)got_, (unsigned long)(want));                          bad++;                                                                     }                                                                          } while (0)

    adif_check_fields_t g = { .call = "R3PLA", .qso_date = "20260826",
                              .time_on = "194400", .band = "20M", .mode = "FT8",
                              .station_call = "OZ1LAV" };
    adif_check_fields_t f;

    CASE("complete, not activating", adif_check_record(&g, false), 0);
    CASE("complete, activating",     adif_check_record(&g, true),  ADIF_CHK_NO_MY_REF);

    f = g; f.my_sig_info = "US-1241";
    CASE("activating with a park",   adif_check_record(&f, true), 0);
    f = g; f.call = NULL;   CASE("no CALL",  adif_check_record(&f, false), ADIF_CHK_NO_CALL);
    f = g; f.call = "";     CASE("empty CALL", adif_check_record(&f, false), ADIF_CHK_NO_CALL);
    f = g; f.band = NULL;   CASE("no BAND",  adif_check_record(&f, false), ADIF_CHK_NO_BAND);
    f = g; f.mode = NULL;   CASE("no MODE",  adif_check_record(&f, false), ADIF_CHK_NO_MODE);
    f = g; f.station_call = NULL;
    CASE("no STATION_CALLSIGN", adif_check_record(&f, false), ADIF_CHK_NO_STATION_CALL);
    f = g; f.qso_date = "2026826";
    CASE("7-digit date",     adif_check_record(&f, false), ADIF_CHK_BAD_DATE);
    f = g; f.qso_date = "2026O826";
    CASE("date with a letter", adif_check_record(&f, false), ADIF_CHK_BAD_DATE);
    f = g; f.time_on = "1944";
    CASE("HHMM is valid",    adif_check_record(&f, false), 0);
    f = g; f.time_on = "19:44";
    CASE("punctuated time",  adif_check_record(&f, false), ADIF_CHK_BAD_TIME);
    f = g; f.my_sig_info = "US1241";
    CASE("my ref, no dash",  adif_check_record(&f, true),  ADIF_CHK_BAD_MY_REF);
    f = g; f.sig_info = "G/LD-049";
    CASE("SOTA reference",   adif_check_record(&f, false), 0);
    f = g; f.sig_info = "DLFF-0123";
    CASE("WWFF reference",   adif_check_record(&f, false), 0);
    f = g; f.call = NULL; f.band = NULL; f.qso_date = "x";
    CASE("three at once",    adif_check_record(&f, false),
         ADIF_CHK_NO_CALL | ADIF_CHK_NO_BAND | ADIF_CHK_BAD_DATE);
    CASE("NULL record",      adif_check_record(NULL, false), ADIF_CHK_NO_CALL);

    const char *p = adif_check_first_problem(ADIF_CHK_NO_CALL | ADIF_CHK_NO_BAND);
    if (!p || strcmp(p, "no callsign") != 0) {
        ESP_LOGE(SELFTEST_TAG, "FAIL first_problem priority: '%s'", p ? p : "(null)");
        bad++;
    }
    if (adif_check_first_problem(0) != NULL) {
        ESP_LOGE(SELFTEST_TAG, "FAIL first_problem(0) should be NULL");
        bad++;
    }
    #undef CASE

    ESP_LOGW(SELFTEST_TAG, "adif_check self-test: %s (%d failure(s))",
             bad ? "FAILED" : "all passed", bad);
    return bad;
}
