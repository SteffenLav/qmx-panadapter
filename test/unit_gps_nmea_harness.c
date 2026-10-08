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


/* ---------------------------------------------------------------------------
 * Position, GGA and GSV - added 2026-10-08 for the GPS status window.
 *
 * None of this feeds the clock. It exists so an operator can see WHY a
 * receiver is not locking, which on 2026-10-08 cost an afternoon with no
 * instrument at all. The traps below are the ones that produce a plausible
 * wrong answer rather than a visible failure.
 * ------------------------------------------------------------------------ */

/* NMEA packs DEGREES AND MINUTES into one number: 4807.038 is 48 deg
 * 7.038 min = 48.1173, NOT 4807.038 degrees and NOT 48.07038. Reading it as a
 * plain decimal is the classic bug and it puts the station in the wrong
 * country while every other field still looks right. */
static void test_rmc_position(void)
{
    nmea_rmc_t r;
    const char *line =
        "$GPRMC,123548.400,A,4807.038,N,01131.000,E,0.0,000.0,230326,,*0A";
    CHECK(nmea_parse_rmc(line, &r), "RMC with position should parse\n");
    CHECK(r.has_pos, "position should be present\n");
    CHECK(r.lat_deg > 48.1172 && r.lat_deg < 48.1174,
          "lat %.6f, expected ~48.1173 (48 deg 7.038 min)\n", r.lat_deg);
    CHECK(r.lon_deg > 11.5166 && r.lon_deg < 11.5168,
          "lon %.6f, expected ~11.51667 (11 deg 31.000 min)\n", r.lon_deg);
}

// S and W must come back NEGATIVE. A sign error is invisible in the numbers.
static void test_rmc_position_south_west(void)
{
    nmea_rmc_t r;
    const char *line =
        "$GPRMC,123548.00,A,3345.6789,S,15112.3456,W,0.0,000.0,230326,,*30";
    CHECK(nmea_parse_rmc(line, &r), "S/W position should parse\n");
    CHECK(r.has_pos, "S/W position should be present\n");
    CHECK(r.lat_deg < -33.76 && r.lat_deg > -33.77,
          "lat %.6f, expected ~-33.7613\n", r.lat_deg);
    CHECK(r.lon_deg < -151.20 && r.lon_deg > -151.21,
          "lon %.6f, expected ~-151.2058\n", r.lon_deg);
}

/* A receiver with no fix sends RMC with the position columns EMPTY. That
 * sentence must still parse - it is what the DEVICE state is built on - and
 * it must report has_pos false rather than a position of 0,0, which is a real
 * place in the Atlantic. */
static void test_rmc_no_position(void)
{
    nmea_rmc_t r;
    const char *line = "$GNRMC,101112.00,V,,,,,0.01,,031026,,,A,V*0D";
    CHECK(nmea_parse_rmc(line, &r), "void RMC with empty position should parse\n");
    CHECK(!r.valid, "status V should be valid=false\n");
    CHECK(!r.has_pos, "empty position must NOT report has_pos\n");
}

static void test_gga(void)
{
    nmea_gga_t g;
    const char *line =
        "$GPGGA,123548.400,4807.038,N,01131.000,E,1,09,0.94,545.4,M,46.9,M,,*6C";
    CHECK(nmea_parse_gga(line, &g), "GGA should parse\n");
    CHECK(g.quality == 1, "quality %d, expected 1\n", g.quality);
    CHECK(g.sats_used == 9, "sats_used %d, expected 9\n", g.sats_used);
    CHECK(g.hdop > 0.93f && g.hdop < 0.95f, "hdop %.2f, expected 0.94\n", (double)g.hdop);
    CHECK(g.has_alt && g.alt_m > 545.0f && g.alt_m < 546.0f,
          "alt %.1f, expected 545.4\n", (double)g.alt_m);
    CHECK(g.hour == 12 && g.min == 35 && g.sec == 48, "GGA time wrong\n");
}

/* Quality 0 is a SUCCESSFUL parse. "The receiver has nothing" is the single
 * most useful thing a status window can say, and rejecting the sentence makes
 * it indistinguishable from a receiver that is not talking at all - which is
 * precisely the ambiguity this window exists to remove. */
static void test_gga_no_fix(void)
{
    nmea_gga_t g;
    const char *line = "$GPGGA,123548.400,,,,,0,00,,,M,,M,,*75";
    CHECK(nmea_parse_gga(line, &g), "GGA with no fix should still parse\n");
    CHECK(g.quality == 0, "quality %d, expected 0\n", g.quality);
    CHECK(g.sats_used == 0, "sats_used %d, expected 0\n", g.sats_used);
    CHECK(g.hdop < 0.0f, "absent HDOP should be negative, got %.2f\n", (double)g.hdop);
    CHECK(!g.has_alt, "absent altitude must not be reported\n");
}

static void test_gsv(void)
{
    nmea_gsv_t v;
    const char *line =
        "$GPGSV,3,1,11,03,03,111,00,04,15,270,17,06,01,010,,13,06,292,00*72";
    CHECK(nmea_parse_gsv(line, &v), "GSV should parse\n");
    CHECK(strcmp(v.talker, "GP") == 0, "talker '%s', expected GP\n", v.talker);
    CHECK(v.msg_num == 1 && v.msg_total == 3, "msg %d/%d, expected 1/3\n",
          v.msg_num, v.msg_total);
    CHECK(v.in_view == 11, "in_view %d, expected 11\n", v.in_view);
    CHECK(v.n_sats == 4, "n_sats %d, expected 4\n", v.n_sats);
    CHECK(v.sat[0].prn == 3 && v.sat[0].snr_db == 0,
          "sat0 prn %d snr %d, expected 3/0\n", v.sat[0].prn, v.sat[0].snr_db);
    /* A BLANK SNR means in view but not tracked. Folding it to 0 would hide a
     * sky full of untracked satellites, which is what a bad antenna or a
     * missing ground plane looks like. */
    CHECK(v.sat[2].prn == 6 && v.sat[2].snr_db == -1,
          "blank SNR must stay -1, got prn %d snr %d\n",
          v.sat[2].prn, v.sat[2].snr_db);
}

// A short last sentence in a burst carries fewer than four satellites.
static void test_gsv_short_last(void)
{
    nmea_gsv_t v;
    const char *line = "$GPGSV,3,3,11,22,42,067,42*48";
    CHECK(nmea_parse_gsv(line, &v), "short final GSV should parse\n");
    CHECK(v.n_sats == 1, "n_sats %d, expected 1\n", v.n_sats);
    CHECK(v.sat[0].prn == 22 && v.sat[0].snr_db == 42, "sat0 wrong\n");
}

/* in_view is PER TALKER. A multi-constellation receiver sends one burst per
 * constellation, each with its own in_view, so a total needs the LATEST of
 * each - never a running sum over arriving sentences, which counts the same
 * constellation once per sentence in its burst. */
static void test_gsv_per_talker(void)
{
    nmea_gsv_t v;
    const char *line = "$GLGSV,2,1,05,65,28,180,33,66,45,090,,,,,,,,,*6B";
    CHECK(nmea_parse_gsv(line, &v), "GLONASS GSV should parse\n");
    CHECK(strcmp(v.talker, "GL") == 0, "talker '%s', expected GL\n", v.talker);
    CHECK(v.in_view == 5, "in_view %d, expected 5 (this talker only)\n", v.in_view);
}

static void test_sentence_type_isolation(void)
{
    nmea_rmc_t r;
    nmea_gga_t g;
    nmea_gsv_t v;
    const char *gga = "$GPGGA,123548.400,,,,,0,00,,,M,,M,,*75";
    const char *rmc = "$GNRMC,101112.00,V,,,,,0.01,,031026,,,A,V*0D";

    // Each parser takes ONLY its own sentence. A GGA reaching the RMC path
    // would hand time_sync a time of day with no date.
    CHECK(!nmea_parse_rmc(gga, &r), "RMC parser must reject a GGA\n");
    CHECK(!nmea_parse_gga(rmc, &g), "GGA parser must reject an RMC\n");
    CHECK(!nmea_parse_gsv(rmc, &v), "GSV parser must reject an RMC\n");
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
    test_rmc_position();
    test_rmc_position_south_west();
    test_rmc_no_position();
    test_gga();
    test_gga_no_fix();
    test_gsv();
    test_gsv_short_last();
    test_gsv_per_talker();
    test_sentence_type_isolation();

    printf("\n%s\n", g_fail ? "FAILURES ABOVE" : "ALL PASS");
    return g_fail ? 1 : 0;
}
