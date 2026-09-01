// See lotw_upload.h. TQ8 construction itself lives in lotw_tq8.c (portable,
// host-verified by test/lotw_harness.c); this file is the ESP-side glue:
// cert/key storage on SPIFFS, mbedtls RSA-SHA1 signing, gzip container,
// multipart POST, and the batch/cursor logic.

#include "lotw_upload.h"
#include "lotw_tq8.h"
#include "adif_log.h"
#include "settings.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_app_desc.h"

#include "mbedtls/pk.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>   // fsync
#include <sys/stat.h>

static const char *TAG = "lotw_upload";

#define LOTW_UPLOAD_URL "https://lotw.arrl.org/lotw/upload"
#define CERT_PATH "/spiffs/lotw_cert.b64"
#define KEY_PATH  "/spiffs/lotw_key.b64"

// QSOs per TQ8/POST. Bounds the PSRAM working set (text + per-record sign
// time) without bothering LoTW - it accepts arbitrarily large logs anyway.
#define LOTW_BATCH_MAX 50

static void *psram_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// ---- cert/key storage ----------------------------------------------------

static esp_err_t store_line_file(const char *path, const char *line)
{
    if (!line || !line[0]) {
        unlink(path);
        return ESP_OK;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for write", path);
        return ESP_FAIL;
    }
    size_t len = strlen(line);
    bool ok = fwrite(line, 1, len, f) == len;
    // fsync is mandatory for anything worth keeping on SPIFFS - fflush alone
    // leaves the bytes in FS buffers if power is cut (see CLAUDE.md).
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return ok ? ESP_OK : ESP_FAIL;
}

// Reads the whole file as one NUL-terminated PSRAM string, stripping any
// whitespace/newlines (the stored form is single-line base64).
static char *read_line_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *buf = psram_alloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    size_t o = 0;
    for (size_t i = 0; i < rd; i++) {
        char c = buf[i];
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
            buf[o++] = c;
    }
    buf[o] = '\0';
    if (o == 0) { free(buf); return NULL; }
    return buf;
}

esp_err_t lotw_store_cert_b64(const char *cert_b64) { return store_line_file(CERT_PATH, cert_b64); }
esp_err_t lotw_store_key_b64(const char *key_b64)   { return store_line_file(KEY_PATH,  key_b64); }
char *lotw_read_cert_b64(void) { return read_line_file(CERT_PATH); }
char *lotw_read_key_b64(void)  { return read_line_file(KEY_PATH); }

bool lotw_cert_present(void)
{
    struct stat st;
    return stat(CERT_PATH, &st) == 0 && st.st_size > 64 &&
           stat(KEY_PATH,  &st) == 0 && st.st_size > 64;
}

// ---- signing (mbedtls) ----------------------------------------------------

static int rng_shim(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

typedef struct {
    mbedtls_pk_context pk;
} lotw_sign_ctx_t;

// RSA PKCS#1 v1.5 over SHA-1 - exactly tqsllib's EVP_SignInit(EVP_sha1()).
static int sign_cb(void *vctx, const uint8_t *data, size_t len,
                   uint8_t *sig, size_t *sig_len)
{
    lotw_sign_ctx_t *ctx = (lotw_sign_ctx_t *)vctx;
    unsigned char hash[20];
    if (mbedtls_sha1(data, len, hash) != 0)
        return -1;
    size_t olen = 0;
    int rc = mbedtls_pk_sign(&ctx->pk, MBEDTLS_MD_SHA1, hash, sizeof hash,
                             sig, *sig_len, &olen, rng_shim, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_sign failed: -0x%04x", (unsigned)-rc);
        return -1;
    }
    *sig_len = olen;
    return 0;
}

// Parse the stored base64 PKCS#8 key into ctx->pk. Caller must
// mbedtls_pk_free() on success.
static bool sign_ctx_init(lotw_sign_ctx_t *ctx, char *err, size_t errlen)
{
    mbedtls_pk_init(&ctx->pk);
    char *key_b64 = lotw_read_key_b64();
    if (!key_b64) {
        snprintf(err, errlen, "no LoTW private key stored");
        return false;
    }
    size_t der_cap = strlen(key_b64) * 3 / 4 + 4;
    unsigned char *der = psram_alloc(der_cap);
    size_t der_len = 0;
    bool ok = false;
    if (der && mbedtls_base64_decode(der, der_cap, &der_len,
                                     (const unsigned char *)key_b64,
                                     strlen(key_b64)) == 0) {
        int rc = mbedtls_pk_parse_key(&ctx->pk, der, der_len, NULL, 0,
                                      rng_shim, NULL);
        if (rc == 0) {
            ok = true;
        } else {
            snprintf(err, errlen, "key parse failed (-0x%04x)", (unsigned)-rc);
        }
    } else {
        snprintf(err, errlen, "key base64 decode failed");
    }
    if (der) {
        memset(der, 0, der_cap);  // don't leave key material lying around
        free(der);
    }
    memset(key_b64, 0, strlen(key_b64));
    free(key_b64);
    if (!ok)
        mbedtls_pk_free(&ctx->pk);
    return ok;
}

// ---- PEM reconstruction ---------------------------------------------------

// Rebuild a certificate PEM (64-char wrapped body + armor) from the stored
// single-line base64 - lotw_tq8_build() ships the PEM body verbatim into the
// tCERT record, so the wrapping here defines the on-wire format (64 chars
// matches OpenSSL/TQSL exactly).
static char *cert_pem_from_b64(const char *b64)
{
    size_t blen = strlen(b64);
    size_t cap = blen + blen / 64 + 80;
    char *pem = psram_alloc(cap);
    if (!pem) return NULL;
    size_t o = (size_t)snprintf(pem, cap, "-----BEGIN CERTIFICATE-----\n");
    for (size_t i = 0; i < blen; i += 64) {
        size_t n = blen - i < 64 ? blen - i : 64;
        memcpy(pem + o, b64 + i, n);
        o += n;
        pem[o++] = '\n';
    }
    o += (size_t)snprintf(pem + o, cap - o, "-----END CERTIFICATE-----\n");
    return pem;
}

// ---- gzip container --------------------------------------------------------

// gzip with "stored" (uncompressed) deflate blocks. Always-valid gzip with
// ~20 lines of code and no dependence on the ROM miniz compressor's unknown
// compile-time configuration; TQ8s are a few KB, so compression buys nothing
// over WiFi anyway. CRC32 comes from the ROM (zlib-compatible when seeded 0).
static uint8_t *gzip_store(const uint8_t *src, size_t n, size_t *out_len)
{
    size_t nblocks = n / 65535 + 1;
    size_t cap = 10 + n + nblocks * 5 + 8 + 16;
    uint8_t *g = psram_alloc(cap);
    if (!g) return NULL;
    size_t o = 0;
    g[o++] = 0x1f; g[o++] = 0x8b;   // magic
    g[o++] = 0x08;                  // deflate
    g[o++] = 0x00;                  // no flags
    memset(g + o, 0, 4); o += 4;    // mtime unset
    g[o++] = 0x00;                  // XFL
    g[o++] = 0x03;                  // OS = unix (matches zlib's gzopen)
    size_t rem = n;
    const uint8_t *p = src;
    do {
        uint16_t blk = rem > 65535 ? 65535 : (uint16_t)rem;
        g[o++] = (rem <= 65535) ? 0x01 : 0x00;   // BFINAL + BTYPE=00 (stored)
        g[o++] = (uint8_t)(blk & 0xff);
        g[o++] = (uint8_t)(blk >> 8);
        g[o++] = (uint8_t)(~blk & 0xff);
        g[o++] = (uint8_t)((uint16_t)~blk >> 8);
        memcpy(g + o, p, blk);
        o += blk; p += blk; rem -= blk;
    } while (rem > 0);
    uint32_t crc = esp_rom_crc32_le(0, src, n);
    uint32_t isz = (uint32_t)n;
    for (int i = 0; i < 4; i++) g[o++] = (uint8_t)(crc >> (8 * i));
    for (int i = 0; i < 4; i++) g[o++] = (uint8_t)(isz >> (8 * i));
    *out_len = o;
    return g;
}

// ---- ADIF record -> lotw_qso_t --------------------------------------------

typedef struct {
    char call[16];
    char band[8];
    char mode[12];   // the LoTW mode, already collapsed from ADIF MODE+SUBMODE
    char freq[16];
    char date[12];   // "YYYY-MM-DD"
    char time[12];   // "HH:MM:SSZ"
} lotw_rec_t;

// Extract + reformat one ADIF log record. Returns 1 = usable, 0 = skip
// permanently (no LoTW band / mangled record), so the cursor can advance
// past it instead of wedging the batch forever.
static int fill_rec(int idx, lotw_rec_t *r)
{
    char line[512];
    if (!adif_log_get_record(idx, line, sizeof line))
        return 0;
    char date[12] = "", tim[8] = "";
    if (!adif_log_extract_field(line, "CALL", r->call, sizeof r->call) ||
        !adif_log_extract_field(line, "BAND", r->band, sizeof r->band) ||
        !adif_log_extract_field(line, "MODE", r->mode, sizeof r->mode) ||
        !adif_log_extract_field(line, "QSO_DATE", date, sizeof date) ||
        !adif_log_extract_field(line, "TIME_ON", tim, sizeof tim)) {
        ESP_LOGW(TAG, "record %d missing required LoTW field - skipping", idx);
        return 0;
    }
    if (!adif_log_extract_field(line, "FREQ", r->freq, sizeof r->freq))
        r->freq[0] = '\0';
    if (strlen(date) != 8 || strlen(tim) != 6) {
        ESP_LOGW(TAG, "record %d has bad date/time - skipping", idx);
        return 0;
    }
    // FT4 is logged the ADIF-correct way (MODE=MFSK + SUBMODE=FT4), but LoTW
    // has no MFSK mode of its own, so the pair is collapsed back to the LoTW
    // mode before anything is signed. lotw_mode_from_adif() returns "FT4" here
    // and leaves every other mode - FT8 included - exactly as it was, so the
    // bytes this uploader produces are unchanged from what LoTW has already
    // been accepting.
    {
        char submode[12] = "";
        if (adif_log_extract_field(line, "SUBMODE", submode, sizeof submode)) {
            const char *m = lotw_mode_from_adif(r->mode, submode);
            if (m != r->mode) snprintf(r->mode, sizeof r->mode, "%s", m);
        }
    }
    // ADIF YYYYMMDD / HHMMSS -> TQ8 "YYYY-MM-DD" / "HH:MM:SSZ"
    snprintf(r->date, sizeof r->date, "%.4s-%.2s-%.2s", date, date + 4, date + 6);
    snprintf(r->time, sizeof r->time, "%.2s:%.2s:%.2sZ", tim, tim + 2, tim + 4);
    return 1;
}

// ---- TQ8 build -------------------------------------------------------------

uint8_t *lotw_build_tq8_gz(int from, int max_qsos,
                           int *consumed_out, int *signed_out,
                           size_t *len_out, char *err, size_t errlen)
{
    *consumed_out = 0;
    *signed_out = 0;
    err[0] = '\0';

    qmx_settings_t cfg;
    settings_load_all(&cfg);
    if (!cfg.my_callsign[0]) { snprintf(err, errlen, "callsign not set"); return NULL; }
    if (!cfg.lotw_dxcc[0])   { snprintf(err, errlen, "LoTW DXCC entity not set"); return NULL; }
    if (!lotw_cert_present()) { snprintf(err, errlen, "no LoTW certificate imported"); return NULL; }

    int total = adif_log_count();
    if (from >= total) { snprintf(err, errlen, "nothing to sign"); return NULL; }
    int want = total - from;
    if (want > max_qsos) want = max_qsos;

    lotw_rec_t *recs = psram_alloc(sizeof(lotw_rec_t) * (size_t)want);
    lotw_qso_t *qsos = psram_alloc(sizeof(lotw_qso_t) * (size_t)want);
    if (!recs || !qsos) {
        free(recs); free(qsos);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }

    int consumed = 0, nq = 0;
    for (; consumed < want; consumed++) {
        lotw_rec_t *r = &recs[nq];
        if (!fill_rec(from + consumed, r))
            continue;
        qsos[nq] = (lotw_qso_t){
            .call = r->call, .band = r->band, .mode = r->mode,
            .freq = r->freq, .qso_date = r->date, .qso_time = r->time,
        };
        nq++;
    }

    uint8_t *gz = NULL;
    char *cert_b64 = NULL, *cert_pem = NULL, *tq8 = NULL;
    lotw_sign_ctx_t sctx;
    bool sctx_ok = false;

    if (nq == 0)  // everything in range was skip-worthy; not an error
        goto out;

    cert_b64 = lotw_read_cert_b64();
    cert_pem = cert_b64 ? cert_pem_from_b64(cert_b64) : NULL;
    if (!cert_pem) { snprintf(err, errlen, "certificate read failed"); goto out; }

    if (!sign_ctx_init(&sctx, err, errlen))
        goto out;
    sctx_ok = true;

    {
        char ident[64];
        snprintf(ident, sizeof ident, "QMX-Panadapter %s",
                 esp_app_get_description()->version);
        // Tolerate a county entered in the ADIF "ST,County" style: LoTW wants
        // the name alone and rejects the combined form outright, so strip
        // anything up to and including a comma rather than fail the upload on
        // a formatting choice the operator can't be expected to know about.
        const char *county = cfg.lotw_county;
        const char *comma  = strchr(county, ',');
        if (comma) {
            county = comma + 1;
            while (*county == ' ') county++;
        }
        lotw_station_t st = {
            .callsign   = cfg.my_callsign,
            .dxcc       = cfg.lotw_dxcc,
            .gridsquare = cfg.my_grid,
            .cqz        = cfg.lotw_cqz,
            .ituz       = cfg.lotw_ituz,
            .us_state   = cfg.lotw_state,
            .us_county  = county,
            .cert_pem   = cert_pem,
        };
        tq8 = lotw_tq8_build(&st, qsos, nq, ident, sign_cb, &sctx, err, errlen);
    }
    if (!tq8)
        goto out;

    gz = gzip_store((const uint8_t *)tq8, strlen(tq8), len_out);
    if (!gz)
        snprintf(err, errlen, "out of memory (gzip)");

out:
    if (sctx_ok) mbedtls_pk_free(&sctx.pk);
    free(tq8);
    free(cert_pem);
    free(cert_b64);
    free(recs);
    free(qsos);
    *consumed_out = consumed;
    *signed_out = gz ? nq : 0;
    return gz;
}

// ---- upload ----------------------------------------------------------------

typedef struct { char *buf; size_t len; size_t cap; } resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    resp_buf_t *r = (resp_buf_t *)evt->user_data;
    if (!r || r->len + 1 >= r->cap) return ESP_OK;
    size_t avail = r->cap - r->len - 1;
    size_t n = (size_t)evt->data_len < avail ? (size_t)evt->data_len : avail;
    memcpy(r->buf + r->len, evt->data, n);
    r->len += n;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

// Extracts the token/text after `marker` up to the closing "-->", trimmed.
static void extract_comment(const char *html, const char *marker,
                            char *out, size_t out_sz)
{
    out[0] = '\0';
    const char *p = strstr(html, marker);
    if (!p) return;
    p += strlen(marker);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    const char *e = strstr(p, "-->");
    if (!e) return;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
    size_t n = (size_t)(e - p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

// POST the gzipped TQ8 as RFC1867 multipart, field name "upfile" (TQSL's
// DEFAULT_UPL_FIELD). Response is HTML carrying <!-- .UPL. accepted/rejected -->
// and <!-- .UPLMESSAGE. text --> comments (TQSL's DEFAULT_UPL_STATUSRE /
// MESSAGERE). No login needed - the payload is self-authenticating.
static bool lotw_post_tq8(const uint8_t *gz, size_t gz_len,
                          bool *accepted, char *msg, size_t msg_sz)
{
    *accepted = false;
    msg[0] = '\0';

    static const char boundary[] = "qmxPanadapterTq8Boundary";
    char head[256];
    int head_len = snprintf(head, sizeof head,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"upfile\"; filename=\"qmx_panadapter.tq8\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n", boundary);
    char tail[64];
    int tail_len = snprintf(tail, sizeof tail, "\r\n--%s--\r\n", boundary);

    size_t body_len = (size_t)head_len + gz_len + (size_t)tail_len;
    char *body = psram_alloc(body_len);
    if (!body) {
        snprintf(msg, msg_sz, "out of memory (POST body)");
        return false;
    }
    memcpy(body, head, (size_t)head_len);
    memcpy(body + head_len, gz, gz_len);
    memcpy(body + head_len + gz_len, tail, (size_t)tail_len);

    // LoTW's response page is a full HTML document; the .UPL. comments sit
    // near the top, but give it generous room in PSRAM anyway.
    const size_t resp_cap = 16384;
    char *resp = psram_alloc(resp_cap);
    if (!resp) {
        free(body);
        snprintf(msg, msg_sz, "out of memory (response)");
        return false;
    }
    resp[0] = '\0';
    resp_buf_t resp_ctx = { resp, 0, resp_cap };

    esp_http_client_config_t http_cfg = {
        .url           = LOTW_UPLOAD_URL,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 30000,
        .event_handler = http_event_handler,
        .user_data     = &resp_ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(body); free(resp);
        snprintf(msg, msg_sz, "client init failed");
        return false;
    }
    char ctype[80];
    snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", ctype);
    esp_http_client_set_post_field(client, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LoTW POST transport error: %s (heap_i=%uKB heap_p=%uKB)",
                 esp_err_to_name(err),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024));
        snprintf(msg, msg_sz, "network error: %s", esp_err_to_name(err));
        free(resp);
        return false;
    }
    if (status != 200) {
        snprintf(msg, msg_sz, "HTTP %d", status);
        free(resp);
        return true;
    }

    char result[32];
    extract_comment(resp, "<!-- .UPL.", result, sizeof result);
    extract_comment(resp, "<!-- .UPLMESSAGE.", msg, msg_sz);
    ESP_LOGI(TAG, "LoTW result='%s' message='%.100s'", result, msg);
    // SUBSTRING, not equality: TQSL's own check is "does the captured status
    // CONTAIN DEFAULT_UPL_STATUSOK", so a decorated status (e.g. an "accepted"
    // with a qualifier) still counts as success there. An exact compare was
    // stricter than the reference implementation and would have reported a
    // failure - and declined to advance the upload cursor - on a successful
    // upload. Still matched against the extracted <!-- .UPL. --> comment only,
    // never the raw page, so a login/landing page can't false-positive.
    // ...but a substring test alone cannot tell "accepted" from "not accepted"
    // or "rejected - not accepted", and reporting one of those as success is
    // the worst possible failure here: the cursor advances and the QSOs are
    // never retried. Refuse any status that also carries a negative word.
    if (strstr(result, "accepted") != NULL &&
        strstr(result, "not accepted") == NULL &&
        strstr(result, "rejected")     == NULL) {
        *accepted = true;
    } else if (!msg[0]) {
        snprintf(msg, msg_sz, result[0] ? "LoTW: %s" : "unexpected LoTW response",
                 result);
    }
    free(resp);
    return true;
}

bool lotw_upload_pending(lotw_upload_result_t *result)
{
    if (!result) return false;
    memset(result, 0, sizeof *result);

    qmx_settings_t cfg;
    settings_load_all(&cfg);
    if (!lotw_cert_present()) {
        snprintf(result->error, sizeof result->error, "no LoTW certificate imported");
        return false;
    }
    if (!cfg.lotw_dxcc[0]) {
        snprintf(result->error, sizeof result->error, "LoTW DXCC entity not set");
        return false;
    }

    int total = adif_log_count();
    uint32_t cursor = cfg.lotw_uploaded_n;

    while ((int)cursor < total) {
        int consumed = 0, nsigned = 0;
        size_t gz_len = 0;
        char err[120] = "";
        uint8_t *gz = lotw_build_tq8_gz((int)cursor, LOTW_BATCH_MAX,
                                        &consumed, &nsigned, &gz_len,
                                        err, sizeof err);
        if (!gz) {
            if (nsigned == 0 && consumed > 0 && !err[0]) {
                // Whole batch was skip-worthy records - just advance past them.
                cursor += (uint32_t)consumed;
                result->skipped += consumed;
                continue;
            }
            snprintf(result->error, sizeof result->error, "%s", err);
            result->failed = 1;
            break;
        }

        bool accepted = false;
        char msg[120] = "";
        bool transport_ok = lotw_post_tq8(gz, gz_len, &accepted, msg, sizeof msg);
        free(gz);

        if (!transport_ok || !accepted) {
            // Don't advance the cursor - a retry after the cause is fixed
            // resumes exactly here (same policy as QRZ/eQSL).
            snprintf(result->error, sizeof result->error, "%s",
                     msg[0] ? msg : "upload rejected");
            result->failed = 1;
            break;
        }

        cursor += (uint32_t)consumed;
        result->uploaded += nsigned;
        result->skipped  += consumed - nsigned;
        // Keep whatever LoTW said about the batch we just sent. It is the only
        // thing that can distinguish "queued and fine" from "accepted the file,
        // dropped every QSO in it".
        if (msg[0]) snprintf(result->note, sizeof result->note, "%s", msg);
    }

    if (cursor > cfg.lotw_uploaded_n)
        settings_set_lotw_uploaded_n(cursor);

    ESP_LOGI(TAG, "upload batch: %d uploaded, %d skipped, %d failed (%s)%s%s",
             result->uploaded, result->skipped, result->failed,
             result->failed ? result->error : "ok",
             result->note[0] ? " - LoTW says: " : "", result->note);
    return true;
}
