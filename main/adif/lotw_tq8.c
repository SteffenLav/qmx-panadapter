// lotw_tq8.c — portable TQ8 (LoTW digitally-signed log) generator.
// See lotw_tq8.h for provenance. No ESP-IDF dependencies (host-testable).

#include "lotw_tq8.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Allocation override so the firmware build can route the (potentially
// multi-KB) buffers to PSRAM. Plain free() works on heap_caps allocations
// in ESP-IDF, so callers always free with free().
#ifndef LOTW_TQ8_MALLOC
#define LOTW_TQ8_MALLOC(sz)     malloc(sz)
#define LOTW_TQ8_REALLOC(p, sz) realloc((p), (sz))
#endif

// ---------------------------------------------------------------- utils --

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    oom;
} sb_t;

static int sb_init(sb_t *sb, size_t cap)
{
    sb->buf = (char *)LOTW_TQ8_MALLOC(cap);
    sb->len = 0;
    sb->cap = cap;
    sb->oom = (sb->buf == NULL);
    if (!sb->oom) sb->buf[0] = '\0';
    return sb->oom ? -1 : 0;
}

static void sb_append_n(sb_t *sb, const char *s, size_t n)
{
    if (sb->oom) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap * 2;
        while (ncap < sb->len + n + 1) ncap *= 2;
        char *nb = (char *)LOTW_TQ8_REALLOC(sb->buf, ncap);
        if (!nb) {
            sb->oom = 1;
            return;
        }
        sb->buf = nb;
        sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append(sb_t *sb, const char *s)
{
    sb_append_n(sb, s, strlen(s));
}

// See lotw_tq8.h: an ADIF mode/submode pair collapsed to the LoTW mode.
// Deliberately narrow - MFSK is the only ADIF mode this firmware can produce
// that is not itself a LoTW mode, so this handles exactly that and returns
// everything else untouched. A general ADIF-to-LoTW mode table belongs in
// TQSL's configuration data, which is updated by ARRL and not by us; guessing
// at more of it here would be inventing a mapping rather than following one.
const char *lotw_mode_from_adif(const char *mode, const char *submode)
{
    // Own case-insensitive compare rather than strcasecmp: this file is
    // deliberately dependency-free so test/lotw_harness.c can compile it on any
    // host, and strcasecmp lives in <strings.h>, which not every host has.
    static const char mfsk[] = "MFSK";
    if (!mode) return "";
    if (submode && submode[0]) {
        size_t i = 0;
        for (; mfsk[i] && mode[i]; i++)
            if (toupper((unsigned char)mode[i]) != mfsk[i]) break;
        if (!mfsk[i] && !mode[i]) return submode;
    }
    return mode;
}

// <NAME:len>value  or  <NAME:len:type>value — len is the byte length of
// value (newlines included), exactly like tqsllib's tqsl_adifMakeField.
static void sb_field(sb_t *sb, const char *name, char type, const char *value)
{
    char hdr[64];
    size_t vlen = strlen(value);
    if (type)
        snprintf(hdr, sizeof hdr, "<%s:%u:%c>", name, (unsigned)vlen, type);
    else
        snprintf(hdr, sizeof hdr, "<%s:%u>", name, (unsigned)vlen);
    sb_append(sb, hdr);
    sb_append_n(sb, value, vlen);
}

// Field followed by newline (the normal record-body case).
static void sb_field_nl(sb_t *sb, const char *name, const char *value)
{
    sb_field(sb, name, 0, value);
    sb_append(sb, "\n");
}

// Append value with leading/trailing ASCII whitespace trimmed
// (tqsllib trim()s every value that enters the sign data).
static void sb_append_trimmed(sb_t *sb, const char *s)
{
    if (!s) return;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n > 0) sb_append_n(sb, s, n);
}

static int is_empty(const char *s)
{
    if (!s) return 1;
    while (*s) {
        if (!isspace((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

// Base64 with OpenSSL BIO_f_base64 semantics: newline after every 64
// output characters and after the final partial line (tqsl_encodeBase64
// uses the line-wrapping BIO; the newlines are part of the field value
// and are counted in its length). Returns malloc'd NUL-terminated string.
static char *b64_wrap(const uint8_t *data, size_t n)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t groups = (n + 2) / 3;
    size_t outmax = groups * 4 + groups * 4 / 64 + 4;
    char *out = (char *)LOTW_TQ8_MALLOC(outmax);
    if (!out) return NULL;
    size_t o = 0, line = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        int rem = (int)(n - i);
        if (rem > 1) v |= (uint32_t)data[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)data[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = (rem > 1) ? tab[(v >> 6) & 63] : '=';
        out[o++] = (rem > 2) ? tab[v & 63] : '=';
        line += 4;
        if (line == 64) {
            out[o++] = '\n';
            line = 0;
        }
    }
    if (line != 0)
        out[o++] = '\n';
    out[o] = '\0';
    return out;
}

// ------------------------------------------------------------- signdata --

// Station part of the sign data: the SIGN_LOTW_V2.0 sigspec's tSTATION
// fields we support, in sigspec order, empty fields skipped. CALL/DXCC are in
// the tSTATION *record* but are NOT part of the sign data (they're absent from
// the sigspec field list).
//
// The sigspec order is ALPHABETICAL over the full field set:
//   AU_STATE, CA_PROVINCE, CA_US_PARK, CN_PROVINCE, CQZ, DX_US_PARK, FI_KUNTA,
//   GRIDSQUARE, IOTA, ITUZ, JA_CITY_GUN_KU, JA_PREFECTURE, RU_OBLAST,
//   US_COUNTY, US_PARK, US_STATE
// so the ones we support fall out as CQZ, GRIDSQUARE, ITUZ, US_COUNTY,
// US_STATE. US_COUNTY/US_STATE append AFTER ITUZ, which is why adding them
// cannot change the signature of any existing non-US station.
//
// Order matters absolutely: LoTW re-derives this string and verifies against
// it, and a mismatch is SILENT - the file is accepted and queued, then the QSO
// is dropped during processing with no error reported.
static void signdata_station(sb_t *sb, const lotw_station_t *st)
{
    sb_append_trimmed(sb, st->cqz);
    sb_append_trimmed(sb, st->gridsquare);
    sb_append_trimmed(sb, st->ituz);
    sb_append_trimmed(sb, st->us_county);
    sb_append_trimmed(sb, st->us_state);
}

// QSO part, sigspec order: BAND [BAND_RX] CALL [FREQ] [FREQ_RX] MODE
// [PROP_MODE] QSO_DATE QSO_TIME [SAT_NAME]. We never fill the bracketed
// ones except FREQ (no split/satellite operation on a QMX).
static void signdata_qso(sb_t *sb, const lotw_qso_t *q)
{
    sb_append_trimmed(sb, q->band);
    sb_append_trimmed(sb, q->call);
    sb_append_trimmed(sb, q->freq);
    sb_append_trimmed(sb, q->mode);
    sb_append_trimmed(sb, q->qso_date);
    sb_append_trimmed(sb, q->qso_time);
}

char *lotw_tq8_signdata(const lotw_station_t *st, const lotw_qso_t *qso)
{
    sb_t sb;
    if (sb_init(&sb, 256))
        return NULL;
    signdata_station(&sb, st);
    signdata_qso(&sb, qso);
    if (sb.oom) {
        free(sb.buf);
        return NULL;
    }
    // tqsllib uppercases the whole assembled string before signing and
    // writes the uppercased form into the SIGNDATA field.
    for (size_t i = 0; i < sb.len; i++)
        sb.buf[i] = (char)toupper((unsigned char)sb.buf[i]);
    return sb.buf;
}

// ------------------------------------------------------------- sections --

static int err_out(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf && errlen)
        snprintf(errbuf, errlen, "%s", msg);
    return -1;
}

// Extract the base64 body of the certificate PEM (newlines kept, '\r'
// stripped, trailing newline guaranteed) — tqsl_getGABBItCERT ships the
// PEM body verbatim minus the BEGIN/END armor lines.
static char *cert_pem_body(const char *pem)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----";
    static const char end[]   = "-----END CERTIFICATE-----";
    const char *p = strstr(pem, begin);
    if (!p) return NULL;
    p += sizeof(begin) - 1;
    while (*p == '\r' || *p == '\n') p++;
    const char *e = strstr(p, end);
    if (!e || e <= p) return NULL;
    sb_t sb;
    if (sb_init(&sb, (size_t)(e - p) + 2))
        return NULL;
    for (const char *c = p; c < e; c++) {
        if (*c != '\r')
            sb_append_n(&sb, c, 1);
    }
    if (sb.len == 0 || sb.buf[sb.len - 1] != '\n')
        sb_append(&sb, "\n");
    if (sb.oom) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

char *lotw_tq8_build(const lotw_station_t *st,
                     const lotw_qso_t *qsos, int n_qsos,
                     const char *app_ident,
                     lotw_sign_fn sign, void *sign_ctx,
                     char *errbuf, size_t errlen)
{
    if (!st || !qsos || n_qsos <= 0 || !sign) {
        err_out(errbuf, errlen, "bad arguments");
        return NULL;
    }
    if (is_empty(st->callsign) || is_empty(st->dxcc)) {
        err_out(errbuf, errlen, "station callsign/DXCC missing");
        return NULL;
    }
    if (!st->cert_pem || !strstr(st->cert_pem, "BEGIN CERTIFICATE")) {
        err_out(errbuf, errlen, "certificate PEM missing/invalid");
        return NULL;
    }

    char *certbody = cert_pem_body(st->cert_pem);
    if (!certbody) {
        err_out(errbuf, errlen, "could not parse certificate PEM body");
        return NULL;
    }

    sb_t out;
    if (sb_init(&out, 8192)) {
        free(certbody);
        err_out(errbuf, errlen, "out of memory");
        return NULL;
    }

    // --- TQSL_IDENT header ---
    {
        char ident[192];
        snprintf(ident, sizeof ident, "%s Lib: V2.5 Config: V11.26 AllowDupes: false",
                 app_ident ? app_ident : "Unknown");
        sb_field(&out, "TQSL_IDENT", 0, ident);
        sb_append(&out, "\n");
    }

    // --- tCERT ---
    sb_field_nl(&out, "Rec_Type", "tCERT");
    sb_field_nl(&out, "CERT_UID", "1");
    sb_field(&out, "CERTIFICATE", 0, certbody);  // body ends with '\n' itself
    sb_append(&out, "<eor>\n");
    free(certbody);

    // --- tSTATION ---
    sb_field_nl(&out, "Rec_Type", "tSTATION");
    sb_field_nl(&out, "STATION_UID", "1");
    sb_field_nl(&out, "CERT_UID", "1");
    sb_field_nl(&out, "CALL", st->callsign);
    sb_field_nl(&out, "DXCC", st->dxcc);
    if (!is_empty(st->gridsquare)) sb_field_nl(&out, "GRIDSQUARE", st->gridsquare);
    if (!is_empty(st->cqz))        sb_field_nl(&out, "CQZ", st->cqz);
    if (!is_empty(st->ituz))       sb_field_nl(&out, "ITUZ", st->ituz);
    // TrustedQSL internal field names, NOT the ADIF STATE/CNTY - see lotw_tq8.h.
    if (!is_empty(st->us_state))   sb_field_nl(&out, "US_STATE", st->us_state);
    if (!is_empty(st->us_county))  sb_field_nl(&out, "US_COUNTY", st->us_county);
    sb_append(&out, "<eor>\n");

    // --- tCONTACT per QSO ---
    for (int i = 0; i < n_qsos; i++) {
        const lotw_qso_t *q = &qsos[i];
        if (is_empty(q->call) || is_empty(q->band) || is_empty(q->mode) ||
            is_empty(q->qso_date) || is_empty(q->qso_time)) {
            err_out(errbuf, errlen, "QSO missing required field");
            goto fail;
        }

        char *sd = lotw_tq8_signdata(st, q);
        if (!sd) {
            err_out(errbuf, errlen, "out of memory (signdata)");
            goto fail;
        }
        uint8_t sig[LOTW_SIG_MAX];
        size_t siglen = sizeof sig;
        if (sign(sign_ctx, (const uint8_t *)sd, strlen(sd), sig, &siglen) != 0) {
            free(sd);
            err_out(errbuf, errlen, "signing failed");
            goto fail;
        }
        char *b64 = b64_wrap(sig, siglen);
        if (!b64) {
            free(sd);
            err_out(errbuf, errlen, "out of memory (base64)");
            goto fail;
        }

        // Field order matches tqsl_getGABBItCONTACTData exactly. Note: no
        // '\n' between the SIGN field and SIGNDATA — the wrapped base64
        // value already ends with a newline (counted in its length).
        sb_field_nl(&out, "Rec_Type", "tCONTACT");
        sb_field_nl(&out, "STATION_UID", "1");
        sb_field_nl(&out, "CALL", q->call);
        sb_field_nl(&out, "BAND", q->band);
        sb_field_nl(&out, "MODE", q->mode);
        if (!is_empty(q->freq)) sb_field_nl(&out, "FREQ", q->freq);
        sb_field_nl(&out, "QSO_DATE", q->qso_date);
        sb_field_nl(&out, "QSO_TIME", q->qso_time);
        sb_field(&out, "SIGN_LOTW_V2.0", '6', b64);
        sb_field(&out, "SIGNDATA", 0, sd);
        sb_append(&out, "\n<eor>\n");
        free(b64);
        free(sd);
    }

    if (out.oom) {
        err_out(errbuf, errlen, "out of memory");
        goto fail;
    }
    return out.buf;

fail:
    free(out.buf);
    return NULL;
}
