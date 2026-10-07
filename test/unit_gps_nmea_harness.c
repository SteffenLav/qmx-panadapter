// unit_gps_nmea_harness.c - host-side tests for the Unit GPS RMC parser and
// the second-boundary flip rule (tasks 4.2/4.3/4.6 of the change).
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main/unit_gps -o test/unit_gps_nmea_harness.exe test/unit_gps_nmea_harness.c main/unit_gps/nmea_parse.c
//   ./test/unit_gps_nmea_harness.exe
//
// WHY THIS EXISTS
//   These are the decisions a device test cannot make visible. A checksum that
//   accepts garbage, a date field that accepts "31 February", or a flip rule
//   that stamps an edge on a REPEATED second all produce a firmware that looks
//   healthy: the chip goes green and the clock is wrong, which is the exact
//   failure this feature is supposed to fix (a confident wrong date is worse
//   than no date - see Don WB0LQW's two-days-behind POTA log). A satellite fix
//   is needed to exercise any of it on hardware, and a bench cannot set the
//   receiver's date to something impossible.
//
// It links the REAL parser (nmea_parse.c), not a copy of it.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "nmea_parse.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

// A status-A fix with a full date and a fractional second: the case that must
// produce a complete UTC (date included) or the whole feature is pointless.
static void test_valid_fix(void)
{
    nmea_rmc_t r;
    const char *line =
        "$GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0,230326,,*0A";

    CHECK(nmea_parse_rmc(line, &r), "well-formed A sentence rejected");
    CHECK(r.valid, "status A not read as a fix");
    CHECK(r.year == 2026 && r.mon == 3 && r.mday == 23,
          "date = %04d-%02d-%02d, want 2026-03-23", r.year, r.mon, r.mday);
    CHECK(r.hour == 12 && r.min == 35 && r.sec == 48,
          "time = %02d:%02d:%02d, want 12:35:48", r.hour, r.min, r.sec);
    CHECK(r.frac_us == 400000u, "frac = %u us, want 400000", r.frac_us);

    // Same sentence with a CR/LF terminator, as a UART hands it over.
    nmea_rmc_t r2;
    CHECK(nmea_parse_rmc("$GPRMC,123549,A,4807.038,N,01131.000,E,0.0,000.0,230326,,*11\r\n", &r2),
          "sentence with CRLF rejected");
    CHECK(r2.frac_us == 0, "hhmmss without a fraction should be 0 us, got %u", r2.frac_us);
    CHECK(r2.sec == 49, "second = %d, want 49", r2.sec);
}

// A void fix is a well-formed sentence from a receiver that simply has no lock
// yet - it must PARSE (that is what puts the pipeline in DEVICE rather than
// LISTENING) while reporting valid=false (what keeps it out of LOCKED).
static void test_void_fix(void)
{
    nmea_rmc_t r;
    const char *line =
        "$GPRMC,123548.400,V,4807.038,N,01131.000,E,0.0,000.0,230326,,*1D";
    CHECK(nmea_parse_rmc(line, &r), "void sentence rejected");
    CHECK(!r.valid, "status V read as a fix");
    CHECK(r.hour == 12 && r.mday == 23, "void sentence lost its other fields");
}

static void test_rejections(void)
{
    nmea_rmc_t r;

    CHECK(!nmea_parse_rmc("$GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0,230326,,*00", &r),
          "bad checksum accepted");
    CHECK(!nmea_parse_rmc("$GPGGA,123548.400,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*59", &r),
          "non-RMC sentence accepted (RMC only)");
    CHECK(!nmea_parse_rmc("$GPRMC,123548.400,X,4807.038,N,01131.000,E,0.0,000.0,230326,,*13", &r),
          "impossible status column accepted");
    CHECK(!nmea_parse_rmc("$GPRMC,250000.000,A,4807.038,N,01131.000,E,0.0,000.0,230326,,*00", &r),
          "hour 25 accepted");
    CHECK(!nmea_parse_rmc("$GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0,231326,,*0B", &r),
          "month 13 accepted");
    CHECK(!nmea_parse_rmc("$GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0,290225,,*02", &r),
          "29 February 2025 accepted");
    CHECK(!nmea_parse_rmc("$GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0", &r),
          "truncated sentence accepted");
    CHECK(!nmea_parse_rmc("", &r), "empty line accepted");
    CHECK(!nmea_parse_rmc("GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0,230326,,*0A", &r),
          "line without its '$' accepted");
}

// The two-digit-year pivot: 94 is 1994, 26 is 2026. Getting this wrong turns
// a good fix into a 2094 date, which epoch_is_sane() then rejects - and the
// log would read as "no fix" rather than as a date bug.
static void test_year_pivot(void)
{
    nmea_rmc_t r;
    CHECK(nmea_parse_rmc("$GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0,230394,,*03", &r),
          "1994 sentence rejected");
    CHECK(r.year == 1994, "year = %d, want 1994", r.year);
    CHECK(nmea_parse_rmc("$GNRMC,000000.000,A,4807.038,N,01131.000,E,0.0,000.0,010126,,*1B", &r),
          "2026 sentence rejected");
    CHECK(r.year == 2026, "year = %d, want 2026", r.year);
}

// Flip detection: only a forward step of exactly one second is an edge. The
// 59->00 wrap is the one that happens every minute and matters most; a repeat
// or a gap (a lost sentence) must NOT be stamped as a flip, or the clock gets
// phase-aligned to a moment that never happened.
static void test_flip(void)
{
    CHECK(nmea_second_flipped(59, 0), "59 -> 00 not seen as an edge");
    CHECK(nmea_second_flipped(0, 1),  "00 -> 01 not seen as an edge");
    CHECK(nmea_second_flipped(48, 49), "48 -> 49 not seen as an edge");
    CHECK(!nmea_second_flipped(48, 48), "a repeated second treated as an edge");
    CHECK(!nmea_second_flipped(48, 50), "a one-second gap treated as an edge");
    CHECK(!nmea_second_flipped(48, 10), "a jump treated as an edge");
    CHECK(!nmea_second_flipped(0, 59),  "a backwards step treated as an edge");
    CHECK(!nmea_second_flipped(-1, 0),  "the pre-first-sentence state treated as an edge");
    CHECK(!nmea_second_flipped(59, 30), "half the dial treated as an edge");

    // The pair a real midnight boundary produces, parsed from real sentences:
    // the state the firmware carries across, then the sentence that arrives.
    nmea_rmc_t a, b;
    CHECK(nmea_parse_rmc("$GNRMC,235959.000,A,4807.038,N,01131.000,E,0.0,000.0,311225,,*18", &a),
          "23:59:59 sentence rejected");
    CHECK(nmea_parse_rmc("$GNRMC,000000.000,A,4807.038,N,01131.000,E,0.0,000.0,010126,,*1B", &b),
          "00:00:00 sentence rejected");
    CHECK(nmea_second_flipped(a.sec, b.sec), "midnight pair not seen as an edge");
    CHECK(a.year == 2025 && b.year == 2026, "the date must roll with the boundary");
}

static void test_checksum(void)
{
    CHECK(nmea_checksum_ok("$GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0,230326,,*0A"),
          "valid checksum rejected");
    CHECK(nmea_checksum_ok("$GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0,230326,,*0A\r\n"),
          "valid checksum with CRLF rejected");
    CHECK(!nmea_checksum_ok("$GPRMC,123548,A*"), "missing checksum digits accepted");
    CHECK(!nmea_checksum_ok("$GPRMC,123548,A*GG"), "non-hex checksum accepted");
    CHECK(!nmea_checksum_ok("$GPRMC,123548,A"), "sentence with no checksum accepted");
}

// Field-splitting regression, from the AT6668's actual output captured on the
// bench (2026-10-03): an empty course column (",,") plus the NMEA 4.10 mode
// and nav-status tail. strtok_r collapsed the empty field and shifted the date
// onto the mode column, so EVERY real sentence was rejected - the pipeline sat
// in LISTENING with bytes visibly arriving. The canned sentences above all
// carry a non-empty course, which is why the harness never caught it.
static void test_empty_fields_real_device(void)
{
    nmea_rmc_t r;
    const char *fix =
        "$GNRMC,233525.00,A,3910.05632,N,07710.64776,W,0.01,,031026,,,A,V*3D";
    const char *void_line =
        "$GNRMC,233525.00,V,3910.05632,N,07710.64776,W,0.01,,031026,,,A,V*2A";

    CHECK(nmea_parse_rmc(fix, &r), "real device A sentence (empty course) rejected");
    CHECK(r.valid, "device A sentence not read as a fix");
    CHECK(r.year == 2026 && r.mon == 10 && r.mday == 3,
          "date = %04d-%02d-%02d, want 2026-10-03", r.year, r.mon, r.mday);
    CHECK(r.hour == 23 && r.min == 35 && r.sec == 25,
          "time = %02d:%02d:%02d, want 23:35:25", r.hour, r.min, r.sec);
    CHECK(r.frac_us == 0, "frac = %u us, want 0 (.00)", r.frac_us);

    CHECK(nmea_parse_rmc(void_line, &r), "real device V sentence (empty course) rejected");
    CHECK(!r.valid, "device V sentence read as a fix");
    CHECK(r.year == 2026 && r.mday == 3, "void sentence lost its date field");
}

int main(void)
{
    printf("unit_gps NMEA harness\n\n");
    test_valid_fix();
    test_void_fix();
    test_rejections();
    test_year_pivot();
    test_flip();
    test_checksum();
    test_empty_fields_real_device();

    printf("\n%s\n", g_fail ? "FAILURES ABOVE" : "ALL PASS");
    return g_fail ? 1 : 0;
}
