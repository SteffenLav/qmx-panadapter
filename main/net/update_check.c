// See update_check.h. Polls GitHub Releases (pre-releases count) with a static
// tab5.lav.dk/latest.json fallback, compares against the running firmware
// version, and pushes an "update available" banner to the Reader.

#include "update_check.h"
#include "ui/reader_view.h"
#include "wifi/wifi.h"
#include "net/webserver_ws.h"     // webserver_ws_set_paused
#include "util/psram_task.h"
#include "net/ota_update.h"
#include "net/net_quiet.h"
#include "storage/settings.h"

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

// An AUTOMATIC download waits for both of these; see the guards in check_task().
// 5 minutes clears WiFi bring-up, USB enumeration and the first FT8 slots. The
// heap figure is a margin, not a measured cliff: the verify has been seen to die
// with 10 KB free, and a healthy idle device here sits at 50 KB+, so 32 KB says
// "there is room to spare" without waiting for a quiet moment that may never
// come. A held-off download simply tries again at the next check.
#define AUTODL_MIN_UPTIME_MS      (5 * 60 * 1000)
#define AUTODL_MIN_INTERNAL_FREE  (32 * 1024)

static SemaphoreHandle_t s_lock = NULL;
static char s_latest[24]  = {0};
static bool s_available   = false;
static volatile bool s_force = false;   // set by update_check_now()
// A check takes SECONDS (TLS to GitHub), and until this existed nothing
// could tell 'we asked, wait' from 'we asked, and the answer is no'. Both
// screens rendered the previous verdict half a second after the press, so a
// tester who had just read the release announcement was told "Up to date -
// you are running v1.9.3" and only saw the offer appear seconds later, by
// which time it read as the button having failed (Michael KZ4LY, Samuel
// W7STF, 2026-08-26). Set at the REQUEST, not when the task wakes, or the
// same gap reopens at 500 ms wide.
static volatile bool s_checking = false;
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
        // Not while an update is downloading: do_check()'s GitHub fallback
        // pulls 48 KB over TLS, and the thing it would be checking for is
        // already in flight. See net_quiet.h.
        s_checking = true;
        bool ok = wifi_is_connected() && !net_quiet_active() && do_check();
        s_checking = false;

        // #239: fetch it quietly, so the operator is only ever asked the one
        // question that matters - "restart into it?" - instead of starting a
        // download and then waiting on it. This DOWNLOADS ONLY. Applying still
        // needs a deliberate press, which is the standing rule at the top of
        // ota_update.h and the reason it exists: a warm reset with the radio
        // attached is the #74 trigger, so an unattended RESTART would kill
        // someone's QMX. An unattended download cannot.
        //
        // Opt-out, because it is not free: 3.3 MB saturates this link
        // (~12.7 KB/s measured) for over a minute and costs real money on the
        // phone hotspot a POTA operator is using. ota_update_start() refuses
        // by itself while transmitting or mid-QSO, so those need no gate here.
        //
        // Retried on the next check rather than immediately: gating on "not
        // RUNNING and not DONE" means a failure is re-attempted at the normal
        // CHECK_INTERVAL_MS cadence (30 min), which is the right amount of
        // persistence for something nobody asked for.
        if (ok && s_available) {
            qmx_settings_t cfg;
            settings_load_all(&cfg);
            ota_state_t st = ota_update_get_state(NULL, NULL, 0);
            // ⛔ TWO GUARDS, AND ONLY ON THE AUTOMATIC PATH.
            //
            // Steve N9SZ, 2026-08-26: booting his Tab5 started an automatic
            // update, it "appeared to download fully", the device rebooted by
            // itself and came back on the OLD version. Twice. Then the same
            // update, started BY HAND minutes later, worked.
            //
            // That shape is explained by where esp_https_ota_finish() sits: it
            // both verifies the image AND sets the boot partition, so a reset
            // during the verify leaves the old image bootable and loses the
            // download - precisely "it downloaded and I am still on the old
            // version". And the verify is the memory-hungry end of the job:
            // this file's own instrumentation records it dying inside segment 0
            // with the internal heap at 10 KB free / 4 KB largest.
            //
            // The automatic download fired 30 s after boot, which is the worst
            // moment on this board - WiFi bring-up has just taken ~100 KB of
            // internal RAM, USB is enumerating, and the DMA pool is at its
            // lowest. So: wait until the device has settled, and do not start
            // when internal RAM is already tight. A missed window costs 30
            // minutes; a watchdog reset costs the download and confuses the
            // operator about which version they are running.
            //
            // ⚠ The MANUAL path keeps neither guard, deliberately. Someone who
            // presses the button is watching, can retry, and must not be told
            // "no" by a heuristic - and it was the manual path that worked for
            // Steve.
            //
            // ⚠ HYPOTHESIS UNTIL HIS LOG ARRIVES. It fits the reported
            // behaviour and the failure mode this code already documents, but
            // the crash record (#117) from his device is what would confirm it.
            int64_t up_ms = esp_timer_get_time() / 1000;
            size_t  ifree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (cfg.ota_autodl && st != OTA_RUNNING && st != OTA_DONE &&
                up_ms < AUTODL_MIN_UPTIME_MS) {
                ESP_LOGW(TAG, "auto-download held: only %lld s since boot, waiting for the "
                              "device to settle (a manual update is unaffected)",
                         (long long)(up_ms / 1000));
            } else if (cfg.ota_autodl && st != OTA_RUNNING && st != OTA_DONE &&
                       ifree < AUTODL_MIN_INTERNAL_FREE) {
                ESP_LOGW(TAG, "auto-download held: internal heap %u B, want %u B - the image "
                              "verify needs room at the END of the download",
                         (unsigned)ifree, (unsigned)AUTODL_MIN_INTERNAL_FREE);
            } else if (cfg.ota_autodl && st != OTA_RUNNING && st != OTA_DONE) {
                char aurl[192], oerr[96];
                update_check_get_asset_url(aurl, sizeof(aurl));
                if (aurl[0] && ota_update_start(aurl, oerr, sizeof(oerr)))
                    ESP_LOGW(TAG, "auto-download of %s started (quiet)", s_latest);
                else
                    ESP_LOGI(TAG, "auto-download held off: %s", oerr);
            }
        }
        // Sleep in slices so a forced check does not wait out the interval.
        // Even at 5 minutes that is too long when the operator has just
        // published a release and wants to SEE the offer appear.
        int slept = 0, want = ok ? CHECK_INTERVAL_MS : RETRY_WHEN_DOWN_MS;
        while (slept < want && !s_force) { vTaskDelay(pdMS_TO_TICKS(500)); slept += 500; }
        if (s_force) { s_force = false; ESP_LOGI(TAG, "check forced"); }
    }
}

void update_check_now(void) { s_checking = true; s_force = true; }

bool update_check_in_progress(void) { return s_checking; }

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

#define RELEASE_ASSET_URL_FMT     "https://github.com/SteffenLav/qmx-panadapter/releases/download/%s/qmx_panadapter.bin"

void update_check_get_asset_url(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_available && s_latest[0])
        snprintf(out, out_sz, RELEASE_ASSET_URL_FMT, s_latest);
    if (s_lock) xSemaphoreGive(s_lock);
}
