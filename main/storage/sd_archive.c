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
#include "esp_app_desc.h"   // esp_app_get_description() for the README version stamp

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
#define SD_LOTW_CERT_PATH "/sdcard/qmx-panadapter/lotw_cert.b64"
#define SD_LOTW_KEY_PATH  "/sdcard/qmx-panadapter/lotw_key.b64"
#define SD_README_PATH    "/sdcard/qmx-panadapter/README.txt"

// Source (SPIFFS) paths for the LoTW certificate + private key. Mirror of
// lotw_upload.c's CERT_PATH/KEY_PATH — kept here to avoid a cross-module getter
// for two stable, never-renamed paths (a compile check would be overkill).
#define SRC_LOTW_CERT    "/spiffs/lotw_cert.b64"
#define SRC_LOTW_KEY     "/spiffs/lotw_key.b64"

#define SD_LOG_MAX_BYTES (5 * 1024 * 1024)   // rotate qmx-log.txt at 5 MB
#define PROBE_MS          10000               // mount-probe cadence when no card
#define WORK_MS           3000                // mirror cadence while mounted
#define DIAG_CHUNK        4096                // diag flush copy buffer

static volatile bool s_mounted      = false;
static volatile bool s_adif_dirty   = true;   // mirror once on first mount
static volatile bool s_config_dirty = true;
static volatile bool s_lotw_dirty   = true;   // LoTW cert+key: mount + on import

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

// Write a self-describing README so someone who pops the card into a PC knows
// exactly what every file is — the card is meant to be a grab-and-go station
// backup / transfer medium (POTA/SOTA, no PC needed). Rewritten each mount so
// the version stamp stays current; tiny + one-shot, no measurable cost.
static void write_readme(void)
{
    FILE *f = fopen(SD_README_PATH, "wb");
    if (!f) { ESP_LOGW(TAG, "readme open failed: %s", strerror(errno)); return; }
    const char *fw = "";
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) fw = app->version;
    fprintf(f,
        "QMX Panadapter - station backup\r\n"
        "===============================\r\n"
        "Automatic mirror of your QMX Panadapter's data, written by the Tab5\r\n"
        "whenever this card is inserted. Grab the card to back up or move your\r\n"
        "whole station to another device - no PC required.\r\n"
        "\r\n"
        "Files in this folder (qmx-panadapter/):\r\n"
        "  qso.adi         Your QSO log (ADIF). Import into any logger, or\r\n"
        "                  upload to QRZ / eQSL / LoTW.\r\n"
        "  qmx-config.txt  All settings + memory channels (editable INI text).\r\n"
        "                  Restore a device via the web UI's 'Config' upload.\r\n"
        "  lotw_cert.b64   Your LoTW (TQSL) signing certificate and\r\n"
        "  lotw_key.b64    private key (base64 DER). Needed to sign QSOs for\r\n"
        "                  LoTW after moving to / restoring another device.\r\n"
        "  qmx-log.txt     Diagnostic log, newest session (rolling, for bug\r\n"
        "  qmx-log.1.txt   reports); .1 is the previous segment after rotation.\r\n"
        "\r\n"
        "*** CONTAINS CREDENTIALS ***\r\n"
        "qmx-config.txt stores your WiFi password and QRZ/eQSL logins in clear\r\n"
        "text, and lotw_key.b64 is your LoTW PRIVATE KEY. Keep this card as\r\n"
        "physically secure as you would a house key.\r\n"
        "\r\n"
        "Written by QMX Panadapter %s. Mirror is continuous while inserted.\r\n",
        fw);
    fclose(f);
    ESP_LOGI(TAG, "wrote %s", SD_README_PATH);
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
    s_lotw_dirty = true;
    write_readme();                // self-describing card (fresh version stamp)
    s_mounted = true;
    ui_set_sd_active(true);
    ESP_LOGI(TAG, "SD card mounted, mirroring to %s", SD_DIR);
    return true;
}

// ---- task ------------------------------------------------------------------

static void sd_archive_task(void *arg)
{
    (void)arg;
    // #51-adjacent soak instrumentation (2026-07-19): per-burst SPI write time
    // + a 30 s heartbeat so a WiFi wedge / SDIO-recovery event (both self-log)
    // or an FT8 dec collapse can be correlated against actual SD write activity.
    // The heap + FT8 dec impact is read off the existing per-slot ft8_test line.
    int64_t s_hb_last_us   = esp_timer_get_time();
    int     s_burst_max_ms = 0;
    int     s_burst_cnt    = 0;
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

        int64_t burst_t0 = esp_timer_get_time();
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
        // LoTW cert + private key (base64 DER). Small + rarely change (only on
        // cert import), so copied on mount and on sd_archive_mark_lotw_dirty().
        // A missing source (no cert imported yet) is a no-op, not a card error.
        if (ok && s_lotw_dirty) {
            s_lotw_dirty = false;
            if (!copy_file(SRC_LOTW_CERT, SD_LOTW_CERT_PATH) ||
                !copy_file(SRC_LOTW_KEY,  SD_LOTW_KEY_PATH)) {
                s_lotw_dirty = true;
                ok = false;
            }
        }

        if (!ok) {
            unmount();
            xSemaphoreGive(s_sd_mutex);
            vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
            continue;
        }

        int burst_ms = (int)((esp_timer_get_time() - burst_t0) / 1000);
        if (burst_ms > s_burst_max_ms) s_burst_max_ms = burst_ms;
        s_burst_cnt++;
        xSemaphoreGive(s_sd_mutex);

        // 30 s heartbeat: proves SD is alive + shows how hard it's writing, so
        // any concurrent WiFi/FT8 disturbance in the log has an SD reference.
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_hb_last_us >= 30000000) {
            ESP_LOGI(TAG, "heartbeat: mounted diag_cursor=%llu bursts=%d max_burst=%dms",
                     (unsigned long long)s_diag_cursor, s_burst_cnt, s_burst_max_ms);
            s_hb_last_us   = now_us;
            s_burst_max_ms = 0;
            s_burst_cnt    = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(WORK_MS));
    }
}

// ---- public API ------------------------------------------------------------

void sd_archive_init(void)
{
#if SD_ARCHIVE_DISABLED
    ESP_LOGW(TAG, "SD auto-archive soft-disabled (see SD_ARCHIVE_DISABLED in "
                  "sd_archive.h) - shared-SDMMC/WiFi wedge not yet root-caused");
    return;
#endif
    s_sd_mutex = xSemaphoreCreateMutex();
    psram_task_create(sd_archive_task, "sd_archive", 6144, NULL,
                       2 /* low priority */, 0);
}

bool sd_archive_is_mounted(void)        { return s_mounted; }
void sd_archive_mark_adif_dirty(void)   { s_adif_dirty = true; }
void sd_archive_mark_config_dirty(void) { s_config_dirty = true; }
void sd_archive_mark_lotw_dirty(void)   { s_lotw_dirty = true; }

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

