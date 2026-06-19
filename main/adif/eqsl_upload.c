// Uploads logged FT8 QSOs to eQSL.cc's "real-time logger interface"
// (https://www.eqsl.cc/qslcard/ImportADIF.cfm, spec: qslcard/ImportADIF.txt).
//
// Unlike QRZ, eQSL accepts a whole batch of QSOs (a full ADIF blob with
// header + multiple <EOR> records) in a single POST, and has no API-key
// scheme - auth is the user's eQSL username + password as plain form fields.
// Response is an HTML page containing a "Result: x out of y records added"
// string on success, or one of several documented "Error:"/"Warning:"
// strings.
//
// Progress is tracked the same way as qrz_upload.c: a plain count
// (eqsl_uploaded_n) rather than per-record state, since adif_log.c only ever
// appends - anything before that offset was uploaded by an earlier run.

#include "eqsl_upload.h"
#include "adif_log.h"
#include "settings.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "eqsl_upload";
#define EQSL_API_URL   "https://www.eqsl.cc/qslcard/ImportADIF.cfm"

// Records per request. eQSL has no documented size limit but the spec warns
// against very long URLs for the GET form - we POST instead, but still keep
// batches modest so the whole blob (raw + percent-encoded copy) stays a
// comfortable PSRAM allocation rather than chasing a huge single request.
#define EQSL_BATCH_MAX 20
#define EQSL_RAW_CAP   8192   // raw ADIF blob buffer (PSRAM)

// Percent-encodes src into dst (application/x-www-form-urlencoded). Silently
// truncates if dst is too small - callers size dst generously.
static void url_encode(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 4 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            o += (size_t)snprintf(dst + o, 4, "%%%02X", c);
        }
    }
    dst[o < dst_sz ? o : dst_sz - 1] = '\0';
}

// Accumulates HTTP_EVENT_ON_DATA chunks into a fixed buffer - same pattern as
// qrz_upload.c.
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

// POSTs one ADIF batch to eQSL. Returns false only on a transport-level
// failure; otherwise sets *ok per whether a "Result:" string was found, and
// fills reason on failure (from eQSL's "Error:" text or an unexpected
// response).
static bool eqsl_post_batch(const char *user, const char *pswd, const char *adif_blob,
                             bool *ok, int *added, int *of_total,
                             char *reason, size_t reason_sz)
{
    *ok = false;
    *added = 0;
    *of_total = 0;
    reason[0] = '\0';

    size_t adif_len    = strlen(adif_blob);
    size_t enc_adif_cap = adif_len * 3 + 16;  // worst case: every byte escaped
    char *enc_adif = heap_caps_malloc(enc_adif_cap, MALLOC_CAP_SPIRAM);
    if (!enc_adif) {
        snprintf(reason, reason_sz, "out of memory encoding batch");
        return false;
    }
    url_encode(adif_blob, enc_adif, enc_adif_cap);

    char enc_user[64], enc_pswd[96];
    url_encode(user, enc_user, sizeof(enc_user));
    url_encode(pswd, enc_pswd, sizeof(enc_pswd));

    size_t body_cap = strlen(enc_adif) + strlen(enc_user) + strlen(enc_pswd) + 64;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        heap_caps_free(enc_adif);
        snprintf(reason, reason_sz, "out of memory building request");
        return false;
    }
    int body_len = snprintf(body, body_cap, "ADIFData=%s&EQSL_USER=%s&EQSL_PSWD=%s",
                             enc_adif, enc_user, enc_pswd);
    heap_caps_free(enc_adif);

    char resp[2048] = {0};
    resp_buf_t resp_ctx = { resp, 0, sizeof(resp) };

    esp_http_client_config_t cfg = {
        .url           = EQSL_API_URL,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 15000,
        .event_handler = http_event_handler,
        .user_data     = &resp_ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        heap_caps_free(body);
        snprintf(reason, reason_sz, "client init failed");
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "eQSL POST transport error: %s (heap_i=%uKB heap_p=%uKB)",
                 esp_err_to_name(err),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024));
        snprintf(reason, reason_sz, "network error: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        heap_caps_free(body);
        return false;
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    heap_caps_free(body);

    if (status != 200) {
        snprintf(reason, reason_sz, "HTTP %d", status);
        return true;
    }

    const char *r = strstr(resp, "Result:");
    if (r && sscanf(r, "Result: %d out of %d records added", added, of_total) == 2) {
        *ok = true;
        return true;
    }

    const char *e = strstr(resp, "Error:");
    if (e) {
        e += 6;
        while (*e == ' ') e++;
        const char *end = strchr(e, '<');  // messages are followed by a <BR> tag
        size_t n = end ? (size_t)(end - e) : strlen(e);
        if (n >= reason_sz) n = reason_sz - 1;
        memcpy(reason, e, n);
        reason[n] = '\0';
    } else {
        snprintf(reason, reason_sz, "unexpected response from eQSL");
    }
    return true;
}

bool eqsl_upload_pending(eqsl_upload_result_t *result)
{
    if (!result) return false;
    result->uploaded = 0;
    result->failed   = 0;
    result->error[0] = '\0';

    qmx_settings_t cfg;
    settings_load_all(&cfg);

    if (!cfg.eqsl_user[0] || !cfg.eqsl_pswd[0]) {
        snprintf(result->error, sizeof(result->error), "no eQSL username/password set");
        return false;
    }

    int total = adif_log_count();
    uint32_t n = cfg.eqsl_uploaded_n;
    if ((int)n >= total) {
        return true;  // nothing pending
    }

    while ((int)n < total) {
        char *blob = heap_caps_malloc(EQSL_RAW_CAP, MALLOC_CAP_SPIRAM);
        if (!blob) {
            snprintf(result->error, sizeof(result->error), "out of memory building batch");
            result->failed = 1;
            break;
        }
        int blob_len = snprintf(blob, EQSL_RAW_CAP,
            "<ADIF_VER:5>3.1.4 <PROGRAMID:13>QMX-Panadapter <EOH>\n");

        int batch_n = 0;
        uint32_t idx = n;
        for (; (int)idx < total && batch_n < EQSL_BATCH_MAX; idx++) {
            char rec[512];
            if (!adif_log_get_record((int)idx, rec, sizeof(rec))) break;
            int rec_len = (int)strlen(rec);
            if (blob_len + rec_len + 2 >= EQSL_RAW_CAP) break;  // batch full
            blob_len += snprintf(blob + blob_len, EQSL_RAW_CAP - blob_len, "%s\n", rec);
            batch_n++;
        }

        if (batch_n == 0) { heap_caps_free(blob); break; }

        bool ok = false;
        int added = 0, of_total = 0;
        char reason[96];
        bool transport_ok = eqsl_post_batch(cfg.eqsl_user, cfg.eqsl_pswd, blob,
                                             &ok, &added, &of_total, reason, sizeof(reason));
        heap_caps_free(blob);

        if (!transport_ok || !ok) {
            snprintf(result->error, sizeof(result->error), "%s", reason);
            result->failed = 1;
            break;
        }

        ESP_LOGI(TAG, "eQSL batch: %d of %d records added (batch had %d)", added, of_total, batch_n);
        result->uploaded += batch_n;
        n = idx;
    }

    if (n > cfg.eqsl_uploaded_n) {
        settings_set_eqsl_uploaded_n(n);
    }
    return true;
}
