// Uploads logged FT8 QSOs to the QRZ Logbook API (https://logbook.qrz.com/api).
//
// QRZ's API is a single endpoint taking form-encoded POST bodies:
//   KEY=<api key>&ACTION=INSERT&ADIF=<one ADIF record>
// and replying with a form-encoded body of its own:
//   RESULT=OK&LOGID=...&COUNT=1          (accepted)
//   RESULT=FAIL&REASON=<text>             (rejected)
//
// One record per request - there's no batch-insert action - so a full sync
// is one HTTP round trip per QSO. Records already uploaded are tracked by a
// simple count (qrz_uploaded_n) rather than per-record state, since
// adif_log.c only ever appends: anything before that offset was uploaded by
// an earlier run, anything at/after it wasn't.

#include "qrz_upload.h"
#include "adif_log.h"
#include "settings.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "qrz_upload";
#define QRZ_API_URL "https://logbook.qrz.com/api"

// Percent-encodes src into dst (application/x-www-form-urlencoded). Silently
// truncates if dst is too small - callers size dst generously since ADIF
// records and API keys are both well-bounded.
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

// Accumulates HTTP_EVENT_ON_DATA chunks into a fixed buffer - the standard
// ESP-IDF pattern for reading a small response body via esp_http_client.
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

// POSTs one ADIF record to QRZ. Returns false only on a transport-level
// failure (couldn't reach QRZ at all); otherwise sets *ok per QRZ's
// RESULT=OK/FAIL and fills reason on failure.
static bool qrz_post_one(const char *api_key, const char *adif_line,
                          bool *ok, char *reason, size_t reason_sz)
{
    *ok = false;
    reason[0] = '\0';

    char enc_key[96], enc_adif[768];
    url_encode(api_key, enc_key, sizeof(enc_key));
    url_encode(adif_line, enc_adif, sizeof(enc_adif));

    char body[900];
    int body_len = snprintf(body, sizeof(body), "KEY=%s&ACTION=INSERT&ADIF=%s", enc_key, enc_adif);
    if (body_len < 0 || (size_t)body_len >= sizeof(body)) {
        snprintf(reason, reason_sz, "ADIF record too long to upload");
        return true;  // not a transport failure - just nothing we can send
    }

    char resp[512] = {0};
    resp_buf_t resp_ctx = { resp, 0, sizeof(resp) };

    esp_http_client_config_t cfg = {
        .url           = QRZ_API_URL,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 12000,
        .event_handler = http_event_handler,
        .user_data     = &resp_ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(reason, reason_sz, "client init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        // ESP_ERR_HTTP_CONNECT covers everything from DNS failure to TLS
        // handshake OOM - log heap headroom alongside it so a failure caught
        // with diagnostic logging enabled actually points at a cause instead
        // of just "connect failed". This is the firmware's first-ever
        // outbound HTTPS connection (SNTP is UDP) so this path is unproven.
        ESP_LOGW(TAG, "QRZ POST transport error: %s (heap_i=%uKB heap_p=%uKB)",
                 esp_err_to_name(err),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024));
        snprintf(reason, reason_sz, "network error: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        snprintf(reason, reason_sz, "HTTP %d", status);
        return true;
    }
    if (strstr(resp, "RESULT=OK")) {
        *ok = true;
        return true;
    }

    const char *r = strstr(resp, "REASON=");
    if (r) {
        r += 7;
        const char *amp = strchr(r, '&');
        size_t n = amp ? (size_t)(amp - r) : strlen(r);
        if (n >= reason_sz) n = reason_sz - 1;
        memcpy(reason, r, n);
        reason[n] = '\0';
    } else {
        snprintf(reason, reason_sz, "unexpected response from QRZ");
    }
    return true;
}

bool qrz_upload_pending(qrz_upload_result_t *result)
{
    if (!result) return false;
    result->uploaded = 0;
    result->failed   = 0;
    result->error[0] = '\0';

    qmx_settings_t cfg;
    settings_load_all(&cfg);

    if (!cfg.qrz_api_key[0]) {
        snprintf(result->error, sizeof(result->error), "no QRZ API key set");
        return false;
    }

    int total = adif_log_count();
    uint32_t start = cfg.qrz_uploaded_n;
    if ((int)start >= total) {
        return true;  // nothing pending
    }

    uint32_t n = start;
    int skipped = 0;
    for (; (int)n < total; n++) {
        char record[512];
        if (!adif_log_get_record((int)n, record, sizeof(record))) {
            ESP_LOGW(TAG, "couldn't read record %u from log", (unsigned)n);
            break;
        }

        bool ok = false;
        char reason[80];
        if (!qrz_post_one(cfg.qrz_api_key, record, &ok, reason, sizeof(reason))) {
            snprintf(result->error, sizeof(result->error), "%s", reason);
            result->failed = 1;
            break;
        }
        if (!ok) {
            // A DUPLICATE rejection means the record is already in the QRZ
            // logbook - skip it and keep going. This happens whenever an
            // earlier upload succeeded on QRZ's side but the device rebooted
            // before the debounced NVS flush persisted the advanced
            // qrz_uploaded_n cursor (field-hit 2026-07-13 after a day of
            // dev reflashes): the cursor rolls back, the retry hits
            // "duplicate" on record 0, and the old stop-on-first-rejection
            // policy blocked every NEWER record behind it forever. All other
            // rejections (bad key, quota, ...) still stop the batch, since
            // those do apply to every subsequent record too.
            if (strstr(reason, "duplicate") != NULL) {
                ESP_LOGW(TAG, "QRZ record %u already in logbook (duplicate) - skipping", (unsigned)n);
                skipped++;
                continue;
            }
            ESP_LOGW(TAG, "QRZ rejected record %u: %s", (unsigned)n, reason);
            snprintf(result->error, sizeof(result->error), "%s", reason);
            result->failed = 1;
            break;
        }

        result->uploaded++;
    }

    if (n > start) {
        settings_set_qrz_uploaded_n(n);
    }
    ESP_LOGI(TAG, "upload batch: %d uploaded, %d duplicates skipped, %d failed",
             result->uploaded, skipped, result->failed);
    return true;
}
