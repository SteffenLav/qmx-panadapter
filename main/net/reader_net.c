// See reader_net.h. Feeds the on-device docs Reader (ui/reader_view.c) from the
// user manual built into the firmware binary (net/manual_embed.c).
//
// This module used to download each page over HTTPS on demand and optionally
// mirror the whole manual to the SD card. All of that is gone: the manual now
// ships inside the firmware, so there is nothing to download, nothing to cache
// that can go stale, and no SD card involved. Deleted along with it - and worth
// knowing WHY, so none of it gets reintroduced:
//
//   * the WiFi-up wait, because a page view no longer needs a link at all
//   * the rate-limit cooldown for tab5.lav.dk, whose WAF temporarily blocks
//     bursty clients (429/454/455) - a bulk download of 18 pages was exactly the
//     pattern it blocks, and it hit us repeatedly during development
//   * the SD manual mirror and its two-stage "download now, write on next boot"
//     dance, which existed only because SD writes are unreliable once WiFi is up
//     (see storage/sd_archive.c)
//
// What remains is a small shim: resolve a page from the embedded blob and tell
// the renderer it is available. The work happens on a background task rather
// than on the LVGL thread.
//
// The SPIFFS page cache is GONE too (2026-08-18). This comment used to call it
// "still the hand-off to the renderer", and that had been untrue since
// 2026-08-06: reader_view.c calls manual_embed_get() itself (see the comment at
// its load_page()) and never reads /spiffs/reader.md. So every page view wrote
// up to ~30 KB into a 934 KB filesystem that NOTHING read back - on a partition
// shared with the ADIF log, the LoTW certificate and private key, and the diag
// log, where running out of space is a real failure (it is what produced the
// "Could not cache the page" reports, and the write's own failure path had
// grown an esp_spiffs_info() call to explain itself). Deleting the write is
// what makes those bytes and that churn go away. Do not reintroduce it: if a
// hand-off is ever needed again, pass the blob pointer, not a copy through
// flash.

#include "reader_net.h"
#include "net/manual_embed.h"
#include "ui/reader_view.h"
#include "util/psram_task.h"

#include "esp_log.h"
// No esp_spiffs.h any more: nothing here touches the filesystem except the
// one-time unlink of the caches an older firmware left behind.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *TAG = "reader_net";

#define PAGE_CACHE        "/spiffs/reader.md"
#define TOC_CACHE         "/spiffs/reader_toc.json"

static volatile bool s_busy = false;   // one page load at a time
static char s_job_path[96];
static bool s_job_toc;

static void fetch_task(void *arg)
{
    (void)arg;

    if (s_job_toc) {
        const char *toc = NULL;
        size_t toclen = 0;
        if (manual_embed_get("toc.json", &toc, &toclen)) {
            reader_view_notify_toc_loaded();
        } else {
            ESP_LOGW(TAG, "contents list missing from the embedded manual");
        }
    }

    const char *data = NULL;
    size_t len = 0;
    // reader_view reads the blob itself, so resolving the page here is only a
    // check that it EXISTS - hence the deliberately unused data/len. Keeping the
    // lookup means a missing page is still reported as a missing page rather
    // than as a blank screen.
    if (!manual_embed_get(s_job_path, &data, &len)) {
        ESP_LOGW(TAG, "page '%s' is not in the embedded manual", s_job_path);
        reader_view_notify_status("Page not found in the built-in manual");
        // true keeps the status text above; notify_loaded(false) clears it.
        reader_view_notify_loaded(true);
    } else {
        reader_view_notify_status("");
        reader_view_notify_loaded(false);
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
    if (!psram_task_create(fetch_task, "reader_net", 4096, NULL, 4, tskNO_AFFINITY)) {
        s_busy = false;
    }
}

void reader_net_load_index(void)
{
    reader_net_fetch("index.md", true);
}

void reader_net_purge_legacy_caches(void)
{
    // An upgrade from any build before 2026-08-18 arrives with reader.md (up to
    // ~30 KB) and reader_toc.json still on /spiffs, written by a cache nothing
    // read. Reclaim them ONCE at boot: on a partition this small, and shared
    // with the QSO log and the LoTW private key, tens of KB is worth having
    // back, and stale files also cost GC work forever. unlink() on a file that
    // is not there is a no-op, so later boots pay nothing and this needs no
    // "have I done it" flag in NVS.
    struct stat st;
    size_t freed = 0;
    if (stat(PAGE_CACHE, &st) == 0) freed += (size_t)st.st_size;
    if (stat(TOC_CACHE,  &st) == 0) freed += (size_t)st.st_size;
    unlink(PAGE_CACHE);
    unlink(TOC_CACHE);
    if (freed) {
        ESP_LOGI(TAG, "reclaimed %u B of legacy reader cache from /spiffs", (unsigned)freed);
    }
}

void reader_net_erase_all(void)
{
    // Only the two render caches are left to clear, and they are rebuilt from the
    // embedded manual the next time a page is opened - so this is now a harmless
    // "reset the reader" rather than the cache-poisoning recovery it once was
    // (a captive portal can no longer substitute anything: nothing is fetched).
    // Two small unlinks, so no task needed.
    unlink(PAGE_CACHE);
    unlink(TOC_CACHE);
    ESP_LOGI(TAG, "reader caches cleared (manual itself is in the firmware)");
}
