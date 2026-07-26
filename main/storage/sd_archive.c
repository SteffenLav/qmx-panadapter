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
#include "settings.h"   // wifi_enabled: the WiFi-aware mirroring gate
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
// Set once the boot-window mount retries have finished, mounted or not.
static volatile bool s_boot_probe_done = false;
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
        "WHEN IS THIS WRITTEN?\r\n"
        "  WiFi off  - continuously, the whole time the card is inserted.\r\n"
        "  WiFi on   - once per start-up. Your log/config/certificate are all\r\n"
        "              backed up within a few seconds of switching on, but QSOs\r\n"
        "              made later in that session only reach the card at the next\r\n"
        "              start-up. (WiFi and this card cannot both use the shared\r\n"
        "              bus reliably, so the Tab5 takes the backup first, then\r\n"
        "              leaves the card alone. The bottom-bar SD dot is GREEN while\r\n"
        "              mirroring continuously, YELLOW once the backup is done.)\r\n"
        "  Insert the card BEFORE switching on - a card pushed in later is not\r\n"
        "  picked up until the next start-up.\r\n"
        "\r\n"
        "Written by QMX Panadapter %s.\r\n",
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

static void sd_fail_diag(const char *where, int err);   // TEMP DIAGNOSTIC, see below

// Consecutive failed write bursts tolerated before concluding the card is gone.
// 5 bursts x WORK_MS = ~15 s of retrying on the already-open handle.
#define SD_WRITE_FAIL_UNMOUNT 5
static int s_consec_write_fail = 0;

// Quick mount retries inside the boot window, while DMA memory is still plentiful.
#define SD_BOOT_MOUNT_TRIES   5
#define SD_BOOT_MOUNT_GAP_MS  150

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
            sd_fail_diag("diagwrite", errno);
            // MUST clear the stream error indicator, or every later fwrite on
            // this FILE* returns short WITHOUT touching the card - the retry
            // above would then be a no-op and a transient fault would look
            // permanent. s_diag_cursor is deliberately not advanced, so the
            // next burst re-writes exactly this chunk.
            clearerr(s_log_file);
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

// True once we have deliberately stopped touching the card for this session.
static bool s_parked = false;

// Cleanly stop mirroring and release the card, leaving the completed backup on
// it. Used when WiFi is (or is becoming) active: live mirroring provably cannot
// survive that (hardware-verified 2026-07-26 - with WiFi never started the same
// card mirrored flawlessly for 230 s; with WiFi on it dies within 10-140 s and
// can never be remounted, because the MALLOC_CAP_DMA pool is ~400 B by then).
//
// Parking deliberately is strictly better than being killed: a teardown mid-write
// with the diag log still open is exactly how FAT directory entries get
// corrupted, which is the most likely origin of the garbage entries seen on the
// operator's card. fsync before fclose is mandatory (FatFs only commits the data
// + directory entry on f_sync/f_close).
static void park_snapshot(void)
{
    // Close the diag log but deliberately KEEP THE CARD MOUNTED.
    //
    // Closing the file removes the corruption path (a teardown mid-write with the
    // log held open is how FAT directory entries get damaged) and stops the
    // continuous background writes that collide with WiFi.
    //
    // Do NOT bsp_sdcard_deinit() here. On-demand consumers all gate on
    // sd_archive_is_mounted() - the Reader's "Save offline" (reader_net.c), the
    // /files web browser and the SD log download (filebrowser.c) - and a remount
    // is impossible once WiFi is up (the MALLOC_CAP_DMA pool is ~400 B, so every
    // attempt fails 0x101). Unmounting would therefore make all three report
    // "no SD card" with a card physically inserted, and nothing could ever bring
    // it back. Keeping the boot mount alive is what preserves the POTA workflow:
    // fetch the manual over WiFi at home and "Save offline" to the card.
    if (s_log_file) {
        fflush(s_log_file);
        fsync(fileno(s_log_file));
        fclose(s_log_file);
        s_log_file = NULL;
    }
    s_parked = true;                        // stop background mirroring only
    ui_set_sd_state(UI_SD_SNAPSHOT_ONLY);   // yellow: card usable, not live-mirroring
    ESP_LOGW(TAG, "backup snapshot complete - background mirroring off while WiFi is "
                  "on; card stays mounted for Save-offline / web file browser");
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
// === TEMP DIAGNOSTIC (2026-07-26) - remove once the WiFi/SD question is closed.
// Every observed SD failure reports 0x101 == ESP_ERR_NO_MEM, and the card dies
// 226 ms after WiFi obtains an IP, so the working hypothesis is DMA-capable
// internal-RAM exhaustion rather than a bus/pin conflict (pins are disjoint:
// SD SPI 39/42/43/44 vs WiFi SDIO 8-13). This prints the numbers that confirm
// or kill that hypothesis - "free" alone is not enough, a contiguous
// DMA-capable block is what the SPI/FatFs layer actually needs.
//
// CAPPED at 3 calls on purpose: heap_caps_get_largest_free_block() walks the
// heap with interrupts off, which is what caused the FT4 cyan flash, and
// try_mount() retries every 10 s. Never let this run unbounded on that path.
#define SD_FAIL_DIAG_MAX 8
static void sd_fail_diag(const char *where, int err)
{
    static int n = 0;
    if (n++ >= SD_FAIL_DIAG_MAX) return;
    ESP_LOGW(TAG, "SDFAIL[%s] err=0x%x | INT free=%u lblk=%u | DMA free=%u lblk=%u",
             where, err,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

static bool try_mount(void)
{
    size_t pre_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t pre_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_err_t err = bsp_sdcard_init((char *)SD_MOUNT_POINT, 2);
    if (err != ESP_OK) {
        sd_fail_diag("mount", (int)err);
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

    // The mount attempt at ~4.1 s is the ONLY one that ever runs while the
    // MALLOC_CAP_DMA pool is still large (113 KB here; ~400 B from 14 s onward
    // once WiFi has taken it). Every later attempt on the PROBE_MS cadence fails
    // with 0x101 ESP_ERR_NO_MEM no matter how healthy the card is. Card init is
    // also intermittently returning 0x108 ESP_ERR_INVALID_RESPONSE - observed on
    // 2 of 5 boots, at both 20 MHz and 10 MHz - so a single attempt means one bad
    // roll of the dice costs the card for the whole session.
    //
    // Retry a few times inside that window instead. This does not need to know
    // WHY init is flaky; it only needs the good window to be used properly.
    // ~0.6 s worst case, all before WiFi starts (sd_archive_init runs well ahead
    // of panadapter_wifi_start in app_main).
    for (int i = 0; i < SD_BOOT_MOUNT_TRIES && !s_mounted; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(SD_BOOT_MOUNT_GAP_MS));
        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
        bool got = try_mount();
        xSemaphoreGive(s_sd_mutex);
        if (got) {
            if (i) ESP_LOGW(TAG, "SD mounted on boot attempt %d/%d",
                            i + 1, SD_BOOT_MOUNT_TRIES);
            break;
        }
        ESP_LOGW(TAG, "boot mount attempt %d/%d failed", i + 1, SD_BOOT_MOUNT_TRIES);
    }
    // Release app_main, which flushes the Reader's staged offline manual to the
    // card before it starts WiFi (see sd_archive_wait_mounted).
    s_boot_probe_done = true;

    for (;;) {
        // WiFi-aware gating. Read the user's on/off INTENT live (not
        // wifi_is_connected) - the interference comes from esp_hosted/SDIO being
        // active at all, which includes scanning and reconnect attempts.
        qmx_settings_t gs;
        settings_load_all(&gs);
        const bool wifi_on = gs.wifi_enabled;

        if (s_parked) {
            // Background mirroring stays off for the rest of the session, but the
            // card remains MOUNTED and fully usable on demand (Save offline, web
            // file browser, log download). Resuming the background mirror after
            // WiFi is switched off is deliberately not attempted here - it is an
            // untested path, and a reboot with WiFi off gives the verified
            // continuous-mirroring behaviour.
            static bool s_noted_wifi_off = false;
            if (!wifi_on && !s_noted_wifi_off) {
                s_noted_wifi_off = true;
                ESP_LOGW(TAG, "WiFi now off - card still mounted and usable, but "
                              "background mirroring stays off until reboot");
            }
            vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
            continue;
        }

        // WiFi on and the boot window closed without a mount: further probes are
        // futile (they fail 0x101 ESP_ERR_NO_MEM regardless of the card), and
        // retrying every 10 s forever is pure log noise. Stop cleanly instead.
        if (wifi_on && !s_mounted) {
            s_parked = true;
            ui_set_sd_state(UI_SD_NONE);
            ESP_LOGW(TAG, "no card mounted in the boot window and WiFi is up - "
                          "further mount attempts cannot succeed; stopping probes");
            continue;
        }

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

        // A write error used to mean "card removed" immediately, because this
        // board routes no card-detect line. But unmounting is a one-way door:
        // re-mounting needs a contiguous DMA-capable allocation, and after WiFi
        // is up the MALLOC_CAP_DMA pool is ~400 B, so the remount can NEVER
        // succeed (measured 2026-07-26: 112 KB free at 4.2 s -> ~400 B from 44 s
        // onward, while the general internal heap stays healthy at a 31 KB
        // largest block). One transient glitch therefore killed the card for the
        // whole session.
        //
        // So retry on the STILL-OPEN handle first: that needs no new allocation
        // and sidesteps the DMA exhaustion entirely. Only conclude removal after
        // several consecutive failed bursts. A genuinely removed card just fails
        // SD_WRITE_FAIL_UNMOUNT times first, which costs nothing that matters.
        if (!ok) {
            s_consec_write_fail++;
            if (s_consec_write_fail < SD_WRITE_FAIL_UNMOUNT) {
                ESP_LOGW(TAG, "SD write failed (%d/%d) - retrying on live handle",
                         s_consec_write_fail, SD_WRITE_FAIL_UNMOUNT);
                xSemaphoreGive(s_sd_mutex);
                vTaskDelay(pdMS_TO_TICKS(WORK_MS));
                continue;
            }
            ESP_LOGW(TAG, "SD write failed %d times consecutively - treating as removal",
                     s_consec_write_fail);
            s_consec_write_fail = 0;
            unmount();
            xSemaphoreGive(s_sd_mutex);
            vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
            continue;
        }
        if (s_consec_write_fail) {
            // The measured answer to "is the EIO transient?" - if this line ever
            // appears, retrying on the live handle is the right fix.
            ESP_LOGW(TAG, "SD write RECOVERED after %d consecutive failure(s)",
                     s_consec_write_fail);
            s_consec_write_fail = 0;
        }

        int burst_ms = (int)((esp_timer_get_time() - burst_t0) / 1000);
        if (burst_ms > s_burst_max_ms) s_burst_max_ms = burst_ms;
        s_burst_cnt++;

        // The burst that just succeeded wrote the complete backup (qso.adi,
        // qmx-config.txt, lotw_cert/key and the README all start dirty). If WiFi
        // is on, park now rather than keep mirroring until the card is killed
        // mid-write. On a WiFi unit the SD diag log loses little - the full log is
        // available over the network at /api/log - and the POTA/no-WiFi case,
        // which is the one that actually needs an on-card log, keeps mirroring
        // continuously below.
        if (wifi_on) {
            park_snapshot();
            xSemaphoreGive(s_sd_mutex);
            continue;
        }

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

bool sd_archive_wait_mounted(uint32_t timeout_ms)
{
    const uint32_t step = 25;
    uint32_t waited = 0;
    while (!s_boot_probe_done && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    return s_mounted;
}
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

