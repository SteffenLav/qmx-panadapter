// Contributed by Uwe DL8UG, who wrote this module and sent it as a patch.
// Ported by him from his own rbn_monitor project. What changed on the way
// in - the spot map being opt-in rather than always running - is in the
// merge commit and in settings.h under spotmap_en.
// Who is hearing our own WSPR signal - see wspr_self.h.
//
// wsprnet.org's "olddb" lookup is an HTML page (no JSON/XML API exists for
// this), one <tr id="evenrow"|"oddrow"> per spot, 14 plain <td> cells in a
// fixed column order (verified against the live site, 2026-09-11):
//   Date | Call | Frequency | SNR | Drift | Grid | dBm | W | by | loc | km | mi | Mode | Version
// "Call" is the TRANSMITTER (us, since we searched on our own callsign);
// "by"/"loc" are the REPORTER's callsign/grid - the station that heard us,
// which is the only pair this module actually wants. Every cell is wrapped
// &nbsp;value&nbsp; regardless of column.
//
// Verified live query, 2026-09-11:
//   http://www.wsprnet.org/olddb?mode=html&band=all&limit=30&findcall=DL8UG&findReporter=&sort=date
// (querying the www host directly - the bare wsprnet.org host 302-redirects
// to it, and there is no reason to pay that extra round trip every poll).
//
// Plain HTTP, not HTTPS: verified live (three consecutive fetches, identical
// byte count each time) that this specific endpoint answers 200 directly
// over HTTP with no redirect - unlike every other read-only feed this
// project queries (hamqsl.com, PSK Reporter's own retrieve.*, spothole.app,
// tab5.lav.dk all 301 straight to HTTPS; POTA's API 403s plain HTTP
// outright). net/wsprnet.c's own POST to this same host has used plain HTTP
// for its entire history for the same reason. Skipping the TLS handshake
// avoids an mbedtls session on every one of this module's periodic polls -
// this data (who heard our own already-public WSPR beacon) carries no
// credential and needs no confidentiality.

#include "wspr_self.h"
#include "wifi.h"
#include "net/net_quiet.h"
#include "storage/settings.h"
#include "util/maidenhead.h"
#include "util/psram_task.h"
#include "webserver_ws.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>

static const char *TAG = "wspr_self";

#define RESP_CAP        65536   // same order of magnitude as psk_rx.c's own HTML/XML fetch
#define QUERY_LIMIT     30      // rows requested - plenty for "who's hearing me right now"
#define POLL_INTERVAL_S 180     // WSPR cycles are 2 min; this trails by half a cycle so a
                                 // just-uploaded report has landed in wsprnet's DB by the time we ask
#define WSPR_SELF_TTL_S (24 * 3600)   // same as net/rbn.c's RBN_SELF_TTL_S - see the reason there

static EXT_RAM_BSS_ATTR wspr_self_spot_t s_store[WSPR_SELF_MAX];
static int               s_count;
static SemaphoreHandle_t s_mutex;

// ---- store -----------------------------------------------------------------

static void store_expire(int64_t now)
{
    int keep = 0;
    for (int i = 0; i < s_count; i++)
        if (now - s_store[i].heard_unix <= WSPR_SELF_TTL_S) s_store[keep++] = s_store[i];
    s_count = keep;
}

// Same dedupe-by-reporter shape as net/rbn.c's note_self_spot() / net/pskr_self.c's store_add().
static void store_add(const wspr_self_spot_t *in)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    store_expire(in->heard_unix);
    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_store[i].call, in->call) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_count < WSPR_SELF_MAX) {
            slot = s_count++;
        } else {
            slot = 0;
            for (int i = 1; i < WSPR_SELF_MAX; i++)
                if (s_store[i].heard_unix < s_store[slot].heard_unix) slot = i;
        }
    }
    // A reporter can appear multiple times across a poll window (one row per
    // WSPR cycle they copied us in); keep the newest, matching what the map/
    // table want to show ("last heard").
    if (slot >= 0 && (s_store[slot].heard_unix == 0 || in->heard_unix >= s_store[slot].heard_unix))
        s_store[slot] = *in;
    xSemaphoreGive(s_mutex);
}

int wspr_self_spots_get(wspr_self_spot_t *out, int max)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    store_expire((int64_t)time(NULL));
    int n = s_count < max ? s_count : max;
    memcpy(out, s_store, n * sizeof(wspr_self_spot_t));
    xSemaphoreGive(s_mutex);
    return n;
}

void wspr_self_spots_clear(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_count = 0;
    xSemaphoreGive(s_mutex);
}

// ---- fetch + parse ----------------------------------------------------------

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

// Reads the next <td>...</td> cell starting at *cursor (bounded by row_end),
// strips one leading/trailing "&nbsp;" if present, and advances *cursor past
// it. Returns false once there are no more cells before row_end.
static bool next_td(const char **cursor, const char *row_end, char *out, size_t out_sz)
{
    const char *p = strstr(*cursor, "<td");
    if (!p || p >= row_end) return false;
    const char *gt = strchr(p, '>');
    if (!gt || gt >= row_end) return false;
    const char *start = gt + 1;
    const char *end = strstr(start, "</td>");
    if (!end || end > row_end) return false;

    size_t len = (size_t)(end - start);
    if (len >= 6 && strncmp(start, "&nbsp;", 6) == 0) { start += 6; len -= 6; }
    if (len >= 6 && strncmp(start + len - 6, "&nbsp;", 6) == 0) len -= 6;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, start, len);
    out[len] = '\0';

    *cursor = end + 5;   // past "</td>"
    return true;
}

// "YYYY-MM-DD HH:MM", always UTC (wsprnet's own convention) - this project's
// newlib uses UTC by default, so mktime() here is timegm() in every way that
// matters (same reasoning time_sync.c documents for its own RTC path).
static int64_t parse_wspr_date(const char *s)
{
    struct tm tmv = {0};
    int y, mo, d, h, mi;
    if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = 0;
    time_t t = mktime(&tmv);
    return (t > 0) ? (int64_t)t : 0;
}

static void parse_and_store(const char *html)
{
    int found = 0;
    const char *p = html;
    const char *row;
    while ((row = strstr(p, "<tr id=")) != NULL) {
        const char *row_end = strstr(row, "</tr>");
        if (!row_end) break;
        p = row_end + 5;

        // Column order: Date, Call(tx), Frequency, SNR, Drift, Grid(tx), dBm, W,
        // by(reporter call), loc(reporter grid), km, mi, Mode, Version.
        char field[14][24];
        const char *cur = row;
        int nf = 0;
        while (nf < 14 && next_td(&cur, row_end, field[nf], sizeof(field[nf]))) nf++;
        if (nf < 10) continue;   // short/malformed row - skip rather than misread columns

        wspr_self_spot_t sp = {0};
        snprintf(sp.call, sizeof(sp.call), "%.15s", field[8]);
        if (!sp.call[0]) continue;

        sp.heard_unix = parse_wspr_date(field[0]);
        if (sp.heard_unix <= 0) continue;

        float mhz = strtof(field[2], NULL);
        sp.freq_hz = (uint32_t)(mhz * 1e6f + 0.5f);
        sp.snr_db  = atoi(field[3]);

        double lat, lon;
        if (maidenhead_to_latlon(field[9], &lat, &lon)) {
            sp.lat = (float)lat; sp.lon = (float)lon; sp.has_pos = true;
            /* The grid as SENT - see the same note in net/pskr_self.c. */
            snprintf(sp.grid, sizeof(sp.grid), "%.6s", field[9]);
        } else {
            // Same diagnostic shape as net/pskr_self.c's own - wsprnet.org's
            // "loc" column IS the reporter's own grid, straight from their
            // upload, so a miss here means the grid string itself (not a
            // lookup, there is none for WSPR) didn't parse. field[9] is
            // already a bounded 24-byte copy (next_td()), safe to log as-is.
            ESP_LOGW(TAG, "no position for reporter '%s' - loc='%s' (len=%d, %s)",
                     sp.call, field[9], (int)strlen(field[9]),
                     field[9][0] ? "grid present but unparsed" : "no grid in report");
        }

        store_add(&sp);
        found++;
    }
    ESP_LOGI(TAG, "%d reception report(s) parsed", found);
}

static void fetch_once(const char *mycall)
{
    char *buf = heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGW(TAG, "no PSRAM for the response buffer"); return; }
    buf[0] = '\0';
    resp_buf_t ctx = { buf, 0, RESP_CAP };

    char call_upper[16];
    size_t i = 0;
    for (; mycall[i] && i + 1 < sizeof(call_upper); i++)
        call_upper[i] = (char)toupper((unsigned char)mycall[i]);
    call_upper[i] = '\0';

    char url[192];
    snprintf(url, sizeof(url),
             "http://www.wsprnet.org/olddb?mode=html&band=all&limit=%d&findcall=%s&findReporter=&sort=date",
             QUERY_LIMIT, call_upper);

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
    esp_http_client_set_header(client, "User-Agent", "qmx-panadapter (github.com/SteffenLav/qmx-panadapter)");

    // Same courtesy net/psk_rx.c's own periodic query pays - keep the spectrum
    // WS stream off this link while the transfer runs.
    webserver_ws_set_paused(true);
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    webserver_ws_set_paused(false);

    if (status == 200) {
        parse_and_store(buf);
    } else {
        ESP_LOGW(TAG, "query failed (status=%d err=0x%x)", status, err);
    }
    heap_caps_free(buf);
}

static void wspr_self_task(void *arg)
{
    (void)arg;
    while (!wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    /* ⛔ QUERY FIRST, THEN WAIT. The delay used to come first, so nothing
     * appeared for a full POLL_INTERVAL_S (3 min) after boot - the operator's
     * "the list starts populating like 3min after opening the map". A short
     * settle lets the post-Got-IP burst (SNTP, POTA, web server) pass. */
    vTaskDelay(pdMS_TO_TICKS(15000));
    for (bool first = true; ; first = false) {
        if (!first) vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_S * 1000));

        if (net_quiet_active()) continue;
        // The spot map is opt-in (settings.h, spotmap_en) and this is one of
        // its three feeds. Read every pass so the switch applies live.
        if (!settings_get_spotmap_en()) continue;
        // Deliberately NOT gated on settings_get_wspr_tx_en(): a query is
        // cheap (one GET, run at most every POLL_INTERVAL_S) and TX being off
        // right now says nothing about whether a report from an earlier
        // transmission - this session's or an older one, wsprnet.org keeps
        // history - is still worth showing. Same reasoning as RBN/PSK
        // Reporter self-spotting, neither of which is gated on "are you
        // transmitting" either.

        qmx_settings_t s;
        settings_load_all(&s);
        if (!s.my_callsign[0] || !wifi_is_connected()) continue;
        fetch_once(s.my_callsign);
    }
}

void wspr_self_init(void)
{
    if (s_mutex) return;
    s_mutex = xSemaphoreCreateMutex();
    // 6144 -> 9216: a qmx_settings_t local in this file, generous not
    // incremental - see sd_archive.c's comment for why.
    psram_task_create(wspr_self_task, "wspr_self", 11264, NULL, 3, tskNO_AFFINITY);
    ESP_LOGI(TAG, "self-spotting ready (wsprnet.org query, %d s poll)", POLL_INTERVAL_S);
}
