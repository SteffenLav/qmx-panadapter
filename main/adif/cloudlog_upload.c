// Uploads logged QSOs to a self-hosted Cloudlog or Wavelog instance (#171,
// Mark G4MEM).
//
// API (from the Cloudlog wiki; the wiki's own example uses a Wavelog URL, which
// is why both work here):
//   POST <base>/index.php/api/qso     Content-Type: application/json
//   {"key":"<api key>","station_profile_id":"<id>","type":"adif","string":"<adif>"}
// Several QSOs may be sent in one request, and duplicate checking is done
// server-side - so a repeated upload is harmless, which makes the cursor a
// performance optimisation rather than a correctness requirement.
//
// ⚠ The response format is NOT documented. So success is judged by HTTP status
// alone and the body is reported verbatim on failure, rather than inventing a
// schema and then mis-parsing a future version of it.
//
// ⛔ WHAT MAKES THIS DIFFERENT FROM QRZ/eQSL/LoTW: the address is the
// OPERATOR'S. This is the only upload target whose host is not compiled in, so
// it is the only one that can be pointed at a machine on the local network -
// and the only one where a plain-HTTP URL is a reasonable thing to want. The
// decision of whether that is safe lives in util/net_guard.c and is re-taken on
// EVERY upload, never cached; see the header there for why that ordering is the
// whole safety argument.

#include "cloudlog_upload.h"
#include "adif_log.h"
#include "settings.h"
#include "util/net_guard.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "cloudlog";

// Records per request. Cloudlog accepts many; the cap keeps one PSRAM buffer
// bounded and means a failure costs at most this much re-sending.
#define CL_BATCH_MAX   20
#define CL_RECORD_MAX  512
#define CL_BODY_CAP    (CL_BATCH_MAX * (CL_RECORD_MAX + 8) + 512)

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

// Escapes a string into a JSON string body (no surrounding quotes). ADIF is
// plain ASCII, but a callsign field is still operator-supplied text, and an
// unescaped quote or backslash would produce a malformed request rather than a
// rejected one - much harder to diagnose from the other end.
static size_t json_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 7 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
        case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
        case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
        case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
        case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
        case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
        default:
            if (c < 0x20) o += (size_t)snprintf(dst + o, 7, "\\u%04X", c);
            else          dst[o++] = (char)c;
            break;
        }
    }
    dst[o] = '\0';
    return o;
}

// Our current IPv4 address and netmask, converted to the SAME convention
// net_ipv4_parse() produces.
//
// ⛔ esp_netif hands these back in NETWORK byte order, and this function used to
// pass them straight through. On this little-endian CPU that made 192.168.1.8
// read as 0x0801A8C0 against a parsed 0xC0A80108, and turned a 255.255.255.0
// netmask into 0x00FFFFFF - so the masked equality compared the wrong end of the
// address and every upload to a LAN server was refused with "is not on this
// network". Reported by Mark G4MEM within hours of v1.8.7, with the cause
// correctly identified. Do not remove the conversion.
static bool own_ipv4(uint32_t *ip_out, uint32_t *mask_out)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    *ip_out   = net_ipv4_from_network_order(info.ip.addr);
    *mask_out = net_ipv4_from_network_order(info.netmask.addr);
    return true;
}

// Decides whether `url` may be contacted at all, and says why not in words the
// operator can act on. Re-run for every upload - see the header note.
static bool address_is_acceptable(const char *url, char *why, size_t why_sz)
{
    net_scheme_t scheme;
    char host[80];
    uint16_t port;

    if (!net_url_parse(url, &scheme, host, sizeof(host), &port)) {
        snprintf(why, why_sz, "the Cloudlog address is not a usable URL");
        return false;
    }
    if (scheme == NET_SCHEME_HTTPS) return true;   // certificate does the work

    // Plain HTTP. Allowed only onto our own subnet.
    uint32_t target = 0, ours = 0, mask = 0;
    if (!own_ipv4(&ours, &mask)) {
        snprintf(why, why_sz, "no network address yet - connect to WiFi first");
        return false;
    }

    // The host must be an IP LITERAL for this check to mean anything. A name
    // would have to be resolved, and whatever answered could differ from what
    // answers when the connection is actually made - so the safe address is the
    // one that cannot change between the check and the request.
    if (!net_ipv4_parse(host, &target)) {
        snprintf(why, why_sz,
                 "http:// needs a numeric address, e.g. http://192.168.1.20");
        return false;
    }
    if (!net_plaintext_allowed(target, ours, mask)) {
        // Host is capped so the message cannot be pushed out of the buffer by a
        // long address - the reason must survive, the address is a detail.
        snprintf(why, why_sz, "%.20s is not on this network - use https://", host);
        return false;
    }
    return true;
}

bool cloudlog_upload_pending(cloudlog_upload_result_t *result)
{
    if (!result) return false;
    result->uploaded = 0;
    result->failed   = 0;
    result->error[0] = '\0';

    qmx_settings_t cfg;
    settings_load_all(&cfg);

    if (!cfg.cloudlog_url[0] || !cfg.cloudlog_key[0]) {
        snprintf(result->error, sizeof(result->error), "Cloudlog is not set up yet");
        return false;
    }

    if (!address_is_acceptable(cfg.cloudlog_url, result->error, sizeof(result->error))) {
        ESP_LOGW(TAG, "refusing upload: %s", result->error);
        return false;
    }

    int total = adif_log_count();
    uint32_t start = cfg.cloudlog_uploaded_n;
    if ((int)start >= total) return true;    // nothing pending

    char url[160];
    snprintf(url, sizeof(url), "%s/index.php/api/qso", cfg.cloudlog_url);
    // Trim a trailing slash on the operator's base URL so we never send "//".
    {
        char *p = strstr(url, "//index.php");
        if (p) memmove(p, p + 1, strlen(p));
    }

    // PSRAM: this is well over the 16 KB threshold below which IDF forces an
    // allocation into scarce internal RAM (see the malloc audit in CLAUDE.md).
    char *body = heap_caps_malloc(CL_BODY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *adif = heap_caps_malloc(CL_BODY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body || !adif) {
        free(body); free(adif);
        snprintf(result->error, sizeof(result->error), "out of memory");
        return false;
    }

    uint32_t n = start;
    while ((int)n < total) {
        // Gather up to CL_BATCH_MAX records into one ADIF blob.
        size_t alen = 0;
        uint32_t batch_end = n;
        for (int i = 0; i < CL_BATCH_MAX && (int)batch_end < total; i++, batch_end++) {
            char record[CL_RECORD_MAX];
            if (!adif_log_get_record((int)batch_end, record, sizeof(record))) {
                ESP_LOGW(TAG, "couldn't read record %u from log", (unsigned)batch_end);
                break;
            }
            int w = snprintf(adif + alen, CL_BODY_CAP - alen, "%s\n", record);
            if (w < 0 || (size_t)w >= CL_BODY_CAP - alen) break;
            alen += (size_t)w;
        }
        if (batch_end == n) break;   // nothing readable

        char esc_key[128], esc_stn[24];
        json_escape(cfg.cloudlog_key, esc_key, sizeof(esc_key));
        json_escape(cfg.cloudlog_station[0] ? cfg.cloudlog_station : "1",
                    esc_stn, sizeof(esc_stn));

        int hdr = snprintf(body, CL_BODY_CAP,
                           "{\"key\":\"%s\",\"station_profile_id\":\"%s\","
                           "\"type\":\"adif\",\"string\":\"", esc_key, esc_stn);
        if (hdr < 0 || (size_t)hdr >= CL_BODY_CAP) break;
        size_t blen = (size_t)hdr;
        blen += json_escape(adif, body + blen, CL_BODY_CAP - blen - 4);
        int tail = snprintf(body + blen, CL_BODY_CAP - blen, "\"}");
        if (tail < 0) break;
        blen += (size_t)tail;

        char resp[384] = {0};
        resp_buf_t resp_ctx = { resp, 0, sizeof(resp) };

        esp_http_client_config_t hcfg = {
            .url               = url,
            .method            = HTTP_METHOD_POST,
            .timeout_ms        = 15000,
            .event_handler     = http_event_handler,
            .user_data         = &resp_ctx,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&hcfg);
        if (!client) {
            snprintf(result->error, sizeof(result->error), "client init failed");
            result->failed = 1;
            break;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_post_field(client, body, (int)blen);

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "POST transport error: %s (heap_i=%uKB heap_p=%uKB)",
                     esp_err_to_name(err),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            snprintf(result->error, sizeof(result->error),
                     "could not reach %.32s: %.20s", cfg.cloudlog_url, esp_err_to_name(err));
            result->failed = 1;
            esp_http_client_cleanup(client);
            break;
        }
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (status < 200 || status >= 300) {
            // The body is quoted verbatim BECAUSE the response schema is
            // undocumented - whatever Cloudlog says is more useful to the
            // operator than our guess at what it meant.
            snprintf(result->error, sizeof(result->error), "HTTP %d from Cloudlog%s%.40s",
                     status, resp[0] ? ": " : "", resp);
            result->failed = 1;
            break;
        }

        result->uploaded += (int)(batch_end - n);
        n = batch_end;
        // Persist after every accepted batch, not once at the end: a reboot
        // mid-run then costs at most one batch of (harmless) re-sends rather
        // than re-uploading the whole log.
        settings_set_cloudlog_uploaded_n(n);
    }

    free(body);
    free(adif);
    ESP_LOGI(TAG, "upload batch: %d uploaded, %d failed", result->uploaded, result->failed);
    return true;
}
