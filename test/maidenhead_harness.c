/* Host test for maidenhead_to_latlon() - grid square -> centre lat/lon.
 *
 * Build (from the repo root):
 *   gcc -I main/util -o maidenhead_harness test/maidenhead_harness.c \
 *       main/util/maidenhead.c -lm && ./maidenhead_harness
 *
 * Why this exists: the parser rejected every 8-character grid ("IO51uu43"), so
 * a spot-map reporter publishing one had no position and no distance - found by
 * Uwe DL8UG from EI4HQ's live PSK Reporter report (2026-09-11). The same
 * function feeds TWELVE callers: FT8 decode-list distances, PSK Reporter, the
 * band-plan REGION guess, WSPR spots, the web page. So the 4- and 6-character
 * answers must not move by a hair while the 8-character case is added, and
 * every case is pinned here to hand-derived values.
 *
 * It links the REAL function, not a copy. A harness that mirrors the code under
 * test only ever proves the mirror.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "maidenhead.h"

static int g_fail = 0;

static void expect_ok(const char *grid, double want_lat, double want_lon)
{
    double lat = 1e9, lon = 1e9;
    bool ok = maidenhead_to_latlon(grid, &lat, &lon);
    bool good = ok && fabs(lat - want_lat) < 1e-6 && fabs(lon - want_lon) < 1e-6;
    printf("%s  %-10s -> %s lat %.7f lon %.7f  (want %.7f %.7f)\n",
           good ? "PASS" : "FAIL", grid, ok ? "ok " : "REJ", lat, lon, want_lat, want_lon);
    if (!good) g_fail++;
}

static void expect_rej(const char *label, const char *grid)
{
    double lat = 0, lon = 0;
    bool ok = maidenhead_to_latlon(grid, &lat, &lon);
    printf("%s  %-10s -> %s (want rejected)\n", ok ? "FAIL" : "PASS",
           label, ok ? "ACCEPTED" : "rejected");
    if (ok) g_fail++;
}

/* An 8-char centre must fall inside its own 6-char subsquare, and a 6-char
 * centre inside its 4-char square - the finer grid only ever narrows. */
static void expect_nested(const char *fine, const char *coarse,
                          double half_lat, double half_lon)
{
    double la1, lo1, la2, lo2;
    bool ok = maidenhead_to_latlon(fine, &la1, &lo1) &&
              maidenhead_to_latlon(coarse, &la2, &lo2) &&
              fabs(la1 - la2) < half_lat && fabs(lo1 - lo2) < half_lon;
    printf("%s  %-10s inside %s\n", ok ? "PASS" : "FAIL", fine, coarse);
    if (!ok) g_fail++;
}

int main(void)
{
    /* 4 chars: centre of the 2 x 1 degree square. JO65 = lon 12..14, lat 55..56. */
    expect_ok("JO65",     55.5,        13.0);
    expect_ok("jo65",     55.5,        13.0);            /* case-insensitive */
    expect_ok("AA00",    -89.5,      -179.0);            /* the south-west corner */
    expect_ok("RR99",     89.5,       179.0);            /* the north-east corner */

    /* 6 chars: centre of the 5' x 2.5' subsquare. */
    expect_ok("JO65ab",   55.0 + 1.0/24 + 1.0/48,   12.0 + 1.0/24);
    expect_ok("JO65AB",   55.0 + 1.0/24 + 1.0/48,   12.0 + 1.0/24);
    expect_ok("RR99xx",   89.0 + 23.0/24 + 1.0/48, 178.0 + 23.0/12 + 1.0/24);

    /* 8 chars: centre of the 30" x 15" extended square. EI4HQ's shape.
     * IO51 = lon -10..-8, lat 51..52; uu = +20 subsquares; 43 = +4/+3 tenths. */
    expect_ok("IO51uu43", 51.0 + 20.0/24 + 3.0/240 + 1.0/480,
                          -10.0 + 20.0/12 + 4.0/120 + 1.0/240);
    expect_ok("IO51UU43", 51.0 + 20.0/24 + 3.0/240 + 1.0/480,
                          -10.0 + 20.0/12 + 4.0/120 + 1.0/240);
    expect_ok("JO65ab00", 55.0 + 1.0/24 + 1.0/480,  12.0 + 1.0/12 * 0 + 1.0/240);
    expect_ok("JO65ab99", 55.0 + 1.0/24 + 9.0/240 + 1.0/480,
                          12.0 + 9.0/120 + 1.0/240);

    expect_nested("IO51uu43", "IO51uu", 1.0/48, 1.0/24);
    expect_nested("JO65ab99", "JO65ab", 1.0/48, 1.0/24);
    expect_nested("JO65ab",   "JO65",   0.5,    1.0);

    /* Rejections - every length that is not 4/6/8, and every bad character. */
    expect_rej("len 3",      "JO6");
    expect_rej("len 5",      "JO65a");
    expect_rej("len 7",      "JO65ab4");
    expect_rej("len 10",     "JO65ab43xx");
    expect_rej("field S",    "SO65");                    /* fields stop at R */
    expect_rej("digit pos",  "JOx5");
    expect_rej("subsq y",    "JO65ya");                  /* subsquares stop at X */
    expect_rej("ext letter", "JO65ab4x");                /* extended pair is DIGITS */
    expect_rej("ext space",  "JO65ab4 ");
    expect_rej("empty",      "");
    expect_rej("NULL",       NULL);

    printf("\n%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
