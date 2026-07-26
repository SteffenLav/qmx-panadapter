// See reader_net.h. Background HTTPS fetch of the docs markdown + TOC into
// SPIFFS caches, mirroring the esp_http_client + esp_crt_bundle pattern used by
// qrz_upload.c / lotw_upload.c. Also mirrors the whole manual to a microSD card
// on demand ("Save offline") and reads from the card when offline.

#include "reader_net.h"
#include "ui/reader_view.h"
#include "wifi/wifi.h"
#include "util/psram_task.h"
#include "storage/sd_archive.h"
#include "dsp/dsp.h"
#include "net/webserver_ws.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static const char *TAG = "reader_net";

// Public base for the raw source markdown, mirrored alongside the built site by
// mkdocs_reader_export.py (see plan §1d).
#define DOCS_BASE         "https://tab5.lav.dk/md/"
#define PAGE_CACHE        "/spiffs/reader.md"
#define TOC_CACHE         "/spiffs/reader_toc.json"
#define SD_MANUAL_DIR     "/sdcard/qmx-panadapter/manual"

// WAF-friendliness (the tab5.lav.dk host runs behavioural rate-limiting that
// temporarily blocks bursty/retrying clients with 429/454/455 "security
// incident" pages - observed live 2026-07-25; the block self-expires after
// ~a minute of silence). Two accommodations keep the Reader under its radar:
//  - a cooldown after any failed fetch, so tap-retries can't re-arm the block
//  - pacing between the Save-offline bulk requests (see save_task)
#define FETCH_COOLDOWN_MS   60000
#define SAVE_PACING_MS      1200
static int64_t s_cooldown_until_ms = 0;   // esp_timer epoch; 0 = no cooldown

static bool in_cooldown(void)
{
    return s_cooldown_until_ms != 0 &&
           (esp_timer_get_time() / 1000) < s_cooldown_until_ms;
}

static void arm_cooldown(void)
{
    s_cooldown_until_ms = esp_timer_get_time() / 1000 + FETCH_COOLDOWN_MS;
}
#define DOCS_MAX_BYTES    (96 * 1024)
#define USER_AGENT        "qmx-panadapter-tab5"

static volatile bool s_busy = false;   // one network op (fetch OR save) at a time
static char s_job_path[96];
static bool s_job_toc;

// Accumulate the response body into a caller-provided buffer.
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

// GET url into buf. Returns body length on HTTP 200 (>0), else 0.
static size_t http_get_buf(const char *url, char *buf, size_t cap)
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
    if (!client) return 0;
    esp_http_client_set_header(client, "User-Agent", USER_AGENT);
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    if (err == ESP_OK && status == 200 && ctx.len > 0) return ctx.len;
    ESP_LOGW(TAG, "GET %s: err=%s status=%d len=%u", url, esp_err_to_name(err), status, (unsigned)ctx.len);
    return 0;
}

static bool write_file(const char *path, const char *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot open %s for write", path); return false; }
    size_t w = fwrite(buf, 1, len, f);
    fflush(f);
    fclose(f);
    return w == len;
}

// GET url -> cache_path (PSRAM scratch). Returns true only on 200 + written.
static bool fetch_url_to_file(const char *url, const char *cache_path)
{
    char *body = heap_caps_malloc(DOCS_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { ESP_LOGW(TAG, "OOM"); return false; }
    size_t n = http_get_buf(url, body, DOCS_MAX_BYTES);
    bool ok = (n > 0) && write_file(cache_path, body, n);
    if (ok) ESP_LOGI(TAG, "fetched %u bytes -> %s", (unsigned)n, cache_path);
    heap_caps_free(body);
    return ok;
}

// Copy a page from the SD manual mirror into the page cache (offline read).
static bool sd_page_to_cache(const char *rel)
{
    if (!sd_archive_is_mounted()) return false;
    char sdpath[256];
    snprintf(sdpath, sizeof(sdpath), "%s/%s", SD_MANUAL_DIR, rel);
    if (!sd_archive_lock(3000)) return false;
    bool ok = false;
    char *body = heap_caps_malloc(DOCS_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE *in = body ? fopen(sdpath, "rb") : NULL;
    if (in) {
        size_t n = fread(body, 1, DOCS_MAX_BYTES - 1, in);
        fclose(in);
        ok = write_file(PAGE_CACHE, body, n);
        if (ok) ESP_LOGI(TAG, "offline: %s (SD) -> cache", rel);
    }
    if (body) heap_caps_free(body);
    sd_archive_unlock();
    return ok;
}

static void fetch_task(void *arg)
{
    (void)arg;

    if (!wifi_is_connected()) {
        // WiFi may simply not be up YET: on the first manual-open right after
        // boot the STA is often still associating / waiting on DHCP, so an
        // instant bail showed "No documentation cached - connect to WiFi" even
        // on a WiFi-configured unit, only curable by a reboot (Roy KI0ER field
        // report). Wait briefly for the link to come up while the reader is
        // still on screen, then fetch normally - no reboot or re-swipe needed.
        const int MAX_WAIT_S = 30;
        int waited = 0;
        while (!wifi_is_connected() && waited < MAX_WAIT_S && reader_view_is_active()) {
            if (waited == 0) reader_view_notify_status("Waiting for WiFi...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            waited++;
        }
        if (!wifi_is_connected()) {
            // Still offline (or the operator left the page): fall back to the SD
            // manual mirror, else whatever's cached in SPIFFS.
            if (sd_page_to_cache(s_job_path)) reader_view_notify_loaded(false);
            else                              reader_view_notify_loaded(true);
            s_busy = false;
            vTaskDelete(NULL);
            return;
        }
        // Link came up during the wait — fall through and download normally.
    }

    if (in_cooldown()) {
        // A recent fetch failed; hammering the server again would extend the
        // WAF's temporary block. Serve what we have and say why.
        reader_view_notify_status("Server busy - wait a minute, then retry");
        if (sd_page_to_cache(s_job_path)) reader_view_notify_loaded(false);
        else                              reader_view_notify_loaded(true);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    reader_view_notify_status("Downloading...");

    if (s_job_toc) {
        char url[192];
        snprintf(url, sizeof(url), "%stoc.json", DOCS_BASE);
        if (fetch_url_to_file(url, TOC_CACHE)) reader_view_notify_toc_loaded();
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s", DOCS_BASE, s_job_path);
    bool ok = fetch_url_to_file(url, PAGE_CACHE);

    if (ok) {
        s_cooldown_until_ms = 0;
        reader_view_notify_status("");
        reader_view_notify_loaded(false);
    } else if (sd_page_to_cache(s_job_path)) {   // online but fetch failed -> SD
        arm_cooldown();
        reader_view_notify_status("Offline copy (SD)");
        reader_view_notify_loaded(false);
    } else {
        // WiFi is up but the server refused/failed - NOT a WiFi problem
        // (the old "check WiFi" wording sent operators down the wrong path).
        arm_cooldown();
        reader_view_notify_status("tab5.lav.dk not responding - retry in 1 min");
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
    if (!psram_task_create(fetch_task, "reader_net", 6144, NULL, 4, tskNO_AFFINITY)) {
        s_busy = false;
    }
}

void reader_net_load_index(void)
{
    reader_net_fetch("index.md", true);
}

// ============================ SD offline save ============================

// Create every parent directory of a full file path (mkdir each component).
static void mkdirs_for_file(const char *filepath)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", filepath);
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '/') { *q = '\0'; mkdir(tmp, 0777); *q = '/'; }
    }
}

// Write one page buffer to the SD manual mirror at <rel> (under the SD lock).
static bool sd_write_page(const char *rel, const char *buf, size_t len)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SD_MANUAL_DIR, rel);
    if (!sd_archive_lock(5000)) return false;
    mkdirs_for_file(path);
    bool ok = write_file(path, buf, len);
    sd_archive_unlock();
    return ok;
}

// Download the whole manual (toc + every page) to the SD card. Mirrors the
// upload path's WiFi-coexistence safeguards (pause the WS stream + transfer
// quiet) since SD writes during WiFi traffic are this board's most wedge-prone
// combination; SD writes are also serialised against the archive task via
// sd_archive_lock (held only for the write, never across a WiFi fetch).
static void save_task(void *arg)
{
    (void)arg;

    if (!wifi_is_connected())      { reader_view_notify_status("Connect WiFi to save offline"); s_busy = false; vTaskDelete(NULL); return; }
    if (!sd_archive_is_mounted())  { reader_view_notify_status("No SD card");                    s_busy = false; vTaskDelete(NULL); return; }
    if (in_cooldown())             { reader_view_notify_status("Server busy - wait a minute, then retry"); s_busy = false; vTaskDelete(NULL); return; }

    webserver_ws_set_paused(true);
    dsp_set_transfer_quiet(true);

    char *buf = heap_caps_malloc(DOCS_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        dsp_set_transfer_quiet(false); webserver_ws_set_paused(false);
        reader_view_notify_status("Out of memory"); s_busy = false; vTaskDelete(NULL); return;
    }

    int saved = 0, failed = 0;

    // TOC first (also gives us the page list).
    reader_view_notify_status("Saving: contents...");
    char url[256];
    snprintf(url, sizeof(url), "%stoc.json", DOCS_BASE);
    size_t toclen = http_get_buf(url, buf, DOCS_MAX_BYTES);
    cJSON *root = NULL;
    if (toclen > 0) {
        sd_write_page("toc.json", buf, toclen);
        root = cJSON_Parse(buf);   // dups strings; buf reusable after this
    }

    if (root) {
        cJSON *pages = cJSON_GetObjectItem(root, "pages");
        int n = cJSON_IsArray(pages) ? cJSON_GetArraySize(pages) : 0;
        // count real pages (entries with a path) for the progress denominator
        int total = 0;
        for (int i = 0; i < n; i++) {
            cJSON *pa = cJSON_GetObjectItem(cJSON_GetArrayItem(pages, i), "path");
            if (cJSON_IsString(pa) && pa->valuestring && pa->valuestring[0]) total++;
        }
        int idx = 0;
        for (int i = 0; i < n; i++) {
            cJSON *pa = cJSON_GetObjectItem(cJSON_GetArrayItem(pages, i), "path");
            if (!cJSON_IsString(pa) || !pa->valuestring || !pa->valuestring[0]) continue;
            idx++;
            char st[48]; snprintf(st, sizeof(st), "Saving %d/%d to SD...", idx, total);
            reader_view_notify_status(st);
            snprintf(url, sizeof(url), "%s%s", DOCS_BASE, pa->valuestring);
            size_t len = http_get_buf(url, buf, DOCS_MAX_BYTES);
            if (len > 0 && sd_write_page(pa->valuestring, buf, len)) saved++;
            else failed++;
            // Pace the bulk download: ~19 back-to-back requests look like a
            // bot burst to the host's rate limiter and get the IP temporarily
            // blocked mid-save. ~1 request/second passes as a human.
            vTaskDelay(pdMS_TO_TICKS(SAVE_PACING_MS));
        }
        cJSON_Delete(root);
    } else {
        failed = 1;
    }

    heap_caps_free(buf);
    dsp_set_transfer_quiet(false);
    webserver_ws_set_paused(false);

    if (failed > 0) arm_cooldown();
    ESP_LOGI(TAG, "offline save: %d saved, %d failed", saved, failed);
    reader_view_notify_status("");                 // no "Saved N pages" line
    reader_view_notify_saved(saved > 0 && failed == 0);

    s_busy = false;
    vTaskDelete(NULL);
}

// Recursively delete a directory tree (bounded depth - the manual mirror is
// at most base/guide|reference|build/page.md). Used only for SD_MANUAL_DIR.
static void rm_tree(const char *path, int depth)
{
    if (depth > 4) return;
    // Deleting entries while iterating with readdir() is undefined in FatFs
    // (the DIR object caches directory sectors that unlink rewrites). Safe
    // pattern instead: take the FIRST entry, close the iterator, delete it,
    // reopen - bounded by the entry count, and the manual mirror holds ~20
    // files, so the re-scans are cheap.
    for (int guard = 0; guard < 256; guard++) {
        DIR *d = opendir(path);
        if (!d) return;
        char name[64] = "";
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            snprintf(name, sizeof(name), "%.63s", e->d_name);
            break;
        }
        closedir(d);
        if (!name[0]) break;   // directory is empty
        char sub[192];
        snprintf(sub, sizeof(sub), "%s/%s", path, name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) rm_tree(sub, depth + 1);
        else if (unlink(sub) != 0) break;   // can't delete: bail, don't spin
    }
    rmdir(path);
}

// Erase every stored copy of the manual: the SPIFFS page/TOC caches and the
// microSD offline mirror. Recovery action for a cache poisoned by a captive
// portal (hotel WiFi login pages served instead of the real markdown get
// cached and then render as garbage until overwritten). Long-press the
// drawer's User Manual button to trigger.
static void erase_task(void *arg)
{
    (void)arg;

    unlink(PAGE_CACHE);
    unlink(TOC_CACHE);

    if (sd_archive_is_mounted()) {
        if (sd_archive_lock(5000)) {
            rm_tree(SD_MANUAL_DIR, 0);
            sd_archive_unlock();
        } else {
            ESP_LOGW(TAG, "erase: SD busy - manual mirror not deleted");
        }
    }

    ESP_LOGI(TAG, "manual erased (SPIFFS caches + SD mirror)");
    reader_view_notify_saved_reset();  // nothing failed - there's just no saved copy now
    s_busy = false;
    vTaskDelete(NULL);
}

void reader_net_erase_all(void)
{
    if (s_busy) return;
    s_busy = true;
    if (!psram_task_create(erase_task, "reader_erase", 6144, NULL, 4, tskNO_AFFINITY)) {
        s_busy = false;
    }
}

void reader_net_save_offline(void)
{
    if (s_busy) return;
    s_busy = true;
    // 8 KB stack: TLS + cJSON + FatFs. PSRAM stack (non-realtime).
    if (!psram_task_create(save_task, "reader_save", 8192, NULL, 4, tskNO_AFFINITY)) {
        s_busy = false;
    }
}
