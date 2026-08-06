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
// What remains is a small shim: resolve a page from the embedded blob and write
// it to the SPIFFS page cache, which is still the hand-off to the renderer. That
// keeps reader_view.c unchanged, and the write happens on a background task
// rather than on the LVGL thread.

#include "reader_net.h"
#include "net/manual_embed.h"
#include "ui/reader_view.h"
#include "util/psram_task.h"

#include "esp_log.h"
#include "esp_spiffs.h"     // esp_spiffs_info(), for the cache-write failure path

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "reader_net";

#define PAGE_CACHE        "/spiffs/reader.md"
#define TOC_CACHE         "/spiffs/reader_toc.json"

static volatile bool s_busy = false;   // one page load at a time
static char s_job_path[96];
static bool s_job_toc;

static bool write_file(const char *path, const char *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot open %s for write", path); return false; }
    size_t w = fwrite(buf, 1, len, f);
    fflush(f);
    fclose(f);
    // A SHORT write is the failure mode of a full partition, and fwrite reports
    // it only through the count - so say so here rather than leaving the caller
    // to infer it.
    if (w != len) ESP_LOGW(TAG, "short write to %s: %u of %u bytes",
                           path, (unsigned)w, (unsigned)len);
    return w == len;
}

static void fetch_task(void *arg)
{
    (void)arg;

    if (s_job_toc) {
        const char *toc = NULL;
        size_t toclen = 0;
        if (manual_embed_get("toc.json", &toc, &toclen) &&
            write_file(TOC_CACHE, toc, toclen)) {
            reader_view_notify_toc_loaded();
        } else {
            ESP_LOGW(TAG, "contents list missing from the embedded manual");
        }
    }

    const char *data = NULL;
    size_t len = 0;
    // Two DIFFERENT failures used to share one message that asserted the first
    // one ("not in the embedded manual"), which sent a debugging session chasing
    // the blob while the real fault was the write. Keep them apart: a lying
    // diagnostic costs more than no diagnostic.
    if (!manual_embed_get(s_job_path, &data, &len)) {
        ESP_LOGW(TAG, "page '%s' is not in the embedded manual", s_job_path);
        reader_view_notify_status("Page not found in the built-in manual");
        reader_view_notify_loaded(true);
    } else if (!write_file(PAGE_CACHE, data, len)) {
        // Almost always the render cache partition, not the manual. Report the
        // free space with it - a short write on a full SPIFFS is otherwise
        // indistinguishable from a missing page.
        size_t total = 0, used = 0;
        if (esp_spiffs_info(NULL, &total, &used) != ESP_OK) total = used = 0;
        ESP_LOGW(TAG, "page '%s' found (%u B) but caching it to %s FAILED "
                 "- spiffs total=%u used=%u free=%u",
                 s_job_path, (unsigned)len, PAGE_CACHE,
                 (unsigned)total, (unsigned)used, (unsigned)(total - used));
        reader_view_notify_status("Could not cache the page (storage full?)");
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
