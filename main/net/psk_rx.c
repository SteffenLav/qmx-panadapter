// Propagation feedback - see psk_rx.h.
//
// The response is flat XML, one self-closing <receptionReport .../> element per
// report, ~1 KB for a handful. Parsed with a plain attribute scan rather than
// an XML library: there is no nesting to track, and pulling in a parser for
// this shape would cost more internal RAM than the whole feature.
//
// Verified against the live collector before this was written (2026-08-09):
//   <receptionReport receiverCallsign="EI4HQ" receiverLocator="IO51UU"
//     senderCallsign="DL2JRM" senderLocator="JO60jx12ta" frequency="14035000"
//     flowStartSeconds="1786254898" mode="CW" isSender="1"
//     receiverDXCC="Ireland" receiverDXCCCode="EI" sNR="5" />
// A query for a callsign with no activity returns a well-formed document with
// no report elements, which is the normal quiet case and not an error.

#include "psk_rx.h"
#include "storage/settings.h"
#include "util/maidenhead.h"
#include "wifi/wifi.h"
#include "webserver_ws.h"
#include "util/psram_task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "psk_rx";

// PSK Reporter asks that the same query is not repeated more often than every
// five minutes, and enforces it. This is the floor for BOTH the periodic cycle
// and any refresh request, so an operator tapping a refresh button cannot get
// us rate-limited.
#define PSK_RX_MIN_INTERVAL_S 300
#define PSK_RX_WINDOW_S       86400   // look back a day: a POTA op wants the morning, not the minute
#define RESP_CAP              16384

static psk_rx_report_t  *s_store;         // PSRAM
static int               s_count;
static SemaphoreHandle_t s_lock;
static int64_t           s_last_ok_us;
static int64_t           s_last_try_us;
static volatile bool     s_refresh_req;

static bool lock_ms(int ms)
{
    return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---- parsing ---------------------------------------------------------------

// Copies the value of name="..." out of a single element. Returns false when
// the attribute is absent, which is normal - receiverLocator and sNR are both
// optional in practice.
static bool attr(const char *elem, const char *name, char *out, size_t out_sz)
{
    if (out && out_sz) out[0] = '\0';
    char pat[32];
    int n = snprintf(pat, sizeof(pat), "%s=\"", name);
    if (n <= 0 || (size_t)n >= sizeof(pat)) return false;
    const char *p = strstr(elem, pat);
    if (!p) return false;
    p += n;
    const char *e = strchr(p, '"');
    if (!e) return false;
    size_t len = (size_t)(e - p);
    if (!out || out_sz == 0) return true;
    if (len > out_sz - 1) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

bool psk_rx_parse_report(const char *elem, psk_rx_report_t *out)
{
    if (!elem || !out) return false;
    memset(out, 0, sizeof(*out));
    out->distance_km = -1;
    out->bearing_deg = -1;

    if (!attr(elem, "receiverCallsign", out->rx_call, sizeof(out->rx_call))) return false;
    if (!out->rx_call[0]) return false;

    attr(elem, "receiverLocator", out->rx_grid, sizeof(out->rx_grid));
    attr(elem, "receiverDXCC",    out->rx_dxcc, sizeof(out->rx_dxcc));
    attr(elem, "mode",            out->mode,    sizeof(out->mode));

    char buf[24];
    if (attr(elem, "frequency", buf, sizeof(buf)))        out->freq_hz    = (uint32_t)strtoul(buf, NULL, 10);
    if (attr(elem, "flowStartSeconds", buf, sizeof(buf))) out->heard_unix = (int64_t)strtoll(buf, NULL, 10);
    // sNR is genuinely optional and legitimately negative - "no value" and
    // "-13 dB" must not collapse to the same thing, so absence is recorded as
    // a sentinel rather than 0, which is a perfectly ordinary report.
    out->snr_db = attr(elem, "sNR", buf, sizeof(buf)) ? (int16_t)atoi(buf) : (int16_t)-32768;
    return true;
}

// Fills in distance/bearing from our own grid. Separate from parsing so the
// parser stays pure string work and can be self-tested with no settings.
static void add_geometry(psk_rx_report_t *r, bool have_me, double my_lat, double my_lon)
{
    if (!have_me || !r->rx_grid[0]) return;
    double la, lo;
    if (!maidenhead_to_latlon(r->rx_grid, &la, &lo)) return;
    r->distance_km = (int32_t)(haversine_km(my_lat, my_lon, la, lo) + 0.5);
    r->bearing_deg = (int16_t)(bearing_deg(my_lat, my_lon, la, lo) + 0.5);
}

// ---- accessors -------------------------------------------------------------

int psk_rx_get(psk_rx_report_t *out, int max)
{
    if (!out || max <= 0 || !lock_ms(50)) return 0;
    int n = s_count < max ? s_count : max;
    memcpy(out, s_store, (size_t)n * sizeof(psk_rx_report_t));
    unlock();
    return n;
}

int psk_rx_count(void)
{
    if (!lock_ms(50)) return 0;
    int n = s_count;
    unlock();
    return n;
}

int psk_rx_unique_receivers(void)
{
    if (!lock_ms(50)) return 0;
    int uniq = 0;
    for (int i = 0; i < s_count; i++) {
        bool seen = false;
        for (int j = 0; j < i && !seen; j++)
            if (strcasecmp(s_store[i].rx_call, s_store[j].rx_call) == 0) seen = true;
        if (!seen) uniq++;
    }
    unlock();
    return uniq;
}

int psk_rx_max_distance_km(void)
{
    if (!lock_ms(50)) return -1;
    int best = -1;
    for (int i = 0; i < s_count; i++)
        if (s_store[i].distance_km > best) best = s_store[i].distance_km;
    unlock();
    return best;
}

int psk_rx_age_s(void)
{
    if (!s_last_ok_us) return -1;
    return (int)((esp_timer_get_time() - s_last_ok_us) / 1000000);
}

void psk_rx_request_refresh(void) { s_refresh_req = true; }

// ---- fetch -----------------------------------------------------------------

typedef struct { char *buf; size_t len, cap; } resp_buf_t;

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

static int parse_body(const char *xml)
{
    qmx_settings_t s;
    settings_load_all(&s);
    double my_lat = 0, my_lon = 0;
    bool have_me = s.my_grid[0] && maidenhead_to_latlon(s.my_grid, &my_lat, &my_lon);

    // Build into a local list, then swap under one lock - a reader must never
    // see a half-replaced table (the same discipline spots.c documents).
    psk_rx_report_t *tmp = heap_caps_malloc(sizeof(psk_rx_report_t) * PSK_RX_MAX,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) return -1;

    // File-static, not a local: this whole path runs only on psk_rx_task (the
    // one caller of fetch_once), and 512 bytes of stack is real money here -
    // see CLAUDE.md, "Task stacks on this board are TINY".
    static char elem[512];

    int n = 0;
    const char *p = xml;
    while (n < PSK_RX_MAX && (p = strstr(p, "<receptionReport")) != NULL) {
        const char *end = strchr(p, '>');
        if (!end) break;
        size_t len = (size_t)(end - p);
        if (len >= sizeof(elem)) len = sizeof(elem) - 1;
        memcpy(elem, p, len);
        elem[len] = '\0';
        if (psk_rx_parse_report(elem, &tmp[n])) {
            add_geometry(&tmp[n], have_me, my_lat, my_lon);
            n++;
        }
        p = end + 1;
    }

    if (lock_ms(1000)) {
        memcpy(s_store, tmp, sizeof(psk_rx_report_t) * (size_t)n);
        s_count = n;
        unlock();
    }
    heap_caps_free(tmp);
    return n;
}

static void fetch_once(const char *mycall)
{
    char *buf = heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGW(TAG, "no PSRAM for the response buffer"); return; }
    buf[0] = '\0';
    resp_buf_t ctx = { buf, 0, RESP_CAP };

    char url[192];
    snprintf(url, sizeof(url),
             "https://retrieve.pskreporter.info/query?senderCallsign=%s"
             "&flowStartSeconds=-%d&rronly=1", mycall, PSK_RX_WINDOW_S);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 20000,
        .event_handler     = on_data,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { heap_caps_free(buf); return; }
    // Identify ourselves. The collector is a free service run by one person and
    // asks that clients be attributable.
    esp_http_client_set_header(client, "User-Agent", "qmx-panadapter (github.com/SteffenLav/qmx-panadapter)");

    // Same courtesy the spot fetcher and the uploads pay - keep the spectrum
    // stream off this link while the transfer runs.
    webserver_ws_set_paused(true);
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    webserver_ws_set_paused(false);

    if (status == 200) {
        int n = parse_body(buf);
        if (n >= 0) {
            s_last_ok_us = esp_timer_get_time();
            ESP_LOGI(TAG, "%d report(s) of %s from %d receiver(s), furthest %d km",
                     n, mycall, psk_rx_unique_receivers(), psk_rx_max_distance_km());
        }
    } else {
        // A 4xx here usually means we queried too often. Backing off is the
        // only correct response - retrying harder is how a client gets banned.
        ESP_LOGW(TAG, "query failed (status=%d err=0x%x)", status, err);
    }
    heap_caps_free(buf);
}

static void psk_rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < 20; i++) {          // 500 ms granularity for a prompt refresh
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_refresh_req) break;
        }
        bool asked = s_refresh_req;
        s_refresh_req = false;

        qmx_settings_t s;
        settings_load_all(&s);
        if (!s.psk_rx_en || !s.my_callsign[0] || !wifi_is_connected()) continue;

        // One floor for both paths. An operator hammering a refresh button must
        // not be able to breach the collector's stated rate limit.
        int64_t now = esp_timer_get_time();
        int64_t since = (now - s_last_try_us) / 1000000;
        if (s_last_try_us && since < PSK_RX_MIN_INTERVAL_S) {
            if (asked) ESP_LOGI(TAG, "refresh ignored - %lld s since last query, minimum is %d",
                                (long long)since, PSK_RX_MIN_INTERVAL_S);
            continue;
        }
        s_last_try_us = now;
        fetch_once(s.my_callsign);
    }
}

void psk_rx_init(void)
{
    if (s_store) return;
    s_store = heap_caps_calloc(PSK_RX_MAX, sizeof(psk_rx_report_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock  = xSemaphoreCreateMutex();
    if (!s_store || !s_lock) { ESP_LOGE(TAG, "init failed"); return; }
    // 8192, not the 4096 this first shipped with: the TLS handshake inside
    // esp_http_client_perform() runs on THIS stack and overflowed it every
    // time - a "Stack protection fault ... task psk_rx" crash loop, with
    // SHA-256 round constants sitting in the register dump. spots.c does the
    // same HTTPS work on 6144; this task also carries a qmx_settings_t, so it
    // gets more. Do not trim it back.
    psram_task_create(psk_rx_task, "psk_rx", 8192, NULL, 2, tskNO_AFFINITY);
    ESP_LOGI(TAG, "propagation feedback ready (query every %d s when enabled)",
             PSK_RX_MIN_INTERVAL_S);
}

// ---- self-test -------------------------------------------------------------

bool psk_rx_selftest(void)
{
    // Captured verbatim from the live collector, 2026-08-09. If the collector
    // ever changes its attribute names this fails at boot instead of silently
    // reporting "nobody is hearing you" forever, which is indistinguishable
    // from a genuinely dead band and is the whole risk with this feature.
    static const char *ELEM =
        "<receptionReport receiverCallsign=\"EI4HQ\" receiverLocator=\"IO51UU\" "
        "senderCallsign=\"DL2JRM\" senderLocator=\"JO60jx12ta\" frequency=\"14035000\" "
        "flowStartSeconds=\"1786254898\" mode=\"CW\" isSender=\"1\" "
        "receiverDXCC=\"Ireland\" receiverDXCCCode=\"EI\" sNR=\"5\" /";
    psk_rx_report_t r;
    bool ok = true;
    if (!psk_rx_parse_report(ELEM, &r)) { ESP_LOGE(TAG, "selftest: parse failed"); return false; }
    if (strcmp(r.rx_call, "EI4HQ") != 0)   { ESP_LOGE(TAG, "selftest: rx_call '%s'", r.rx_call); ok = false; }
    if (strcmp(r.rx_grid, "IO51UU") != 0)  { ESP_LOGE(TAG, "selftest: rx_grid '%s'", r.rx_grid); ok = false; }
    if (strcmp(r.rx_dxcc, "Ireland") != 0) { ESP_LOGE(TAG, "selftest: dxcc '%s'", r.rx_dxcc); ok = false; }
    if (strcmp(r.mode, "CW") != 0)         { ESP_LOGE(TAG, "selftest: mode '%s'", r.mode); ok = false; }
    if (r.freq_hz != 14035000u)            { ESP_LOGE(TAG, "selftest: freq %lu", (unsigned long)r.freq_hz); ok = false; }
    if (r.snr_db != 5)                     { ESP_LOGE(TAG, "selftest: snr %d", r.snr_db); ok = false; }
    if (r.heard_unix != 1786254898LL)      { ESP_LOGE(TAG, "selftest: time %lld", (long long)r.heard_unix); ok = false; }

    // A negative SNR must survive, and must not be confused with "absent".
    psk_rx_report_t r2;
    if (!psk_rx_parse_report("<receptionReport receiverCallsign=\"K1ABC\" sNR=\"-13\" /", &r2) ||
        r2.snr_db != -13) { ESP_LOGE(TAG, "selftest: negative SNR"); ok = false; }

    // No sNR attribute at all -> sentinel, not 0 (0 dB is an ordinary report).
    psk_rx_report_t r3;
    if (!psk_rx_parse_report("<receptionReport receiverCallsign=\"K1ABC\" mode=\"FT8\" /", &r3) ||
        r3.snr_db != (int16_t)-32768) { ESP_LOGE(TAG, "selftest: missing SNR sentinel"); ok = false; }

    // No receiver callsign is not a report.
    psk_rx_report_t r4;
    if (psk_rx_parse_report("<receptionReport senderCallsign=\"OZ1LAV\" /", &r4)) {
        ESP_LOGE(TAG, "selftest: accepted a report with no receiver"); ok = false;
    }

    ESP_LOGI(TAG, "selftest: %s", ok ? "PASS" : "FAIL");
    return ok;
}
