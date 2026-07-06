#include "sd_archive.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>     // fsync
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"

#include "bsp/m5stack_tab5.h"   // bsp_sdcard_init / bsp_sdcard_deinit

#include "diag_log.h"
#include "adif_log.h"
#include "config_io.h"
#include "ui.h"
#include "psram_task.h"

static const char *TAG = "sd_arch";

#define SD_MOUNT_POINT   "/sdcard"
#define SD_DIR           "/sdcard/qmx-panadapter"
#define SD_LOG_PATH      "/sdcard/qmx-panadapter/qmx-log.txt"
#define SD_LOG_PATH_1    "/sdcard/qmx-panadapter/qmx-log.1.txt"
#define SD_ADIF_PATH     "/sdcard/qmx-panadapter/qso.adi"
#define SD_CONFIG_PATH   "/sdcard/qmx-panadapter/qmx-config.txt"

#define SD_LOG_MAX_BYTES (5 * 1024 * 1024)   // rotate qmx-log.txt at 5 MB
#define PROBE_MS          10000               // mount-probe cadence when no card
#define WORK_MS           3000                // mirror cadence while mounted
#define DIAG_CHUNK        4096                // diag flush copy buffer

static volatile bool s_mounted      = false;
static volatile bool s_adif_dirty   = true;   // mirror once on first mount
static volatile bool s_config_dirty = true;

// Diag-log mirror state (owned by the archive task).
static uint64_t s_diag_cursor = 0;            // position in diag_log_total() space
static FILE    *s_log_file    = NULL;
static size_t   s_log_bytes   = 0;            // bytes in the current qmx-log.txt

// Serializes all FatFs access to the card between the archive task and the web
// server (CONFIG_FATFS_FS_LOCK=0, so concurrent f_read/f_write on the shared
// volume would corrupt it). The task holds it only during its brief work
// burst; the web server holds it while streaming the SD log to a download.
static SemaphoreHandle_t s_sd_mutex = NULL;

// ---- helpers ---------------------------------------------------------------

static void ensure_dir(const char *path)
{
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: %s", path, strerror(errno));
    }
}

// Overwrite-copy a source file to a destination path. Returns false on any I/O
// error (treated by the caller as a possible card removal). Logs the copy so
// the (mirrored) diag log records exactly what was written and when.
static bool copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        // Source missing is not a card error (e.g. ADIF not created yet).
        ESP_LOGI(TAG, "skip %s (source not present)", dst);
        return true;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        ESP_LOGW(TAG, "open %s failed: %s", dst, strerror(errno));
        fclose(in);
        return false;
    }
    static char buf[2048];
    size_t n, total = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
        total += n;
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (ok) ESP_LOGI(TAG, "mirrored %s (%u bytes)", dst, (unsigned)total);
    return ok;
}

static bool mirror_config(void)
{
    size_t len = 0;
    char *text = config_io_export(&len);
    if (!text) return true;  // nothing to write (not a card error)
    FILE *f = fopen(SD_CONFIG_PATH, "wb");
    bool ok = false;
    if (f) {
        ok = (fwrite(text, 1, len, f) == len);
        if (fclose(f) != 0) ok = false;
        if (ok) ESP_LOGI(TAG, "mirrored %s (%u bytes)", SD_CONFIG_PATH, (unsigned)len);
    } else {
        ESP_LOGW(TAG, "open %s failed: %s", SD_CONFIG_PATH, strerror(errno));
    }
    free(text);
    return ok;
}

// Append all newly-captured diag bytes to qmx-log.txt, rotating at 5 MB.
// Returns false on a write error (possible card removal).
static bool mirror_diag(void)
{
    if (!s_log_file) return false;

    static char buf[DIAG_CHUNK];
    for (;;) {
        uint64_t next = s_diag_cursor;
        size_t got = diag_log_read_from(s_diag_cursor, buf, sizeof(buf), &next);
        if (got == 0) break;
        if (fwrite(buf, 1, got, s_log_file) != got) {
            ESP_LOGW(TAG, "diag write failed: %s", strerror(errno));
            return false;
        }
        s_diag_cursor = next;
        s_log_bytes  += got;

        if (s_log_bytes >= SD_LOG_MAX_BYTES) {
            // Rotate: qmx-log.txt -> qmx-log.1.txt, start fresh.
            fclose(s_log_file);
            s_log_file = NULL;
            remove(SD_LOG_PATH_1);
            rename(SD_LOG_PATH, SD_LOG_PATH_1);
            s_log_file = fopen(SD_LOG_PATH, "ab");
            s_log_bytes = 0;
            if (!s_log_file) {
                ESP_LOGW(TAG, "reopen %s after rotate failed: %s",
                         SD_LOG_PATH, strerror(errno));
                return false;
            }
            ESP_LOGI(TAG, "rotated diag log on SD");
        }
    }
    // fflush pushes the stdio buffer into FatFs, but FatFs only writes the
    // data + directory entry to the physical card on f_sync/f_close. Without
    // the fsync the file reads as empty/short if the card is pulled while the
    // log file is still held open. fileno()->fsync() maps to f_sync in the
    // FAT VFS.
    fflush(s_log_file);
    fsync(fileno(s_log_file));
    return true;
}

static void unmount(void)
{
    if (s_log_file) { fclose(s_log_file); s_log_file = NULL; }
    bsp_sdcard_deinit(SD_MOUNT_POINT);
    s_mounted = false;
    ui_set_sd_active(false);
    ESP_LOGW(TAG, "SD card unmounted");
}

// Attempt to mount a card and set up the mirror. Returns true on success.
static bool try_mount(void)
{
    size_t pre_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t pre_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_err_t err = bsp_sdcard_init((char *)SD_MOUNT_POINT, 2);
    if (err != ESP_OK) {
        // Leave the slot in a clean state so the next probe can retry (a failed
        // mount can leave the BSP's card handle dangling otherwise).
        bsp_sdcard_deinit(SD_MOUNT_POINT);
        return false;
    }

    size_t post_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t post_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "SDMMC mount heap cost: internal -%u B (was %u, now %u), PSRAM -%d B",
             (unsigned)(pre_i - post_i), (unsigned)pre_i, (unsigned)post_i,
             (int)(pre_p - post_p));

    ensure_dir(SD_DIR);

    // Open (append) the diag log. s_diag_cursor is NOT reset here: on the
    // first mount of the boot it's still 0, so read_from() (clamping to the
    // oldest retained byte) dumps the whole current ring; on a remount after a
    // card glitch it resumes where it left off, so we don't re-dump 5 MB.
    s_log_file = fopen(SD_LOG_PATH, "ab");
    if (!s_log_file) {
        ESP_LOGW(TAG, "open %s failed: %s — unmounting", SD_LOG_PATH, strerror(errno));
        bsp_sdcard_deinit(SD_MOUNT_POINT);
        return false;
    }
    long pos = ftell(s_log_file);
    s_log_bytes = (pos > 0) ? (size_t)pos : 0;

    s_adif_dirty = true;           // force a full mirror right after mounting
    s_config_dirty = true;
    s_mounted = true;
    ui_set_sd_active(true);
    ESP_LOGI(TAG, "SD card mounted, mirroring to %s", SD_DIR);
    return true;
}

// ---- task ------------------------------------------------------------------

static void sd_archive_task(void *arg)
{
    (void)arg;
    for (;;) {
        // Hold the SD mutex for the whole work burst so a concurrent web
        // download of the SD log can't interleave FatFs I/O with ours.
        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

        if (!s_mounted) {
            if (!try_mount()) {
                xSemaphoreGive(s_sd_mutex);
                vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
                continue;
            }
        }

        // Mirror diag (incremental), then ADIF/config if dirty. Any write
        // failure is taken as a card removal.
        bool ok = mirror_diag();

        if (ok && s_adif_dirty) {
            s_adif_dirty = false;
            if (!copy_file(adif_log_file_path(), SD_ADIF_PATH)) {
                s_adif_dirty = true;   // retry after remount
                ok = false;
            }
        }
        if (ok && s_config_dirty) {
            s_config_dirty = false;
            if (!mirror_config()) {
                s_config_dirty = true;
                ok = false;
            }
        }

        if (!ok) {
            unmount();
            xSemaphoreGive(s_sd_mutex);
            vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
            continue;
        }

        xSemaphoreGive(s_sd_mutex);
        vTaskDelay(pdMS_TO_TICKS(WORK_MS));
    }
}

// ---- public API ------------------------------------------------------------

void sd_archive_init(void)
{
    s_sd_mutex = xSemaphoreCreateMutex();
    psram_task_create(sd_archive_task, "sd_archive", 6144, NULL,
                       2 /* low priority */, 0);
}

bool sd_archive_is_mounted(void)        { return s_mounted; }
void sd_archive_mark_adif_dirty(void)   { s_adif_dirty = true; }
void sd_archive_mark_config_dirty(void) { s_config_dirty = true; }

const char *sd_archive_log_path(void)   { return SD_LOG_PATH; }

bool sd_archive_lock(uint32_t timeout_ms)
{
    if (!s_sd_mutex) return false;
    return xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

// Free/total space on the mounted card, for the resource-monitor overlay.
// Best-effort: takes sd_archive_lock() so this read-only query can't land
// mid-write against the archive task's own FatFs bursts, same as the QRZ/
// eQSL upload quiet-window pattern. Returns false if no card is mounted or
// the lock can't be acquired quickly (never blocks the caller waiting on a
// wedged card).
bool sd_archive_get_free_bytes(uint64_t *out_free, uint64_t *out_total)
{
    if (!s_mounted) return false;
    if (!sd_archive_lock(50)) return false;
    uint64_t total = 0, free_b = 0;
    esp_err_t err = esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b);
    sd_archive_unlock();
    if (err != ESP_OK) return false;
    if (out_free)  *out_free  = free_b;
    if (out_total) *out_total = total;
    return true;
}

void sd_archive_unlock(void)
{
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
}

