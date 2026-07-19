// See reader_net.h. Background HTTPS fetch of the docs markdown + TOC into
// SPIFFS caches, mirroring the esp_http_client + esp_crt_bundle pattern used by
// qrz_upload.c / lotw_upload.c.

#include "reader_net.h"
#include "ui/reader_view.h"
#include "wifi/wifi.h"
#include "util/psram_task.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "reader_net";

// Public base for the raw source markdown, mirrored alongside the built site by
// mkdocs_reader_export.py (see plan §1d). Confirm/adjust if the site-side path
// changes.
#define DOCS_BASE         "https://tab5.lav.dk/md/"
#define PAGE_CACHE        "/spiffs/reader.md"
#define TOC_CACHE         "/spiffs/reader_toc.json"
#define DOCS_MAX_BYTES    (96 * 1024)
#define USER_AGENT        "qmx-panadapter-tab5"

static volatile bool s_busy = false;
static char s_job_path[96];
static bool s_job_toc;

// Accumulate the response body into a PSRAM buffer.
typedef struct { char *buf; size_t len; size_t cap; } resp_buf_t;

static esp_err_t on_data(esp_http_client_event_t *evt)
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

static bool write_cache(const char *path, const char *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot open %s for write", path); return false; }
    size_t w = fwrite(buf, 1, len, f);
    fflush(f);
    fclose(f);
    return w == len;
}

// GET url -> cache_path. Returns true only on HTTP 200 with a non-empty body
// successfully written.
static bool fetch_url_to_file(const char *url, const char *cache_path)
{
    char *body = heap_caps_malloc(DOCS_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { ESP_LOGW(TAG, "OOM"); return false; }
    body[0] = '\0';
    resp_buf_t ctx = { body, 0, DOCS_MAX_BYTES };

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 15000,
        .event_handler     = on_data,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    bool ok = false;
    if (client) {
        esp_http_client_set_header(client, "User-Agent", USER_AGENT);
        esp_err_t err = esp_http_client_perform(client);
        int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
        if (err == ESP_OK && status == 200 && ctx.len > 0) {
            ok = write_cache(cache_path, ctx.buf, ctx.len);
            ESP_LOGI(TAG, "fetched %u bytes -> %s (%s)", (unsigned)ctx.len, cache_path, ok ? "ok" : "write FAIL");
        } else {
            ESP_LOGW(TAG, "fetch failed: %s err=%s status=%d len=%u",
                     url, esp_err_to_name(err), status, (unsigned)ctx.len);
        }
        esp_http_client_cleanup(client);
    }
    heap_caps_free(body);
    return ok;
}

static void fetch_task(void *arg)
{
    (void)arg;

    if (!wifi_is_connected()) {
        ESP_LOGI(TAG, "WiFi down - using cached docs");
        reader_view_notify_loaded(true);   // render whatever cache exists, mark offline
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    reader_view_notify_status("Downloading...");

    // TOC first (if requested) so the contents list is fresh before the page.
    if (s_job_toc) {
        char url[192];
        snprintf(url, sizeof(url), "%stoc.json", DOCS_BASE);
        if (fetch_url_to_file(url, TOC_CACHE)) reader_view_notify_toc_loaded();
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s", DOCS_BASE, s_job_path);
    bool ok = fetch_url_to_file(url, PAGE_CACHE);

    if (ok) {
        reader_view_notify_status("");
        reader_view_notify_loaded(false);
    } else {
        reader_view_notify_status("Download failed - showing cached copy");
        reader_view_notify_loaded(true);
    }

    s_busy = false;
    vTaskDelete(NULL);
}

void reader_net_fetch(const char *page_rel, bool with_toc)
{
    if (s_busy) return;
    s_busy = true;
    snprintf(s_job_path, sizeof(s_job_path), "%s",
             (page_rel && page_rel[0]) ? page_rel : "index.md");
    s_job_toc = with_toc;
    // 6 KB stack: TLS handshake + small file. PSRAM stack (non-realtime).
    if (!psram_task_create(fetch_task, "reader_net", 6144, NULL, 4, tskNO_AFFINITY)) {
        s_busy = false;
    }
}

void reader_net_load_index(void)
{
    reader_net_fetch("index.md", true);
}
