#include "sd_health.h"
#include "sd_archive.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "sd_health";

#define SD_MOUNT "/sdcard"
#define READ_CHUNK      4096
#define SD_BLOCK         512      /* one SD block - the driver's real unit */
#define DMA_ALIGN         64      /* cache line; keeps the driver's fast path */
#define MAX_DEPTH          6
#define PATTERN_BYTES  65536
#define PATTERN_PERIOD   512      /* fixed, so the compare does not depend on
                                   * whatever chunk size we managed to get */
#define YIELD_EVERY   (256*1024)
#define READ_ATTEMPTS      4      /* a starved read is retried, not blamed */
#define BACKOFF_MS       400

/* ⛔ THERE IS NO 24 KB REQUIREMENT. DO NOT PUT A THRESHOLD BACK HERE.
 *
 * The first version refused to start unless MALLOC_CAP_DMA had 24576 B free.
 * That number was invented - "leaves room for whatever else is running" - and
 * it made the feature unusable: on 2026-10-03 it refused with 1687 B free
 * while sd_archive was, at that same moment, mirroring the log to that same
 * card without trouble. The operator's words: "how can we implement a feature
 * that is not able to run?"
 *
 * MEASURED, in the driver's own source: esp-idf 5.4.4
 * components/sdmmc/sdmmc_cmd.c:477 (read) and :611 (write) allocate
 * block_size - 512 B - for ONE transaction and free it immediately. That is
 * the whole requirement, and it is transient.
 *
 * The pool this guard watched sits at a few kilobytes once WiFi is up, and has
 * been measured at ~400 B (sd_archive.c). Any threshold in tens of kilobytes
 * is therefore not caution, it is a permanent refusal.
 *
 * What replaces it: take whatever buffer the heap will give, and when a read
 * does fail, look at the pool AT THAT MOMENT to decide whether the Tab5 or the
 * card is at fault - then back off and try the file again. */

static sd_health_report_t s_rep;
static volatile bool      s_cancel;
static TaskHandle_t       s_task;

void sd_health_get(sd_health_report_t *out)
{
    if (out) *out = s_rep;
}

void sd_health_cancel(void) { s_cancel = true; }

static bool suspect_name(const char *n)
{
    size_t l = strlen(n);
    if (l >= 4 && strcasecmp(n + l - 4, ".CHK") == 0) return true;
    if (strncasecmp(n, "FOUND.", 6) == 0) return true;
    if (strncasecmp(n, "FSCK", 4) == 0)   return true;
    return false;
}

/* Below one aligned block the driver cannot move anything at all, so a failure
 * here is the Tab5's and not the card's. */
static bool dma_starved(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < (SD_BLOCK + DMA_ALIGN);
}

/* Take what the heap will give. A DMA-capable, aligned buffer lets the driver
 * read straight into it with no per-block bounce at all; a plain one still
 * works, with the driver's own 512 B bounce behind it. Either beats refusing. */
static uint8_t *alloc_read_buf(size_t *size_out, bool *dma_out)
{
    for (size_t n = READ_CHUNK; n >= SD_BLOCK; n /= 2) {
        uint8_t *p = heap_caps_aligned_alloc(DMA_ALIGN, n,
                                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (p) { *size_out = n; *dma_out = true; return p; }
    }
    uint8_t *p = malloc(READ_CHUNK);
    if (p) { *size_out = READ_CHUNK; *dma_out = false; return p; }
    return NULL;
}

/* Returns true if the file was read end to end. On false, *starved_out says
 * which kind of failure it was: true = the Tab5 never had the memory, which is
 * NOT evidence about the card; false = the read failed with memory available,
 * which is. */
static bool read_whole_file(const char *path, uint8_t *buf, size_t chunk,
                            uint64_t *bytes_out, bool *starved_out)
{
    for (int attempt = 0; attempt < READ_ATTEMPTS; attempt++) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            if (!dma_starved()) return false;
            vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS));
            continue;
        }
        size_t n;
        uint64_t got = 0, since_yield = 0;
        bool ok = true;
        while ((n = fread(buf, 1, chunk, f)) > 0) {
            got         += n;
            since_yield += n;
            if (since_yield >= YIELD_EVERY) { since_yield = 0; vTaskDelay(1); }
            if (s_cancel) break;
        }
        if (ferror(f)) ok = false;
        fclose(f);
        if (ok || s_cancel) { *bytes_out += got; return true; }

        /* ⛔ errno CANNOT TELL THESE APART. The driver's ESP_ERR_NO_MEM comes
         * back through FatFs as FR_DISK_ERR and reaches fread as a plain EIO -
         * byte for byte what a bad sector gives. So ask the pool instead. */
        if (!dma_starved()) return false;       /* memory was there: the card failed */
        ESP_LOGW(TAG, "read backed off (no DMA memory), retrying: %s", path);
        vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS));
    }
    *starved_out = true;
    return false;
}

static void walk(const char *dir, uint8_t *buf, size_t chunk, int depth)
{
    if (depth > MAX_DEPTH || s_cancel) return;
    /* Same rule as a file read: a directory that will not open while the DMA
     * pool is empty is the Tab5, not the card. Retry before concluding. */
    DIR *d = NULL;
    for (int attempt = 0; attempt < READ_ATTEMPTS; attempt++) {
        d = opendir(dir);
        if (d || !dma_starved()) break;
        vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS));
    }
    if (!d) {
        /* Failing to OPEN a directory is not a bad file - it is the check not
         * working. Counting it as a read error made the verdict blame the card
         * for the Tab5 being out of memory. */
        if (depth == 0) s_rep.could_not_read = true;
        else            s_rep.read_errors++;
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (s_cancel) break;
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
            (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;

        char path[320];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (suspect_name(e->d_name)) {
            s_rep.suspect_names++;
            ESP_LOGW(TAG, "lost-chain leftover: %s", path);
        }

        struct stat st;
        if (stat(path, &st) != 0) { s_rep.read_errors++; continue; }

        if (S_ISDIR(st.st_mode)) {
            s_rep.dirs_seen++;
            walk(path, buf, chunk, depth + 1);
        } else {
            s_rep.files_seen++;
            snprintf(s_rep.current, sizeof(s_rep.current), "%s", path);
            bool starved = false;
            if (!read_whole_file(path, buf, chunk, &s_rep.bytes_read, &starved)) {
                if (starved) {
                    s_rep.unchecked_files++;
                    ESP_LOGW(TAG, "NOT CHECKED (Tab5 out of DMA memory): %s", path);
                } else {
                    s_rep.read_errors++;
                    ESP_LOGE(TAG, "UNREADABLE: %s", path);
                }
            }
        }
    }
    closedir(d);
}

static void write_verify(uint8_t *buf, size_t chunk)
{
    const char *path = SD_MOUNT "/qmx-health.tmp";
    s_rep.write_verify_run = true;
    s_rep.write_verify_ok  = false;

    /* Period is fixed at one block, so the compare below does not depend on
     * which chunk size the allocator happened to give us. */
    for (size_t i = 0; i < chunk; i++)
        buf[i] = (uint8_t)((i % PATTERN_PERIOD) * 7 + 13);

    FILE *f = NULL;
    for (int attempt = 0; attempt < READ_ATTEMPTS; attempt++) {
        f = fopen(path, "wb");
        if (f || !dma_starved()) break;
        vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS));
    }
    if (!f) {
        /* Inconclusive, NOT a failure of the card: write_verify_done stays
         * false and the verdict must not accuse anything. */
        ESP_LOGE(TAG, "write-verify: cannot create the test file - the check could "
                      "not run, which is NOT a verdict on the card");
        return;
    }
    size_t written = 0;
    while (written < PATTERN_BYTES) {
        if (fwrite(buf, 1, chunk, f) != chunk) {
            ESP_LOGE(TAG, "write-verify: the write itself failed - inconclusive");
            fclose(f); unlink(path); return;
        }
        written += chunk;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    /* Past here the data IS on the card, so a mismatch is the card's doing. */
    s_rep.write_verify_done = true;

    f = fopen(path, "rb");
    if (!f) { s_rep.write_verify_done = false; unlink(path); return; }
    uint8_t cmp[64];
    bool ok = true;
    size_t total = 0;
    while (total < PATTERN_BYTES && ok) {
        size_t n = fread(cmp, 1, sizeof(cmp), f);
        if (n == 0) { ok = false; break; }
        for (size_t i = 0; i < n; i++) {
            size_t off = (total + i) % PATTERN_PERIOD;
            if (cmp[i] != (uint8_t)(off * 7 + 13)) { ok = false; break; }
        }
        total += n;
    }
    if (total != PATTERN_BYTES) ok = false;
    fclose(f);
    unlink(path);
    s_rep.write_verify_ok = ok;
    if (!ok) ESP_LOGE(TAG, "write-verify FAILED - this card does not store what it accepts");
}

static void verdict(void)
{
    /* ⛔ NEVER ACCUSE A CARD THE CHECK COULD NOT TEST. The first version said
     * "replace it" when the Tab5 had run out of DMA memory and nothing had been
     * read or written at all - 0 files, 0 bytes, and a write that never
     * happened. An instrument that reports its own failure as the subject's
     * failure is worse than no instrument. */
    if (s_rep.could_not_read || (s_rep.files_seen == 0 && !s_rep.write_verify_done)) {
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "The check could not run - the card could not be read just now, most "
                 "likely because memory was short. This says nothing about the card. "
                 "Try again when the Tab5 is less busy.");
        return;
    }
    if (!s_rep.write_verify_done) {
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "Read %lu file(s) with %lu error(s). The write test could not be "
                 "started, so nothing is known about whether the card stores what it "
                 "accepts.",
                 (unsigned long)s_rep.files_seen, (unsigned long)s_rep.read_errors);
        return;
    }
    /* ⛔ A FILE THE TAB5 WAS TOO BUSY TO READ IS NOT A FILE THE CARD LOST.
     * It is reported, it is counted, and it is kept out of every sentence that
     * passes judgement on the card. */
    if (s_rep.unchecked_files && !s_rep.read_errors && s_rep.write_verify_ok) {
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "Everything the Tab5 could get to reads and writes correctly. "
                 "%lu file(s) were skipped because the Tab5 ran out of memory "
                 "mid-check, not because of the card - run it again when it is "
                 "quieter for a complete answer.",
                 (unsigned long)s_rep.unchecked_files);
        return;
    }
    if (s_rep.read_errors && !s_rep.write_verify_ok)
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "This card is failing: %lu file(s) unreadable and the data written back "
                 "did not match. Copy what you want off it and replace it.",
                 (unsigned long)s_rep.read_errors);
    else if (!s_rep.write_verify_ok)
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "The card stored a write and gave back something different. That is how "
                 "a counterfeit or worn-out card behaves. Replace it.");
    else if (s_rep.read_errors)
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "%lu file(s) would not read. The card still writes correctly, so copy "
                 "what you need off it and reformat.",
                 (unsigned long)s_rep.read_errors);
    else if (s_rep.suspect_names)
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "Everything reads and writes correctly. The %lu recovered fragment(s) are "
                 "leftovers from an interrupted write - safe to delete.",
                 (unsigned long)s_rep.suspect_names);
    else
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "Healthy: every file read back and the write test matched.");
}

static void health_task(void *arg)
{
    (void)arg;
    size_t chunk = 0;
    bool   dma   = false;
    uint8_t *buf = alloc_read_buf(&chunk, &dma);
    if (!buf) {
        s_rep.state = SD_HEALTH_FAILED;
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "Not enough memory to run the check - not even 512 bytes. "
                 "This says nothing about the card; try again when the Tab5 is quieter.");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "read buffer: %u B, %s", (unsigned)chunk,
             dma ? "DMA-capable (no per-block bounce)" : "plain (driver bounces 512 B)");

    uint64_t total = 0, freeb = 0;
    if (sd_archive_get_free_bytes(&freeb, &total)) {
        s_rep.total_bytes = total;
        s_rep.free_bytes  = freeb;
    }

    ESP_LOGW(TAG, "card check starting - reading every file, this takes a while");
    walk(SD_MOUNT, buf, chunk, 0);
    if (!s_cancel) write_verify(buf, chunk);
    verdict();
    s_rep.current[0] = 0;
    s_rep.state = SD_HEALTH_DONE;
    ESP_LOGW(TAG, "card check done: %lu files, %llu bytes read, %lu read error(s), "
                  "%lu not checked (no memory), %lu fragment(s), write-verify %s",
             (unsigned long)s_rep.files_seen, (unsigned long long)s_rep.bytes_read,
             (unsigned long)s_rep.read_errors, (unsigned long)s_rep.unchecked_files,
             (unsigned long)s_rep.suspect_names,
             s_rep.write_verify_ok ? "OK" : "FAILED");

    free(buf);
    s_task = NULL;
    vTaskDelete(NULL);
}

bool sd_health_start(void)
{
    if (s_task) return false;
    if (!sd_archive_is_mounted()) {
        memset(&s_rep, 0, sizeof(s_rep));
        s_rep.state = SD_HEALTH_FAILED;
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "No card is mounted, so there is nothing to check.");
        return false;
    }
    /* No memory threshold here, deliberately - see the block at the top of this
     * file. The check starts, and handles starvation where it actually happens. */
    ESP_LOGI(TAG, "card check starting with DMA free %u, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    memset(&s_rep, 0, sizeof(s_rep));
    s_rep.state = SD_HEALTH_RUNNING;
    s_cancel = false;
    if (xTaskCreatePinnedToCore(health_task, "sd_health", 6144, NULL,
                                tskIDLE_PRIORITY + 1, &s_task, 1) != pdPASS) {
        s_task = NULL;
        s_rep.state = SD_HEALTH_FAILED;
        snprintf(s_rep.verdict, sizeof(s_rep.verdict), "Could not start the check task.");
        return false;
    }
    return true;
}
