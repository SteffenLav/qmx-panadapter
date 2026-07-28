// lotw_harness.c — host-side verification of the TQ8 generator (lotw_tq8.c).
//
// Build (from test/):
//   gcc -O2 -Wall -I../main/adif -o lotw_harness.exe lotw_harness.c ../main/adif/lotw_tq8.c
//
// Run (from test/, needs openssl.exe in PATH):
//   ./lotw_harness.exe
//
// What it proves:
//   1. SIGNDATA construction matches the hand-derived expected string for a
//      known station + QSO (trim/order/uppercase rules from tqsllib source).
//   2. The generated TQ8's field lengths are self-consistent: the signature
//      and SIGNDATA are extracted back out of the file purely by the
//      <NAME:len> length bookkeeping, base64-unwrapped, and the RSA-SHA1
//      signature verifies against the certificate via openssl.
//   3. Optional-field paths (no FREQ, no CQZ/ITUZ) don't disturb the record.
//
// Uses a throwaway self-signed RSA key/cert (generated on first run) — no
// real LoTW certificate is required or touched.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lotw_tq8.h"

#define KEY_PEM  "lotw_test_key.pem"
#define CERT_PEM "lotw_test_cert.pem"
#define PUB_PEM  "lotw_test_pub.pem"

static int fail_count = 0;

static void check(int ok, const char *what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fail_count++;
}

static int run(const char *cmd)
{
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "command failed (%d): %s\n", rc, cmd);
    return rc;
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = (size_t)n;
    return buf;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len ? 0 : -1;
}

// Sign callback: shell out to openssl (RSA PKCS#1 v1.5 over SHA-1, exactly
// what tqsllib's EVP_SignInit(EVP_sha1()) does).
static int sign_openssl(void *ctx, const uint8_t *data, size_t len,
                        uint8_t *sig, size_t *sig_len)
{
    (void)ctx;
    if (write_file("hx_signdata.bin", data, len))
        return -1;
    if (run("openssl dgst -sha1 -sign " KEY_PEM " -out hx_sig.bin hx_signdata.bin"))
        return -1;
    size_t n = 0;
    char *s = read_file("hx_sig.bin", &n);
    if (!s || n == 0 || n > *sig_len) { free(s); return -1; }
    memcpy(sig, s, n);
    *sig_len = n;
    free(s);
    return 0;
}

// Extract the value of the idx-th occurrence (1-based) of <name:len[:type]>
// using ONLY the length bookkeeping — validates the lengths themselves.
static char *extract_field(const char *doc, const char *name, int idx, size_t *out_len)
{
    char tag[64];
    snprintf(tag, sizeof tag, "<%s:", name);
    const char *p = doc;
    for (int i = 0; i < idx; i++) {
        p = strstr(p, tag);
        if (!p) return NULL;
        p += strlen(tag);
    }
    long len = strtol(p, (char **)&p, 10);
    if (len <= 0) return NULL;
    if (*p == ':') {            // optional type char
        p++;
        if (*p) p++;
    }
    if (*p != '>') return NULL;
    p++;
    char *out = malloc((size_t)len + 1);
    if (!out) return NULL;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
    if (out_len) *out_len = (size_t)len;
    return out;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, uint8_t *out)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (; *in; in++) {
        int v = b64_val(*in);
        if (v < 0) continue;    // skip newlines and '='
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

int main(void)
{
    // --- one-time key + self-signed cert ---
    FILE *f = fopen(KEY_PEM, "rb");
    if (f) {
        fclose(f);
    } else {
        printf("generating throwaway RSA key + self-signed cert...\n");
        if (run("openssl req -x509 -newkey rsa:2048 -keyout " KEY_PEM
                " -out " CERT_PEM " -days 36500 -nodes -subj /CN=OZ1LAV -sha256 2>nul"))
            return 1;
    }
    if (run("openssl x509 -in " CERT_PEM " -pubkey -noout > " PUB_PEM))
        return 1;

    char *cert_pem = read_file(CERT_PEM, NULL);
    if (!cert_pem) { fprintf(stderr, "no cert\n"); return 1; }

    // --- station + QSOs ---
    lotw_station_t st = {
        .callsign   = "OZ1LAV",
        .dxcc       = "221",
        .gridsquare = "JO65HN",
        .cqz        = "14",
        .ituz       = "18",
        .cert_pem   = cert_pem,
    };
    lotw_qso_t qsos[2] = {
        { .call = "W1AW", .band = "20M", .mode = "FT8",
          .freq = "14.074", .qso_date = "2026-07-14", .qso_time = "12:34:56Z" },
        // no FREQ — exercises the optional-field path
        { .call = "ja1xyz", .band = "40M", .mode = "FT4",
          .freq = "", .qso_date = "2026-07-13", .qso_time = "23:59:59Z" },
    };

    // --- check 1: SIGNDATA construction (hand-derived expectations) ---
    // station part (sigspec order CQZ, GRIDSQUARE, ITUZ) + QSO part
    // (BAND, CALL, [FREQ], MODE, QSO_DATE, QSO_TIME), all uppercased.
    {
        char *sd = lotw_tq8_signdata(&st, &qsos[0]);
        const char *exp = "14JO65HN1820MW1AW14.074FT82026-07-1412:34:56Z";
        check(sd && strcmp(sd, exp) == 0, "SIGNDATA QSO 1 matches hand-derived string");
        if (sd && strcmp(sd, exp)) printf("  got: %s\n  exp: %s\n", sd, exp);
        free(sd);

        char *sd2 = lotw_tq8_signdata(&st, &qsos[1]);
        const char *exp2 = "14JO65HN1840MJA1XYZFT42026-07-1323:59:59Z";
        check(sd2 && strcmp(sd2, exp2) == 0,
              "SIGNDATA QSO 2 (no FREQ, lowercase call uppercased) matches");
        if (sd2 && strcmp(sd2, exp2)) printf("  got: %s\n  exp: %s\n", sd2, exp2);
        free(sd2);

        // Zones omitted -> station part is grid only
        lotw_station_t st2 = st;
        st2.cqz = NULL;
        st2.ituz = "";
        char *sd3 = lotw_tq8_signdata(&st2, &qsos[0]);
        const char *exp3 = "JO65HN20MW1AW14.074FT82026-07-1412:34:56Z";
        check(sd3 && strcmp(sd3, exp3) == 0, "SIGNDATA with zones omitted matches");
        free(sd3);

        // US subdivision: US_COUNTY then US_STATE, both AFTER ITUZ (the sigspec
        // field list is alphabetical). Adding them must not disturb the station
        // part for anyone who leaves them empty - the three checks above are the
        // regression guard for that.
        lotw_station_t st4 = st;
        st4.us_state  = "VA";
        st4.us_county = "Arlington";
        char *sd4 = lotw_tq8_signdata(&st4, &qsos[0]);
        const char *exp4 = "14JO65HN18ARLINGTONVA20MW1AW14.074FT82026-07-1412:34:56Z";
        check(sd4 && strcmp(sd4, exp4) == 0, "SIGNDATA with US_COUNTY/US_STATE matches");
        if (sd4 && strcmp(sd4, exp4)) printf("  got: %s\n  exp: %s\n", sd4, exp4);
        free(sd4);

        // INDEPENDENT VECTOR. The station portion below is taken from CardSat's
        // LOTW_TQ8_FORMAT.md worked example, whose full signed string came from
        // a QSO that LoTW actually ACCEPTED AND POSTED - so this is external
        // ground truth for our station-part field ORDER, not another
        // hand-derivation by the same author who wrote the code.
        // Their station: CQZ 5, GRID FM18LU, ITUZ 8, US_COUNTY Arlington,
        // US_STATE VA -> "5FM18LU8ARLINGTONVA".
        // (Their QSO part additionally exercises BAND_RX/FREQ_RX/PROP_MODE/
        // SAT_NAME, which a QMX cannot produce, so only the station part is
        // comparable - it is also the part this change touches.)
        {
            lotw_station_t n8hm = {
                .callsign = "N8HM", .dxcc = "291", .gridsquare = "FM18LU",
                .cqz = "5", .ituz = "8",
                .us_state = "VA", .us_county = "Arlington",
                .cert_pem = cert_pem,
            };
            lotw_qso_t q = { .call = "X", .band = "B", .mode = "M",
                             .freq = "", .qso_date = "D", .qso_time = "T" };
            char *sd5 = lotw_tq8_signdata(&n8hm, &q);
            const char *pfx = "5FM18LU8ARLINGTONVA";
            check(sd5 && strncmp(sd5, pfx, strlen(pfx)) == 0,
                  "station part matches CardSat's LoTW-accepted vector");
            if (sd5 && strncmp(sd5, pfx, strlen(pfx)))
                printf("  got: %s\n  exp prefix: %s\n", sd5, pfx);
            free(sd5);
        }
    }

    // --- build the TQ8 ---
    char errbuf[128] = "";
    char *tq8 = lotw_tq8_build(&st, qsos, 2, "QMX-Panadapter harness",
                               sign_openssl, NULL, errbuf, sizeof errbuf);
    check(tq8 != NULL, "lotw_tq8_build succeeds");
    if (!tq8) {
        fprintf(stderr, "  error: %s\n", errbuf);
        return 1;
    }
    write_file("lotw_sample_tq8.txt", tq8, strlen(tq8));
    printf("--- generated TQ8 (uncompressed, saved to lotw_sample_tq8.txt) ---\n%s"
           "--- end ---\n", tq8);

    // --- check 2: structural sanity ---
    check(strstr(tq8, "<Rec_Type:5>tCERT") != NULL, "tCERT record present");
    check(strstr(tq8, "<Rec_Type:8>tSTATION") != NULL, "tSTATION record present");
    check(strstr(tq8, "<DXCC:3>221") != NULL, "tSTATION carries DXCC");

    // --- check 3: extract per length-bookkeeping, verify RSA-SHA1 sig ---
    for (int i = 1; i <= 2; i++) {
        size_t sd_len = 0, sig_len = 0;
        char *sd  = extract_field(tq8, "SIGNDATA", i, &sd_len);
        char *sg  = extract_field(tq8, "SIGN_LOTW_V2.0", i, &sig_len);
        char what[80];
        snprintf(what, sizeof what, "QSO %d: SIGNDATA+SIGN extract via field lengths", i);
        check(sd != NULL && sg != NULL, what);
        if (!sd || !sg) { free(sd); free(sg); continue; }

        // base64 value must end with the OpenSSL-BIO trailing newline
        snprintf(what, sizeof what, "QSO %d: base64 sig value ends with newline", i);
        check(sig_len > 0 && sg[sig_len - 1] == '\n', what);

        uint8_t sig[LOTW_SIG_MAX];
        size_t n = b64_decode(sg, sig);
        write_file("hx_sd_x.bin", sd, sd_len);
        write_file("hx_sig_x.bin", sig, n);
        int rc = run("openssl dgst -sha1 -verify " PUB_PEM
                     " -signature hx_sig_x.bin hx_sd_x.bin");
        snprintf(what, sizeof what, "QSO %d: signature verifies (openssl)", i);
        check(rc == 0, what);
        free(sd);
        free(sg);
    }

    free(tq8);
    free(cert_pem);
    printf("\n%s (%d failure%s)\n", fail_count ? "HARNESS FAILED" : "ALL CHECKS PASSED",
           fail_count, fail_count == 1 ? "" : "s");
    return fail_count ? 1 : 0;
}
