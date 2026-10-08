#include "nmea_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NMEA sentences are capped at 82 characters including framing (the standard's
 * own limit), so a fixed buffer rejects anything that is not one rather than
 * letting a longer line overflow or silently truncate mid-field. */
#define NMEA_MAX_LINE 120

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool nmea_checksum_ok(const char *line)
{
    if (!line || line[0] != '$') return false;

    const char *star = strchr(line, '*');
    if (!star || !star[1] || !star[2]) return false;

    int hi = hexval(star[1]);
    int lo = hexval(star[2]);
    if (hi < 0 || lo < 0) return false;   // the two checksum digits must be hex

    unsigned sum = 0;
    for (const char *p = line + 1; p < star; p++) sum ^= (unsigned char)*p;

    if (sum != (unsigned)((hi << 4) | lo)) return false;

    // Nothing but CR/LF after the checksum: a sentence carrying a second '*'
    // or trailing junk is a mangled line, not a sentence.
    const char *tail = star + 3;
    while (*tail == '\r' || *tail == '\n') tail++;
    return *tail == '\0';
}

// hhmmss[.f] -> h, m, s, frac_us. Returns false on a wrong digit count, a
// non-numeric field, minutes/seconds out of range, or a fraction that is not
// decimal seconds.
static bool parse_hms(const char *f, int *h, int *m, int *s, uint32_t *frac_us)
{
    size_t whole = strcspn(f, ".");
    if (whole != 6) return false;
    for (size_t i = 0; i < 6; i++) {
        if (f[i] < '0' || f[i] > '9') return false;
    }
    *h = (f[0] - '0') * 10 + (f[1] - '0');
    *m = (f[2] - '0') * 10 + (f[3] - '0');
    *s = (f[4] - '0') * 10 + (f[5] - '0');
    if (*h > 23 || *m > 59 || *s > 59) return false;

    *frac_us = 0;
    if (f[whole] == '.') {
        /* The digits after the point are a FRACTION of a second, not a number:
         * ".400" is four tenths, so strtod's 400.0 would put the clock 400
         * seconds out. Build the integer and divide by 10^ndigits - .4 -> 4/10,
         * .400 -> 400/1000, .078125 -> 78125/1000000. */
        const char *digits = f + whole + 1;
        uint64_t v = 0;
        int ndig = 0;
        while (digits[ndig] >= '0' && digits[ndig] <= '9' && ndig < 9) {
            v = v * 10ULL + (uint64_t)(digits[ndig] - '0');
            ndig++;
        }
        if (ndig == 0) return false;            // "12:00:01." with nothing after it
        for (int i = ndig; digits[i] != '\0'; i++) {
            if (digits[i] < '0' || digits[i] > '9') return false;  // "01.4x"
        }
        uint64_t denom = 1;
        for (int i = 0; i < ndig; i++) denom *= 10ULL;
        *frac_us = (uint32_t)((v * 1000000ULL) / denom);
    }
    return true;
}

// ddmmyy -> year, mon, mday. Rejects impossible calendar dates outright: a
// 31st of February parses as a valid-looking sentence and then silently
// becomes the wrong day once mktime normalises it.
static bool parse_date(const char *f, int *year, int *mon, int *mday)
{
    if (strlen(f) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (f[i] < '0' || f[i] > '9') return false;
    }
    int d = (f[0] - '0') * 10 + (f[1] - '0');
    int mo = (f[2] - '0') * 10 + (f[3] - '0');
    int yy = (f[4] - '0') * 10 + (f[5] - '0');
    if (mo < 1 || mo > 12) return false;
    if (d < 1 || d > 31) return false;

    /* RMC's year is two digits with no century. The conventional pivot is 80:
     * 00-79 -> 2000-2079, 80-99 -> 1980-1999. Every receiver this project will
     * ever see reports the CURRENT year (26 -> 2026), and the pivot keeps a
     * canned 1994 test sentence - or a receiver whose RTC was reset - from
     * silently becoming 2094, which is past epoch_is_sane() and would look
     * like a bad fix rather than a date. */
    int year_full = (yy >= 80) ? 1900 + yy : 2000 + yy;
    static const int mdays[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dim = mdays[mo];
    if (mo == 2 && ((year_full % 4 == 0 && year_full % 100 != 0) || year_full % 400 == 0)) {
        dim = 29;
    }
    if (d > dim) return false;

    *year = year_full;
    *mon  = mo;
    *mday = d;
    return true;
}

#define NMEA_MAX_FIELDS 24

/* Split on commas WITHOUT collapsing empties. strtok_r treats a run of
 * delimiters as one, so a sentence with an empty course (",,") - which a real
 * receiver emits constantly while stationary - shifts every later field and
 * lands the date slot on the status/mode column. The AT6668's own output
 * ($GNRMC,...,0.01,,031026,,,A,V) is exactly that case and was rejected
 * wholesale on hardware until this walk replaced strtok.
 *
 * Returns the number of fields written. Modifies buf in place. */
static int split_fields(char *buf, char **fld, int max)
{
    int   nf = 0;
    char *p  = buf;
    while (nf < max) {
        fld[nf++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    return nf;
}

/* Copy a line into buf, stripped of CR/LF, after the checks every parser here
 * shares. Returns false when the line is not a complete NMEA sentence. */
static bool prep_line(const char *line, char *buf, size_t buflen)
{
    if (!line) return false;
    if (strlen(line) >= NMEA_MAX_LINE) return false;
    if (!nmea_checksum_ok(line)) return false;
    snprintf(buf, buflen, "%s", line);
    buf[strcspn(buf, "\r\n")] = '\0';
    /* Cut the "*HH" checksum off. It is not data, and leaving it on welds it
     * to the LAST FIELD: "$GPGSV,3,3,11,22,42,067,42*48" splits with a final
     * field of "42*48", which then fails every numeric check.
     *
     * RMC never showed this because the field it reads last - the date, index
     * 9 - always has mode/nav-status columns after it. GSV and GGA end on a
     * field they actually use, so both were rejected wholesale until this line
     * existed. Found 2026-10-08 by dumping the split, not by reading it. */
    buf[strcspn(buf, "*")] = '\0';
    return true;
}

/* Is this a "$ttXXX" sentence body of type `type` (three letters)? */
static bool body_is(const char *body, const char *type)
{
    if (!body || body[0] != '$') return false;
    size_t n = strlen(body);
    if (n < 6) return false;                      /* $ + 2 talker + 3 type */
    if (body[1] == '\0' || body[2] == '\0') return false;
    return strcmp(body + n - 3, type) == 0;
}

/* ddmm.mmmm + hemisphere -> signed degrees.
 *
 * NMEA packs DEGREES AND MINUTES into one number: 5540.1234 is 55 deg
 * 40.1234 min, not 5540.1234 degrees. Reading it as a plain float puts a
 * Danish station in the South Atlantic. deg_digits is 2 for latitude, 3 for
 * longitude. */
static bool parse_latlon(const char *f, const char *hemi, int deg_digits, double *out)
{
    if (!f || !hemi) return false;
    if (f[0] == '\0' || hemi[0] == '\0' || hemi[1] != '\0') return false;

    size_t whole = strcspn(f, ".");
    if (whole != (size_t)(deg_digits + 2)) return false;
    for (size_t i = 0; f[i]; i++) {
        if (f[i] == '.') continue;
        if (f[i] < '0' || f[i] > '9') return false;
    }

    double deg = 0;
    for (int i = 0; i < deg_digits; i++) deg = deg * 10 + (f[i] - '0');
    double minutes = strtod(f + deg_digits, NULL);
    if (minutes < 0 || minutes >= 60.0) return false;

    double v = deg + minutes / 60.0;
    char   h = hemi[0];
    if (deg_digits == 2) {
        if (h != 'N' && h != 'S') return false;
        if (v > 90.0) return false;
        if (h == 'S') v = -v;
    } else {
        if (h != 'E' && h != 'W') return false;
        if (v > 180.0) return false;
        if (h == 'W') v = -v;
    }
    *out = v;
    return true;
}

/* A field that must be a non-negative integer, or empty. Returns -1 for an
 * empty field and -2 for a malformed one, so a caller can tell "the receiver
 * did not say" from "this is not the sentence I think it is". */
static int opt_int(const char *f)
{
    if (!f || f[0] == '\0') return -1;
    for (int i = 0; f[i]; i++) {
        if (f[i] < '0' || f[i] > '9') return -2;
    }
    return atoi(f);
}

bool nmea_parse_rmc(const char *line, nmea_rmc_t *out)
{
    if (!out) return false;

    char buf[NMEA_MAX_LINE];
    if (!prep_line(line, buf, sizeof(buf))) return false;

    char *fld[NMEA_MAX_FIELDS];
    int   nf = split_fields(buf, fld, NMEA_MAX_FIELDS);

    /* Talker is two bytes ("GP", "GN", "GL", ...) and the type is exactly RMC.
     * GGA and GSV have their own parsers below. They are for the STATUS
     * display and never touch the clock: a GGA carries a time of day with NO
     * DATE, and letting that near time_sync is how a receiver silently sets
     * the wrong day. */
    if (!body_is(fld[0], "RMC")) return false;

    // RMC field indices: 1=time, 2=status, ..., 9=date (0 is the body).
    // Requires all fields through the date; trailing mode/nav-status columns
    // (NMEA 4.10+) are ignored rather than counted against the sentence.
    if (nf < 10) return false;
    char *time_f  = fld[1];
    char *status  = fld[2];
    char *date_f  = fld[9];
    if (time_f[0] == '\0' || status[0] == '\0' || date_f[0] == '\0') return false;

    nmea_rmc_t r = { 0 };
    if (!parse_hms(time_f, &r.hour, &r.min, &r.sec, &r.frac_us)) return false;
    if (!parse_date(date_f, &r.year, &r.mon, &r.mday)) return false;

    r.valid = (status[0] == 'A' && status[1] == '\0');
    // 'V' (void) is a well-formed sentence and stays valid=false. Anything
    // else in the status column means we are not reading what we think we are.
    if (!r.valid && !(status[0] == 'V' && status[1] == '\0')) return false;

    /* Position is OPTIONAL, and its absence is not an error: a receiver with
     * no fix sends RMC with the lat/lon columns empty, and that sentence is
     * exactly what the DEVICE state is built on. A MALFORMED position is also
     * left as "no position" rather than failing the sentence - the clock path
     * does not read position, and it must not lose a good time over it. */
    if (nf >= 7) {
        double lat, lon;
        if (parse_latlon(fld[3], fld[4], 2, &lat) &&
            parse_latlon(fld[5], fld[6], 3, &lon)) {
            r.has_pos = true;
            r.lat_deg = lat;
            r.lon_deg = lon;
        }
    }

    *out = r;
    return true;
}

bool nmea_second_flipped(int prev_sec, int cur_sec)
{
    if (prev_sec < 0 || prev_sec > 59 || cur_sec < 0 || cur_sec > 59) return false;
    if (prev_sec == 59) return cur_sec == 0;   // the boundary that matters most
    return cur_sec == prev_sec + 1;
}


bool nmea_parse_gga(const char *line, nmea_gga_t *out)
{
    if (!out) return false;

    char buf[NMEA_MAX_LINE];
    if (!prep_line(line, buf, sizeof(buf))) return false;

    char *fld[NMEA_MAX_FIELDS];
    int   nf = split_fields(buf, fld, NMEA_MAX_FIELDS);
    if (!body_is(fld[0], "GGA")) return false;

    /* GGA: 1=time 2=lat 3=N/S 4=lon 5=E/W 6=quality 7=sats 8=HDOP
     *      9=altitude 10=units ... */
    if (nf < 9) return false;

    nmea_gga_t g = { 0 };
    g.hdop  = -1.0f;
    g.alt_m = 0.0f;

    uint32_t frac;
    if (!parse_hms(fld[1], &g.hour, &g.min, &g.sec, &frac)) return false;

    /* Quality 0 is a SUCCESSFUL parse. "The receiver has nothing" is the
     * single most useful thing a status window can say, and rejecting the
     * sentence would make it indistinguishable from a receiver that is
     * not talking at all. */
    int q = opt_int(fld[6]);
    if (q < 0 || q > 8) return false;
    g.quality = q;

    int used = opt_int(fld[7]);
    if (used < -1 || used > 64) return false;
    g.sats_used = (used < 0) ? 0 : used;

    if (fld[8][0] != '\0') {
        g.hdop = (float)strtod(fld[8], NULL);
        if (g.hdop < 0.0f || g.hdop > 100.0f) g.hdop = -1.0f;
    }
    if (nf >= 10 && fld[9][0] != '\0') {
        g.alt_m   = (float)strtod(fld[9], NULL);
        g.has_alt = true;
    }

    *out = g;
    return true;
}

bool nmea_parse_gsv(const char *line, nmea_gsv_t *out)
{
    if (!out) return false;

    char buf[NMEA_MAX_LINE];
    if (!prep_line(line, buf, sizeof(buf))) return false;

    char *fld[NMEA_MAX_FIELDS];
    int   nf = split_fields(buf, fld, NMEA_MAX_FIELDS);
    if (!body_is(fld[0], "GSV")) return false;

    /* GSV: 1=total msgs 2=msg number 3=sats in view, then up to four groups
     * of (PRN, elevation, azimuth, SNR). NMEA 4.10 appends a signal-ID field
     * after the last group; it is ignored, not counted. */
    if (nf < 4) return false;

    nmea_gsv_t v = { 0 };
    v.talker[0] = fld[0][1];
    v.talker[1] = fld[0][2];
    v.talker[2] = '\0';

    v.msg_total = opt_int(fld[1]);
    v.msg_num   = opt_int(fld[2]);
    v.in_view   = opt_int(fld[3]);
    if (v.msg_total < 1 || v.msg_num < 1 || v.msg_num > v.msg_total) return false;
    if (v.in_view < 0 || v.in_view > 64) return false;

    for (int i = 0; i < NMEA_GSV_SATS_PER_MSG; i++) {
        int base = 4 + i * 4;
        if (base >= nf) break;                /* no room for even a PRN */

        int prn = opt_int(fld[base]);
        if (prn == -2) return false;          /* not a number: wrong sentence */
        if (prn < 0) break;                   /* blank PRN ends the list */

        int el  = (base + 1 < nf) ? opt_int(fld[base + 1]) : -1;
        int az  = (base + 2 < nf) ? opt_int(fld[base + 2]) : -1;
        int snr = (base + 3 < nf) ? opt_int(fld[base + 3]) : -1;
        if (el == -2 || az == -2 || snr == -2) return false;
        if (el > 90 || az > 360 || snr > 99) return false;

        v.sat[v.n_sats].prn      = prn;
        v.sat[v.n_sats].elev_deg = el;
        v.sat[v.n_sats].azim_deg = az;
        /* A blank SNR means the satellite is in view but NOT TRACKED. That is
         * a real and useful state - a sky full of untracked satellites is what
         * a bad antenna looks like - so it is kept as -1, not folded to 0. */
        v.sat[v.n_sats].snr_db   = snr;
        v.n_sats++;
    }

    *out = v;
    return true;
}


bool nmea_parse_gsa(const char *line, nmea_gsa_t *out)
{
    if (!out) return false;

    char buf[NMEA_MAX_LINE];
    if (!prep_line(line, buf, sizeof(buf))) return false;

    char *fld[NMEA_MAX_FIELDS];
    int   nf = split_fields(buf, fld, NMEA_MAX_FIELDS);
    if (!body_is(fld[0], "GSA")) return false;

    /* GSA: 1=mode (M/A), 2=fix type, 3..14=PRNs used, 15=PDOP, 16=HDOP,
     * 17=VDOP. NMEA 4.10 appends a system ID; ignored. */
    if (nf < 3) return false;

    nmea_gsa_t g = { 0 };
    g.pdop = g.hdop = g.vdop = -1.0f;

    int ft = opt_int(fld[2]);
    if (ft < 1 || ft > 3) return false;   /* blank or out of range: not usable */
    g.fix_type = ft;

    for (int i = 3; i <= 14 && i < nf; i++) {
        if (fld[i][0] == '\0') continue;    /* an unused slot, not an error */
        int prn = opt_int(fld[i]);
        if (prn == -2) return false;
        if (prn > 0 && g.n_used < 12) g.used[g.n_used++] = prn;
    }

    if (nf > 15 && fld[15][0] != '\0') g.pdop = (float)strtod(fld[15], NULL);
    if (nf > 16 && fld[16][0] != '\0') g.hdop = (float)strtod(fld[16], NULL);
    if (nf > 17 && fld[17][0] != '\0') g.vdop = (float)strtod(fld[17], NULL);

    *out = g;
    return true;
}
