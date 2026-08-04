// Live spots store + POTA fetcher. See spots.h for the contract.
//
// Sizing, measured against the live API 2026-08-04: 94 spots in 39.6 KB, mixed
// SSB/CW/FT8. So one fetch is a ~40 KB body - trivial to parse, but far too
// big for internal RAM, hence the PSRAM response buffer (and CLAUDE.md's rule
// that anything under 16 KB silently lands in internal RAM cuts the other way
// here: this is deliberately a big allocation so it goes where we want).
//
// WiFi discipline: this board's esp_hosted link is fragile under concurrent
// load, which is why the LoTW import and the QRZ/eQSL uploads all quiet the
// spectrum WebSocket for their duration. A 40 KB fetch once a minute is far
// lighter than those, but it follows the same rule for the same reason.

#include "spots.h"
#include "webserver_ws.h"     // webserver_ws_set_paused
#include "wifi.h"             // wifi_is_connected
#include "storage/settings.h"
#include "util/psram_task.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "spots";

// ---- TEMP DIAGNOSTIC (2026-08-04): why does ALL outbound HTTPS fail? --------
//
// Every TLS attempt on this bench dies as:
//     esp-aes: Failed to allocate memory for the array of DMA descriptors
//     esp-tls-mbedtls: mbedtls_ctr_drbg_seed returned -0x0001
// update_check fails identically, so it is device-wide TLS, not spots.
//
// On ESP32-P4 SOC_AES_SUPPORT_DMA=1 and there is NO small-buffer fallback, so
// EVERY hardware-AES operation - including ctr_drbg's 16-byte ones - allocates
// a descriptor array first (esp_aes_dma_core.c:422):
//     heap_caps_aligned_calloc(8, n, 16, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)
// That is 16-48 bytes at 8-byte alignment. audio.c's periodic line reports
// 335-1515 B free in MALLOC_CAP_DMA, which OUGHT to satisfy it - so the open
// question is whether the pool is fragmented below that size, or whether the
// caps COMBINATION the AES port asks for resolves to a different and emptier
// region than the MALLOC_CAP_DMA-only figure we have been measuring.
//
// So: report free AND largest-free-block for each cap set, and directly attempt
// the allocation itself. largest_free_block walks the heap with interrupts off,
// which must never sit on a periodic path (the DSI cyan-flash rule), hence the
// hard cap - baseline at init plus the first few failures, then silence.
#define DMA_PROBE_MAX 4
static int s_dma_probes;

static void dma_probe(const char *where)
{
    if (s_dma_probes >= DMA_PROBE_MAX) return;
    s_dma_probes++;

    // Exactly what esp_aes_dma_core.c asks for on a small (ctr_drbg-sized) op.
    void *p_aes = heap_caps_aligned_calloc(8, 3, 16,
                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Same caps, no alignment demand - separates "cannot align" from "no room".
    void *p_plain = heap_caps_malloc(48, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGW(TAG, "DMAPROBE[%s]: dma free=%uB lblk=%uB | dma|int free=%uB lblk=%uB | int free=%uB lblk=%uB | aes48@8=%s plain48=%s",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             p_aes ? "OK" : "FAIL", p_plain ? "OK" : "FAIL");

    free(p_aes);
    free(p_plain);
}
// ---- end TEMP DIAGNOSTIC ---------------------------------------------------

#define POTA_URL       "https://api.pota.app/spot/activator"
#define RESP_CAP       (96 * 1024)     // ~2.4x the measured body, room to grow
#define FETCH_PERIOD_S 60              // POTA spots carry an ~expire of minutes
#define RETRY_PERIOD_S 20              // after a failure

static spot_t           *s_store;              // PSRAM, SPOTS_MAX entries
static int               s_count;
static SemaphoreHandle_t s_lock;
static int64_t           s_last_ok_us;
static volatile bool     s_refresh_req;

// ---- store -----------------------------------------------------------------

static bool lock(void)   { return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE; }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

int spots_get(spot_t *out, int max)
{
    if (!out || max <= 0 || !lock()) return 0;
    int n = s_count < max ? s_count : max;
    memcpy(out, s_store, (size_t)n * sizeof(spot_t));
    unlock();
    return n;
}

int spots_get_in_range(spot_t *out, int max, uint32_t lo_hz, uint32_t hi_hz)
{
    if (!out || max <= 0 || !lock()) return 0;
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++)
        if (s_store[i].freq_hz >= lo_hz && s_store[i].freq_hz <= hi_hz)
            out[n++] = s_store[i];
    unlock();
    return n;
}

int spots_age_s(void)
{
    if (!s_last_ok_us) return -1;
    return (int)((esp_timer_get_time() - s_last_ok_us) / 1000000);
}

void spots_request_refresh(void) { s_refresh_req = true; }

// ---- fetch -----------------------------------------------------------------

typedef struct { char *buf; size_t len; size_t cap; } resp_buf_t;

static esp_err_t on_data(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    resp_buf_t *r = (resp_buf_t *)evt->user_data;
    if (!r || r->len + 1 >= r->cap) return ESP_OK;    // full: keep what we have
    size_t avail = r->cap - r->len - 1;
    size_t n = (size_t)evt->data_len < avail ? (size_t)evt->data_len : avail;
    memcpy(r->buf + r->len, evt->data, n);
    r->len += n;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

static spot_mode_t mode_from_str(const char *m)
{
    if (!m || !m[0]) return SPOT_MODE_OTHER;
    if (strcasecmp(m, "CW") == 0)  return SPOT_MODE_CW;
    if (strcasecmp(m, "SSB") == 0 || strcasecmp(m, "USB") == 0 ||
        strcasecmp(m, "LSB") == 0 || strcasecmp(m, "PHONE") == 0) return SPOT_MODE_SSB;
    if (strncasecmp(m, "FT", 2) == 0 || strcasecmp(m, "PSK") == 0 ||
        strcasecmp(m, "RTTY") == 0 || strcasecmp(m, "DATA") == 0) return SPOT_MODE_DIGI;
    return SPOT_MODE_OTHER;
}

// "2026-08-04T16:02:24" (UTC, no zone suffix) -> unix seconds. Returns 0 when
// unparseable, which the UI treats as "age unknown" rather than "ancient".
static int64_t parse_spot_time(const char *s)
{
    if (!s) return 0;
    struct tm tmv = {0};
    int y, mo, d, h, mi, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = sec;
    // ESP-IDF newlib runs UTC, so mktime() == timegm() here (same assumption
    // the RTC code documents and relies on).
    return (int64_t)mktime(&tmv);
}

// Replace the store with what the payload holds. Spots without a usable
// frequency are dropped rather than clamped - a spot you cannot tune to is
// worse than no spot.
static int parse_pota(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "POTA: unparseable JSON"); return -1; }
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); ESP_LOGW(TAG, "POTA: not an array"); return -1; }

    int n = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, root) {
        if (n >= SPOTS_MAX) break;
        const cJSON *jf = cJSON_GetObjectItem(it, "frequency");
        double khz = 0;
        if (cJSON_IsNumber(jf))       khz = jf->valuedouble;
        else if (cJSON_IsString(jf))  khz = atof(jf->valuestring);
        if (khz < 1000.0 || khz > 60000.0) continue;      // not HF/6m: skip

        spot_t sp = {0};
        sp.freq_hz = (uint32_t)(khz * 1000.0);
        sp.source  = SPOT_SRC_POTA;
        const cJSON *jc = cJSON_GetObjectItem(it, "activator");
        const cJSON *jr = cJSON_GetObjectItem(it, "reference");
        const cJSON *jm = cJSON_GetObjectItem(it, "mode");
        const cJSON *jt = cJSON_GetObjectItem(it, "spotTime");
        if (cJSON_IsString(jc)) snprintf(sp.call, sizeof(sp.call), "%s", jc->valuestring);
        if (cJSON_IsString(jr)) snprintf(sp.ref,  sizeof(sp.ref),  "%s", jr->valuestring);
        sp.mode = mode_from_str(cJSON_IsString(jm) ? jm->valuestring : NULL);
        sp.heard_unix = cJSON_IsString(jt) ? parse_spot_time(jt->valuestring) : 0;
        if (!sp.call[0]) continue;

        if (lock()) { s_store[n++] = sp; unlock(); }
    }
    cJSON_Delete(root);
    if (lock()) { s_count = n; unlock(); }
    return n;
}

static void fetch_once(void)
{
    char *buf = heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGW(TAG, "no PSRAM for the response buffer"); return; }
    buf[0] = '\0';
    resp_buf_t ctx = { buf, 0, RESP_CAP };

    esp_http_client_config_t cfg = {
        .url               = POTA_URL,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 15000,
        .event_handler     = on_data,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { heap_caps_free(buf); return; }
    esp_http_client_set_header(client, "Accept", "application/json");

    // Same courtesy the uploads pay: keep the spectrum stream off the link
    // while the transfer runs (see the LoTW note in CLAUDE.md).
    webserver_ws_set_paused(true);
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    webserver_ws_set_paused(false);

    if (status == 200 && ctx.len > 0) {
        int n = parse_pota(buf);
        if (n >= 0) {
            s_last_ok_us = esp_timer_get_time();
            ESP_LOGI(TAG, "POTA: %d spots (%u bytes)", n, (unsigned)ctx.len);
        }
    } else {
        ESP_LOGW(TAG, "POTA fetch failed (status=%d err=0x%x)", status, err);
        dma_probe("fetch-fail");    // TEMP DIAGNOSTIC, capped
    }
    heap_caps_free(buf);
}

static void spots_task(void *arg)
{
    (void)arg;
    int wait_s = 5;
    for (;;) {
        for (int i = 0; i < wait_s * 2; i++) {       // 500 ms granularity so a
            vTaskDelay(pdMS_TO_TICKS(500));          // refresh request is prompt
            if (s_refresh_req) break;
        }
        s_refresh_req = false;

        qmx_settings_t s;
        settings_load_all(&s);
        if (!s.spots_en || !wifi_is_connected()) { wait_s = 10; continue; }

        fetch_once();
        wait_s = (spots_age_s() == 0) ? FETCH_PERIOD_S : RETRY_PERIOD_S;
    }
}

void spots_init(void)
{
    if (s_store) return;
    s_store = heap_caps_calloc(SPOTS_MAX, sizeof(spot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock  = xSemaphoreCreateMutex();
    if (!s_store || !s_lock) { ESP_LOGE(TAG, "init failed"); return; }
    dma_probe("init");    // TEMP DIAGNOSTIC: baseline before the pool collapses
    psram_task_create(spots_task, "spots", 6144, NULL, 2, tskNO_AFFINITY);
    ESP_LOGI(TAG, "spot fetcher started (POTA)");
}
