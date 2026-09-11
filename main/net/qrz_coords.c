// QRZ.com Callbook XML lookup -- real per-station coordinates for RBN/DX-
// cluster spots. Ported from the sibling rbn_monitor project's qrz_client.cpp
// (see qrz_coords.h). QRZ's Callsign Lookup service has no static API key
// (confirmed against the current XML Interface spec -- only the unrelated
// Logbook/QSO-sync API uses one, see adif/qrz_upload.c): auth is
// username+password, exchanged for a session key that's then passed on every
// subsequent lookup. Endpoint and parameter shape (semicolon-separated, not
// '&') per that spec:
//   Login:  https://xml.qrz.com/xml/current/?username=U;password=P;agent=A
//   Lookup: https://xml.qrz.com/xml/current/?s=SESSIONKEY;callsign=CALL
// Response is XML; rather than pull in a full XML parser for two or three
// flat tags, this hand-rolls simple <tag>...</tag> substring extraction (see
// extract_tag) -- fine for QRZ's shallow, predictable response shape.

#include "qrz_coords.h"
#include "storage/settings.h"
#include "util/psram_task.h"
#include "net/net_quiet.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "qrz_coords";
static const char *QRZ_URL = "https://xml.qrz.com/xml/current/";
#define QRZ_RX_BUF_SIZE 4096
#define QRZ_CALL_LEN    16   // matches spot_t.call (net/spots.h)
#define QRZ_USER_LEN    40   // matches settings_get_qrz_lookup_creds()
#define QRZ_PASS_LEN    40
#define CACHE_SIZE      300  // distinct callsigns worth remembering - generous for a session's worth of RBN/cluster activity
#define PENDING_CAP     32   // in-flight lookup requests queued at once

static char s_username[QRZ_USER_LEN] = {0};
static char s_password[QRZ_PASS_LEN] = {0};
static char s_session_key[64] = {0};
static volatile bool s_logged_in = false;
// Set once QRZ explicitly rejects the current username/password (as opposed
// to a network hiccup) -- stops all further login attempts until the stored
// credentials change. Without this, a wrong password combined with a steady
// stream of new callsigns (always refilling the pending queue) would retry
// login every few seconds indefinitely, which is both pointless (same wrong
// password every time) and actively harmful: QRZ answers repeated bad logins
// with an escalating temporary block, so hammering it just keeps extending
// that block further into the future.
static volatile bool s_credentials_rejected = false;

static SemaphoreHandle_t s_mutex;

typedef struct {
    char  call[QRZ_CALL_LEN];
    float lat, lon;
    bool  has_coords;
    bool  used;
} cache_entry_t;

typedef struct {
    char call[QRZ_CALL_LEN];
    bool used;
} pending_entry_t;

// PSRAM, not internal DRAM - see CLAUDE.md's rule that anything under 16 KB
// otherwise lands in the scarce internal heap. 300 * ~24 B is trivial for
// PSRAM and would be a needless internal-RAM cost otherwise.
static EXT_RAM_BSS_ATTR cache_entry_t s_cache[CACHE_SIZE];
static int s_cache_next = 0; // ring-buffer eviction once full

static EXT_RAM_BSS_ATTR pending_entry_t s_pending[PENDING_CAP];

typedef enum { CACHE_NOT_CACHED, CACHE_NO_COORDS, CACHE_HAS_COORDS } cache_result_t;

// Drops a skimmer's trailing flag suffix ("-#", "-1", ...) and uppercases, so
// "DK3WW-#" and a later "DK3WW-1" share one cache entry. Portable-prefix
// calls ("EA8/DF4UE") are passed through as-is -- QRZ's lookup understands
// compound callsigns itself.
static void normalize_call(const char *raw, char *out, size_t out_len)
{
    size_t oi = 0;
    for (size_t i = 0; raw[i] != '\0' && raw[i] != '-' && oi + 1 < out_len; i++) {
        out[oi++] = (char)toupper((unsigned char)raw[i]);
    }
    out[oi] = '\0';
}

static bool contains_ci(const char *haystack, const char *needle)
{
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn == 0 || nn > hn) return false;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for (; j < nn; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) break;
        }
        if (j == nn) return true;
    }
    return false;
}

static void url_encode(const char *in, char *out, size_t out_len)
{
    size_t oi = 0;
    for (size_t i = 0; in[i] != '\0' && oi + 4 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[oi++] = (char)c;
        } else {
            snprintf(out + oi, 4, "%%%02X", c);
            oi += 3;
        }
    }
    out[oi] = '\0';
}

// Finds the first <tag>...</tag> anywhere in `xml` and copies its content
// (bounded, always null-terminated) into `out`. No nesting/attribute support
// -- QRZ's response tags are flat and simple.
static bool extract_tag(const char *xml, const char *tag, char *out, size_t out_len)
{
    char open_tag[32], close_tag[32];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    const char *start = strstr(xml, open_tag);
    if (!start) return false;
    start += strlen(open_tag);
    const char *end = strstr(start, close_tag);
    if (!end || end < start) return false;
    size_t n = (size_t)(end - start);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

static cache_result_t cache_find_locked(const char *norm_call, float *lat, float *lon)
{
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].call, norm_call) == 0) {
            if (s_cache[i].has_coords) {
                *lat = s_cache[i].lat;
                *lon = s_cache[i].lon;
                return CACHE_HAS_COORDS;
            }
            return CACHE_NO_COORDS;
        }
    }
    return CACHE_NOT_CACHED;
}

static void cache_store(const char *norm_call, bool has_coords, float lat, float lon)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].call, norm_call) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        slot = s_cache_next;
        s_cache_next = (s_cache_next + 1) % CACHE_SIZE;
    }
    snprintf(s_cache[slot].call, sizeof(s_cache[slot].call), "%s", norm_call);
    s_cache[slot].has_coords = has_coords;
    s_cache[slot].lat = lat;
    s_cache[slot].lon = lon;
    s_cache[slot].used = true;
    xSemaphoreGive(s_mutex);
}

// Dedups against whatever's already queued -- a hot callsign spotted
// repeatedly before its first lookup completes only gets one entry.
static void enqueue_pending(const char *norm_call)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool already = false;
    int free_slot = -1;
    for (int i = 0; i < PENDING_CAP; i++) {
        if (s_pending[i].used && strcmp(s_pending[i].call, norm_call) == 0) { already = true; break; }
        if (!s_pending[i].used && free_slot < 0) free_slot = i;
    }
    if (!already && free_slot >= 0) {
        snprintf(s_pending[free_slot].call, sizeof(s_pending[free_slot].call), "%s", norm_call);
        s_pending[free_slot].used = true;
    }
    // If the queue's full, this callsign just doesn't get looked up this
    // round -- it'll try again next time it's spotted (harmless, RBN/cluster
    // repeat active calls within seconds).
    xSemaphoreGive(s_mutex);
}

// Pops (and removes) the first queued entry, if any. Whether the lookup that
// follows succeeds or fails, the slot stays free either way -- a failure
// just means this callsign gets re-enqueued next time it's spotted, rather
// than the queue head getting stuck retrying forever.
static bool pop_pending(char *out, size_t out_len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < PENDING_CAP; i++) {
        if (s_pending[i].used) {
            snprintf(out, out_len, "%s", s_pending[i].call);
            s_pending[i].used = false;
            xSemaphoreGive(s_mutex);
            return true;
        }
    }
    xSemaphoreGive(s_mutex);
    return false;
}

typedef struct { char *buf; int len; } rx_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        rx_ctx_t *ctx = (rx_ctx_t *)evt->user_data;
        int room = QRZ_RX_BUF_SIZE - 1 - ctx->len;
        int n = evt->data_len < room ? evt->data_len : room;
        if (n > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, n);
            ctx->len += n;
        }
    }
    return ESP_OK;
}

// Shared by do_login/do_lookup_once -- GET `url`, return the null-terminated
// response body in rx_buf, or false on any transport/HTTP error (status !=
// 200 included).
static bool http_get(const char *url, char *rx_buf)
{
    rx_ctx_t ctx = {rx_buf, 0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d", status);
        return false;
    }
    rx_buf[ctx.len] = '\0';
    return true;
}

static bool do_login(char *rx_buf)
{
    if (s_username[0] == '\0') return false;
    char enc_user[QRZ_USER_LEN * 3 + 1];
    char enc_pass[QRZ_PASS_LEN * 3 + 1];
    url_encode(s_username, enc_user, sizeof(enc_user));
    url_encode(s_password, enc_pass, sizeof(enc_pass));

    char url[320];
    snprintf(url, sizeof(url), "%s?username=%s;password=%s;agent=qmx-panadapter",
             QRZ_URL, enc_user, enc_pass);

    if (!http_get(url, rx_buf)) return false;

    char key[64];
    if (!extract_tag(rx_buf, "Key", key, sizeof(key))) {
        char err_text[96];
        if (extract_tag(rx_buf, "Error", err_text, sizeof(err_text))) {
            // "incorrect"/"blocked"/"suspend" mean the credentials themselves
            // are the problem -- retrying won't help and only extends QRZ's
            // block further, so stop entirely instead of backing off and
            // trying again later.
            bool hard_reject = contains_ci(err_text, "incorrect") || contains_ci(err_text, "blocked") ||
                                contains_ci(err_text, "suspend");
            if (hard_reject) {
                ESP_LOGE(TAG, "QRZ login rejected, stopping until credentials change: %s", err_text);
            } else {
                ESP_LOGW(TAG, "QRZ login failed: %s", err_text);
            }
            s_credentials_rejected = hard_reject;
        } else {
            ESP_LOGW(TAG, "QRZ login failed (no session key in response)");
        }
        return false;
    }
    s_credentials_rejected = false;
    snprintf(s_session_key, sizeof(s_session_key), "%s", key);
    s_logged_in = true;
    ESP_LOGI(TAG, "Logged in to QRZ.com");
    return true;
}

// Returns true if the *request* itself completed (so the caller knows
// whether to cache the outcome), regardless of whether coordinates were
// actually found -- *found tells that. Returns false only on a transport
// error or a session-related error (caller should re-login and retry).
static bool do_lookup_once(const char *norm_call, char *rx_buf, float *lat, float *lon, bool *found)
{
    char enc_call[QRZ_CALL_LEN * 3 + 1];
    url_encode(norm_call, enc_call, sizeof(enc_call));

    char url[256];
    snprintf(url, sizeof(url), "%s?s=%s;callsign=%s", QRZ_URL, s_session_key, enc_call);

    *found = false;
    if (!http_get(url, rx_buf)) return false;

    char err_text[96];
    if (extract_tag(rx_buf, "Error", err_text, sizeof(err_text))) {
        bool session_related = strstr(err_text, "ession") != NULL; // "Session Timeout", "Invalid session key", ...
        if (session_related) {
            ESP_LOGI(TAG, "QRZ session expired, will re-login");
            return false;
        }
        ESP_LOGD(TAG, "QRZ lookup for %s: %s", norm_call, err_text);
        return true; // genuine "not found" -- cache as no-coords
    }

    char lat_str[16], lon_str[16];
    if (extract_tag(rx_buf, "lat", lat_str, sizeof(lat_str)) && extract_tag(rx_buf, "lon", lon_str, sizeof(lon_str))) {
        *lat = (float)atof(lat_str);
        *lon = (float)atof(lon_str);
        *found = true;
    }
    return true;
}

// Re-reads the stored credentials each pass and picks up a change made in
// the settings drawer while running - no external "set_credentials" call
// needed, unlike the rbn_monitor original. A changed username/password gets
// a fresh login attempt and clears any prior rejection.
static void refresh_credentials(void)
{
    char user[QRZ_USER_LEN], pass[QRZ_PASS_LEN];
    settings_get_qrz_lookup_creds(user, pass);
    if (strcmp(user, s_username) != 0 || strcmp(pass, s_password) != 0) {
        snprintf(s_username, sizeof(s_username), "%s", user);
        snprintf(s_password, sizeof(s_password), "%s", pass);
        s_logged_in = false;
        s_credentials_rejected = false; // give the new credentials a fresh chance
    }
}

static void qrz_task(void *arg)
{
    (void)arg;
    char *rx_buf = heap_caps_malloc(QRZ_RX_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        vTaskDelete(NULL);
        return;
    }

    int64_t next_login_attempt_ms = 0;
    int64_t next_cred_check_ms = 0;
    while (true) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now >= next_cred_check_ms) {
            refresh_credentials();
            next_cred_check_ms = now + 5000; // cheap enough to poll every 5s
        }

        if (net_quiet_active()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        // These lookups exist only to place an RBN skimmer on the spot map, so
        // they follow the map's own opt-in (settings.h, spotmap_en). Checked
        // every pass, so the drawer switch applies live.
        if (!settings_get_spotmap_en()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        if (s_username[0] == '\0' || s_credentials_rejected) {
            // Not configured, or QRZ explicitly rejected these credentials
            // (see s_credentials_rejected's comment) -- idle without even
            // draining the pending queue, so whatever was waiting is still
            // there once the operator fixes the credentials.
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        char call[QRZ_CALL_LEN];
        if (!pop_pending(call, sizeof(call))) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        if (!s_logged_in) {
            if (now < next_login_attempt_ms) {
                // Recent login failure, still backing off -- this call's
                // lookup is simply skipped this round (it'll be re-enqueued
                // next time it's spotted).
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            }
            if (!do_login(rx_buf)) {
                // 30s, not 5s -- QRZ answers repeated bad logins with an
                // escalating temporary block (see s_credentials_rejected),
                // so even transient-looking failures get a real backoff
                // rather than a near-immediate retry.
                next_login_attempt_ms = now + 30000;
                continue;
            }
        }

        float lat = 0, lon = 0;
        bool found = false;
        bool ok = do_lookup_once(call, rx_buf, &lat, &lon, &found);
        if (!ok) {
            s_logged_in = false;
            if (now >= next_login_attempt_ms && do_login(rx_buf)) {
                ok = do_lookup_once(call, rx_buf, &lat, &lon, &found);
            } else {
                next_login_attempt_ms = now + 30000;
            }
        }
        if (ok) {
            cache_store(call, found, lat, lon);
            if (found) {
                ESP_LOGI(TAG, "%s -> %.4f, %.4f", call, (double)lat, (double)lon);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250)); // be polite to QRZ's server between requests
    }
}

void qrz_coords_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    psram_task_create(qrz_task, "qrz_coords", 6144, NULL, 3, tskNO_AFFINITY);
}

bool qrz_coords_lookup_cached(const char *call, float *lat_out, float *lon_out)
{
    if (!s_mutex || s_username[0] == '\0') return false;
    char norm[QRZ_CALL_LEN];
    normalize_call(call, norm, sizeof(norm));
    if (norm[0] == '\0') return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cache_result_t res = cache_find_locked(norm, lat_out, lon_out);
    xSemaphoreGive(s_mutex);

    if (res == CACHE_NOT_CACHED) enqueue_pending(norm);
    return res == CACHE_HAS_COORDS;
}

bool qrz_coords_is_logged_in(void) { return s_logged_in; }
bool qrz_coords_credentials_rejected(void) { return s_credentials_rejected; }
