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

bool nmea_parse_rmc(const char *line, nmea_rmc_t *out)
{
    if (!line || !out) return false;
    if (strlen(line) >= NMEA_MAX_LINE) return false;
    if (!nmea_checksum_ok(line)) return false;

    char buf[NMEA_MAX_LINE];
    snprintf(buf, sizeof(buf), "%s", line);
    buf[strcspn(buf, "\r\n")] = '\0';

    /* Split on commas WITHOUT collapsing empties. strtok_r treats a run of
     * delimiters as one, so a sentence with an empty course (",,") - which a
     * real receiver emits constantly while stationary - shifts every later
     * field and lands the date slot on the status/mode column. The AT6668's
     * own output ($GNRMC,...,0.01,,031026,,,A,V) is exactly that case and was
     * rejected wholesale on hardware until this walk replaced strtok. */
    char *fld[16];
    int   nf = 0;
    char *p  = buf;
    while (nf < (int)(sizeof(fld) / sizeof(fld[0]))) {
        fld[nf++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }

    char *body = fld[0];
    if (!body || body[0] != '$') return false;

    // Talker is two bytes ("GP", "GN", "GL", ...) and the type is exactly RMC.
    // GGA/GSV/GST are deliberately NOT parsed: they would carry position and
    // satellite counts this project has no use for, and accepting more
    // sentence types is more surface than "RMC only".
    size_t body_len = strlen(body);
    if (body_len < 6) return false;
    if (strcmp(body + body_len - 3, "RMC") != 0) return false;
    if (body[1] == '\0' || body[2] == '\0') return false;

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

    *out = r;
    return true;
}

bool nmea_second_flipped(int prev_sec, int cur_sec)
{
    if (prev_sec < 0 || prev_sec > 59 || cur_sec < 0 || cur_sec > 59) return false;
    if (prev_sec == 59) return cur_sec == 0;   // the boundary that matters most
    return cur_sec == prev_sec + 1;
}
