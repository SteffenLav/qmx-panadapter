// See update_check.h. Polls GitHub Releases (pre-releases count) with a static
// tab5.lav.dk/latest.json fallback, compares against the running firmware
// version, and pushes an "update available" banner to the Reader.

#include "update_check.h"
#include "ui/reader_view.h"
#include "wifi/wifi.h"
#include "net/webserver_ws.h"     // webserver_ws_set_paused
#include "util/psram_task.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "update_check";

#define GITHUB_RELEASES_URL "https://api.github.com/repos/SteffenLav/qmx-panadapter/releases?per_page=5"
#define LATEST_JSON_URL     "https://tab5.lav.dk/latest.json"
#define USER_AGENT          "qmx-panadapter-tab5"

#define RESP_MAX_BYTES      (48 * 1024)
#define FIRST_DELAY_MS      30000              // let WiFi/SNTP settle first
// 30 MINUTES, down from 6 hours. Six hours meant a unit left switched on before
// a release might not notice until the next day, which made the OTA offer close
// to useless - and untestable.
//
// Cheap because of what is actually fetched. MEASURED: latest.json is 139 BYTES
// and takes 0.09 s, so this is ~280 bytes an hour per device and 50 hits an hour
// across 25 devices. Five minutes was tried first and is unnecessary now that
// update_check_now() exists: anyone who has just read the announcement can force
// a check from the version number in the footer, so the timer only has to catch
// the people who have not.
#define CHECK_INTERVAL_MS   (30 * 60 * 1000)       // 30 min
// ⛔ THE GITHUB FALLBACK IS A DIFFERENT ANIMAL AND MUST NOT RUN AT THAT RATE.
// MEASURED: /releases?per_page=5 is 48,764 BYTES and takes 3.7 s - 350x the
// traffic, several seconds of a marginal WiFi link, and GitHub's unauthenticated
// limit is 60 requests/hour per IP. If latest.json ever went down, every device
// on a site would start pulling 48 KB every five minutes and would rate-limit
// themselves out. So it is tried at most this often, and the cheap check keeps
// running in between.
#define GITHUB_MIN_GAP_MS   (60 * 60 * 1000)       // 1 h
#define RETRY_WHEN_DOWN_MS  (10 * 60 * 1000)   // WiFi down: retry sooner

static SemaphoreHandle_t s_lock = NULL;
static char s_latest[24]  = {0};
static bool s_available   = false;
static volatile bool s_force = false;   // set by update_check_now()
static bool s_started     = false;

// ---- version compare -------------------------------------------------------

// Parse up to 4 dotted numeric components from a version like "v1.2.3" or
// "v1.2.3.1" or a git-describe "v1.2.3-4-gabc" (everything from '-' is ignored).
static void parse_ver(const char *v, int out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!v) return;
    const char *p = v;
    if (*p == 'v' || *p == 'V') p++;
    int i = 0;
    while (*p && i < 4) {
        if (*p == '-') break;           // git-describe suffix / pre-release tag
        if (isdigit((unsigned char)*p)) {
            char *end = NULL;
            out[i] = (int)strtol(p, &end, 10);
            p = end;
            i++;
            if (*p == '.') p++;
        } else {
            break;
        }
    }
}

// >0 if a is newer than b, <0 if older, 0 if equal (on the numeric prefix).
static int ver_cmp(const char *a, const char *b)
{
    int va[4], vb[4];
    parse_ver(a, va);
    parse_ver(b, vb);
    for (int i = 0; i < 4; i++) {
        if (va[i] != vb[i]) return va[i] - vb[i];
    }
    return 0;
}

// ---- HTTP ------------------------------------------------------------------

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

// GET url into buf (PSRAM). Returns HTTP status (or -1 on transport failure).
static int http_get(const char *url, char *buf, size_t cap, size_t *out_len)
{
    buf[0] = '\0';
    resp_buf_t ctx = { buf, 0, cap };
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 15000,
        .event_handler     = on_data,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    esp_http_client_set_header(client, "User-Agent", USER_AGENT);   // GitHub 403s without one
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    if (out_len) *out_len = ctx.len;
    return status;
}

// Parse the GitHub /releases array: newest non-draft tag (pre-releases count).
static bool parse_github(const char *json, char *tag_out, size_t tag_sz)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool found = false;
    if (cJSON_IsArray(root)) {
        int n = cJSON_GetArraySize(root);
        for (int i = 0; i < n; i++) {
            cJSON *rel = cJSON_GetArrayItem(root, i);
            cJSON *draft = cJSON_GetObjectItem(rel, "draft");
            if (cJSON_IsBool(draft) && cJSON_IsTrue(draft)) continue;
            cJSON *tag = cJSON_GetObjectItem(rel, "tag_name");
            if (cJSON_IsString(tag) && tag->valuestring) {
                strncpy(tag_out, tag->valuestring, tag_sz - 1);
                tag_out[tag_sz - 1] = '\0';
                found = true;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return found;
}

// Parse the fallback latest.json: { "version": "v1.2.3", ... }
static bool parse_latest_json(const char *json, char *tag_out, size_t tag_sz)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool found = false;
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(ver) && ver->valuestring) {
        strncpy(tag_out, ver->valuestring, tag_sz - 1);
        tag_out[tag_sz - 1] = '\0';
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

static void publish(const char *latest, bool newer)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(s_latest, latest ? latest : "", sizeof(s_latest) - 1);
    s_latest[sizeof(s_latest) - 1] = '\0';
    s_available = newer;
    if (s_lock) xSemaphoreGive(s_lock);
    reader_view_set_update_available(newer ? latest : "");
    // #218: the bottom bar is painted by status.c's 1 Hz refresh, which also
    // has to show live OTA progress - one writer for one label.
}

static bool do_check(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const char *cur = app ? app->version : "";

    char *buf = heap_caps_malloc(RESP_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGW(TAG, "OOM"); return false; }

    char tag[24] = {0};
    bool got = false;

    // latest.json (tab5.lav.dk) is checked FIRST, GitHub second - the reverse
    // of the original order. Both carry the same version string, so the result
    // is unchanged; what this buys is a usage signal: the site's ordinary web
    // logs now see one small request per device per check interval, which is
    // the only count of ACTIVE installations available (GitHub reports
    // downloads, not use). No identifying data is sent beyond what any HTTP
    // request inherently carries, and nothing is stored on the device.
    // DISCLOSED in the release notes + manual; see also the PSK Reporter
    // "Software in use" table, which counts on-air users of this firmware.
    // Keep the spectrum WebSocket off the link for the duration, the same
    // courtesy every other network path here pays (spots.c, the QRZ/eQSL/LoTW
    // uploads, the log and file-browser transfers). This board's esp_hosted link
    // is low-throughput and the ~10 fps WS stream saturates the uplink; a fetch
    // landing on top of it is the documented wedge-prone combination.
    //
    // This was MISSING until 2026-08-05 and had never mattered, because all
    // outbound TLS failed at RNG seeding - so this function could never actually
    // reach the network. Fixing TLS turned it into the one network consumer on
    // the board not following the house rule.
    // ⚠ The WS pause is NOT taken for latest.json. The house rule exists because
    // a big fetch landing on the ~10 fps spectrum stream is the documented
    // wedge-prone combination - but this fetch is 139 bytes and 0.09 s, and at
    // one every five minutes a stutter that often would be a worse bug than the
    // one the rule prevents. The GitHub fallback is 48 KB and DOES pause.
    size_t len = 0;
    int status = http_get(LATEST_JSON_URL, buf, RESP_MAX_BYTES, &len);
    if (status == 200 && parse_latest_json(buf, tag, sizeof(tag))) {
        got = true;
    } else {
        static int64_t s_last_github_us = 0;
        int64_t now = esp_timer_get_time();
        if (s_last_github_us &&
            (now - s_last_github_us) < (int64_t)GITHUB_MIN_GAP_MS * 1000) {
            ESP_LOGW(TAG, "latest.json failed (status=%d); GitHub tried %lld min ago "
                          "- waiting rather than hammering it", status,
                     (long long)((now - s_last_github_us) / 60000000));
        } else {
            ESP_LOGW(TAG, "latest.json check failed (status=%d) - trying GitHub", status);
            s_last_github_us = now;
            webserver_ws_set_paused(true);        // 48 KB: this one yields
            status = http_get(GITHUB_RELEASES_URL, buf, RESP_MAX_BYTES, &len);
            webserver_ws_set_paused(false);
            if (status == 200 && parse_github(buf, tag, sizeof(tag))) got = true;
        }
    }
    heap_caps_free(buf);

    if (!got) { ESP_LOGW(TAG, "no version info available"); return false; }

    int cmp = ver_cmp(tag, cur);
    ESP_LOGI(TAG, "latest=%s running=%s -> %s", tag, cur,
             cmp > 0 ? "UPDATE AVAILABLE" : "up to date");
    publish(tag, cmp > 0);
    return true;
}

static void check_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(FIRST_DELAY_MS));
    for (;;) {
        // wifi_is_connected() goes true at association, but DNS/DHCP can still
        // be unusable for a second or two - a check launched in that window
        // fails on BOTH URLs. Retry on the short interval instead of sleeping
        // the full 6 h, or one unlucky boot costs the whole day's check
        // (hardware-observed: both URLs failed at 36.8 s uptime while WiFi
        // came up at ~38.5 s; neighbouring boots succeeded at 37-38 s).
        bool ok = wifi_is_connected() && do_check();
        // Sleep in slices so a forced check does not wait out the interval.
        // Even at 5 minutes that is too long when the operator has just
        // published a release and wants to SEE the offer appear.
        int slept = 0, want = ok ? CHECK_INTERVAL_MS : RETRY_WHEN_DOWN_MS;
        while (slept < want && !s_force) { vTaskDelay(pdMS_TO_TICKS(500)); slept += 500; }
        if (s_force) { s_force = false; ESP_LOGI(TAG, "check forced"); }
    }
}

void update_check_now(void) { s_force = true; }

void update_check_start(void)
{
    if (s_started) return;
    s_started = true;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    psram_task_create(check_task, "update_chk", 6144, NULL, 3, tskNO_AFFINITY);
}

void update_check_get_latest(char *out, int out_sz)
{
    if (!out || out_sz <= 0) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(out, s_latest, out_sz - 1);
    out[out_sz - 1] = '\0';
    if (s_lock) xSemaphoreGive(s_lock);
}

bool update_check_available(void) { return s_available; }
