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
static spot_t           *s_scratch;            // PSRAM, parse target (see parse_pota)
static int               s_count;
static SemaphoreHandle_t s_lock;
static int64_t           s_last_ok_us;
static volatile bool     s_refresh_req;
static volatile uint32_t s_version;

// ---- store -----------------------------------------------------------------

static bool lock_ms(int ms) { return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(ms)) == pdTRUE; }
static bool lock(void)   { return lock_ms(200); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

uint32_t spots_version(void) { return s_version; }

// How long a spot survives with no further mention. Matches the fade the UIs
// already apply (invisible at 30 minutes), so nothing is dropped while still
// being drawn.
#define SPOT_STALE_S 1800

void spots_publish(spot_source_t src, const spot_t *list, int n)
{
    if (!s_store || !lock()) return;

    // MERGE, not replace (operator, 2026-08-09: "sometimes all the spots
    // delete/shift to a new picture - I would think they change at a much more
    // steady and slow pace").
    //
    // This used to compact the whole source out and append whatever the fetch
    // returned, so every 60 s the entire POTA set was swapped for the API's
    // current view. A station the API happened to omit from one response
    // vanished and came back a minute later, and the ORDER changed wholesale -
    // which, since labels are placed first-come, re-shuffled which spots got a
    // label at all. The display churned far more than the band did.
    //
    // Now a fetch REFRESHES what it mentions and leaves the rest to age out on
    // their own. The spots carry heard_unix and both UIs already fade on it, so
    // ageing is the honest expiry - absence from one response is not evidence a
    // station has gone.
    int64_t now = (int64_t)time(NULL);

    // 1. Drop this source's genuinely stale entries (and keep every other source).
    int keep = 0;
    for (int i = 0; i < s_count; i++) {
        bool mine  = (s_store[i].source == src);
        bool stale = mine && s_store[i].heard_unix > 0 &&
                     (now - s_store[i].heard_unix) > SPOT_STALE_S;
        if (!stale) s_store[keep++] = s_store[i];
    }
    s_count = keep;

    // 2. Fold the fetch in: update a spot already known on the same frequency,
    //    otherwise append. Matching on call AND frequency so the same operator
    //    active on two bands stays two spots.
    for (int i = 0; i < n && list; i++) {
        int at = -1;
        for (int j = 0; j < s_count; j++) {
            if (s_store[j].source != src) continue;
            if (s_store[j].freq_hz != list[i].freq_hz) continue;
            if (strncmp(s_store[j].call, list[i].call, sizeof(s_store[j].call)) != 0) continue;
            at = j; break;
        }
        if (at >= 0) {
            s_store[at] = list[i];                 // same station, fresher details
        } else if (s_count < SPOTS_MAX) {
            s_store[s_count++] = list[i];
        }
    }

    s_version++;
    unlock();
}

int spots_get(spot_t *out, int max)
{
    if (!out || max <= 0 || !lock()) return 0;
    int n = s_count < max ? s_count : max;
    memcpy(out, s_store, (size_t)n * sizeof(spot_t));
    unlock();
    return n;
}

// How far apart two spots for the same callsign may sit and still be the same
// station. The RBN reports the CW carrier a skimmer measured; a POTA/SOTA spot
// carries whatever the activator (or a chaser) typed in. They routinely differ
// by a few hundred Hz, and rounding to the nearest kHz is common by hand.
#define SPOT_DUP_TOL_HZ 2000

static inline bool spot_same_station(const spot_t *a, const spot_t *b)
{
    uint32_t d = (a->freq_hz > b->freq_hz) ? a->freq_hz - b->freq_hz
                                           : b->freq_hz - a->freq_hz;
    return d <= SPOT_DUP_TOL_HZ && strcasecmp(a->call, b->call) == 0;
}

static int get_in_range_locked(spot_t *out, int max, uint32_t lo_hz, uint32_t hi_hz)
{
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++)
        if (s_store[i].freq_hz >= lo_hz && s_store[i].freq_hz <= hi_hz)
            out[n++] = s_store[i];

    // Collapse repeats of the same station into one entry. Two kinds occur, and
    // both were measured on the live feed (20 m, 2026-08-09, 79 spots in
    // window): an activator spotted on POTA AND heard by the RBN (2 of only 4
    // activation spots were doubled - which is why it stood out so badly), and
    // the RBN doubling ITSELF (5 pairs, every one exactly 100 Hz apart, i.e.
    // two skimmers rounding the same signal differently). Both put two labels
    // at almost the same x and make a busy band's lane unreadable.
    //
    // The earlier entry wins, because the store is newest-first - EXCEPT that
    // an activation spot always beats an RBN one, since it carries the park or
    // summit reference, which is the whole reason the operator is looking.
    //
    // Runs over the IN-RANGE subset (a band slice), not the whole 200-entry
    // store. The frequency test is an integer compare and rejects almost every
    // pair before the string compare, so the O(k^2) scan stays cheap enough for
    // the LVGL thread holding the store lock.
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++) {
            if (!spot_same_station(&out[i], &out[j])) continue;
            // Keep the activation spot in the surviving slot, whichever way
            // round the two happen to be.
            if (out[i].source != SPOT_SRC_RBN && out[j].source == SPOT_SRC_RBN) {
                spot_t tmp = out[j]; out[j] = out[i]; out[i] = tmp;
            }
            if (out[i].source == SPOT_SRC_RBN && out[j].source != SPOT_SRC_RBN)
                out[j].rbn_confirmed = true;
            if (out[i].rbn_confirmed) out[j].rbn_confirmed = true;
            // Take the later timestamp. A skimmer copying the station five
            // minutes ago is direct evidence it was on the air five minutes
            // ago, which beats the hour-old self-spot the activation feed is
            // still serving - and it makes the lane's age fade correct with no
            // change needed there.
            if (out[i].heard_unix > out[j].heard_unix)
                out[j].heard_unix = out[i].heard_unix;
            memmove(&out[i], &out[i + 1], (size_t)(n - i - 1) * sizeof(spot_t));
            n--;
            i--;                        // re-test the entry shifted into this slot
            break;
        }
    }
    return n;
}

int spots_get_in_range(spot_t *out, int max, uint32_t lo_hz, uint32_t hi_hz)
{
    if (!out || max <= 0 || !lock()) return 0;
    int n = get_in_range_locked(out, max, lo_hz, hi_hz);
    unlock();
    return n;
}

int spots_get_in_range_wait(spot_t *out, int max, uint32_t lo_hz, uint32_t hi_hz, int wait_ms)
{
    if (!out || max <= 0) return 0;
    if (!lock_ms(wait_ms)) return -1;      // caller keeps whatever it already drew
    int n = get_in_range_locked(out, max, lo_hz, hi_hz);
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
//
// Parses into s_scratch and swaps the finished table in under ONE lock. The
// first version wrote each entry straight into s_store and only set s_count at
// the end, so for the whole duration of a parse the live table held a mix of
// new and old entries while readers still saw the previous count - the lane
// could draw a call at another station's frequency. Fixed-size entries under a
// mutex meant it could never crash, which is exactly why it would have been an
// annoying one to find later.
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

        s_scratch[n++] = sp;
    }
    cJSON_Delete(root);

    spots_publish(SPOT_SRC_POTA, s_scratch, n);   // replaces only the POTA slice
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
    s_store   = heap_caps_calloc(SPOTS_MAX, sizeof(spot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scratch = heap_caps_calloc(SPOTS_MAX, sizeof(spot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock    = xSemaphoreCreateMutex();
    if (!s_store || !s_scratch || !s_lock) { ESP_LOGE(TAG, "init failed"); return; }
    dma_probe("init");    // TEMP DIAGNOSTIC: baseline before the pool collapses
    psram_task_create(spots_task, "spots", 6144, NULL, 2, tskNO_AFFINITY);
    ESP_LOGI(TAG, "spot fetcher started (POTA)");
}
