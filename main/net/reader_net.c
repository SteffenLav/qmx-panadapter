// See reader_net.h. Background HTTPS fetch of the docs markdown into a SPIFFS
// cache, mirroring the esp_http_client + esp_crt_bundle pattern used by
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

// Assumed public path for the raw source markdown, mirrored alongside the built
// site (see plan §1d). Confirm/adjust when the site-side copy step is set up.
#define DOCS_URL          "https://tab5.lav.dk/md/index.md"
#define READER_CACHE_PATH "/spiffs/reader.md"
#define DOCS_MAX_BYTES    (96 * 1024)
#define USER_AGENT        "qmx-panadapter-tab5"

static volatile bool s_busy = false;

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

static bool write_cache(const char *buf, size_t len)
{
    FILE *f = fopen(READER_CACHE_PATH, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot open %s for write", READER_CACHE_PATH); return false; }
    size_t w = fwrite(buf, 1, len, f);
    fflush(f);
    fclose(f);
    return w == len;
}

static void fetch_task(void *arg)
{
    (void)arg;

    if (!wifi_is_connected()) {
        ESP_LOGI(TAG, "WiFi down — using cached docs");
        reader_view_notify_loaded(true);   // render whatever cache exists, mark offline
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    reader_view_notify_status("Downloading…");

    char *body = heap_caps_malloc(DOCS_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        ESP_LOGW(TAG, "OOM");
        reader_view_notify_loaded(true);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    body[0] = '\0';
    resp_buf_t ctx = { body, 0, DOCS_MAX_BYTES };

    esp_http_client_config_t cfg = {
        .url               = DOCS_URL,
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
            ok = write_cache(ctx.buf, ctx.len);
            ESP_LOGI(TAG, "fetched %u bytes, cache=%s", (unsigned)ctx.len, ok ? "ok" : "FAIL");
        } else {
            ESP_LOGW(TAG, "fetch failed: err=%s status=%d len=%u",
                     esp_err_to_name(err), status, (unsigned)ctx.len);
        }
        esp_http_client_cleanup(client);
    }
    heap_caps_free(body);

    if (ok) {
        reader_view_notify_status("");
        reader_view_notify_loaded(false);
    } else {
        reader_view_notify_status("Download failed — showing cached copy");
        reader_view_notify_loaded(true);
    }

    s_busy = false;
    vTaskDelete(NULL);
}

void reader_net_load_index(void)
{
    if (s_busy) return;
    s_busy = true;
    // 6 KB stack: TLS handshake + small file. PSRAM stack (non-realtime).
    if (!psram_task_create(fetch_task, "reader_net", 6144, NULL, 4, tskNO_AFFINITY)) {
        s_busy = false;
    }
}
