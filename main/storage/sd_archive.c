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
#include "esp_attr.h"      // RTC_NOINIT_ATTR - the #282 durable instrument
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_app_desc.h"   // esp_app_get_description() for the README version stamp

#include "bsp/m5stack_tab5.h"   // bsp_sdcard_init / bsp_sdcard_deinit

#include "diag_log.h"
#include "adif_log.h"
#include "config_io.h"
#include "settings.h"   // wifi_enabled: the WiFi-aware mirroring gate
#include "cw_decode.h"  // cw_decode_take_pending - the #323 CW transcript
#include "ui.h"
#include "psram_task.h"
// The SD write pauses the spectrum stream around itself - see mirror_diag_slow().
// storage -> net is the same direction net/webserver.c already goes the other way
// (it takes sd_archive_lock() for uploads); both live in the `main` component.
#include "webserver_ws.h"   // webserver_ws_set_paused / _is_paused

static const char *TAG = "sd_arch";

#define SD_MOUNT_POINT   "/sdcard"
#define SD_DIR           "/sdcard/qmx-panadapter"
#define SD_LOG_PATH      "/sdcard/qmx-panadapter/qmx-log.txt"
#define SD_LOG_PATH_1    "/sdcard/qmx-panadapter/qmx-log.1.txt"
#define SD_ADIF_PATH     "/sdcard/qmx-panadapter/qso.adi"
#define SD_ADIF_PREV     "/sdcard/qmx-panadapter/qso.prev.adi"
#define SD_CONFIG_PATH   "/sdcard/qmx-panadapter/qmx-config.txt"
#define SD_LOTW_CERT_PATH "/sdcard/qmx-panadapter/lotw_cert.b64"
#define SD_LOTW_KEY_PATH  "/sdcard/qmx-panadapter/lotw_key.b64"
#define SD_README_PATH    "/sdcard/qmx-panadapter/README.txt"
#define SD_CW_PATH        "/sdcard/qmx-panadapter/cw-decode.txt"

// Source (SPIFFS) paths for the LoTW certificate + private key. Mirror of
// lotw_upload.c's CERT_PATH/KEY_PATH — kept here to avoid a cross-module getter
// for two stable, never-renamed paths (a compile check would be overkill).
#define SRC_LOTW_CERT    "/spiffs/lotw_cert.b64"
#define SRC_LOTW_KEY     "/spiffs/lotw_key.b64"

#define SD_LOG_MAX_BYTES (5 * 1024 * 1024)   // rotate qmx-log.txt at 5 MB
#define PROBE_MS          10000               // mount-probe cadence when no card
#define WORK_MS           3000                // mirror cadence while mounted
// Slow diag-only cadence used while WiFi is on (#153). 30 s rather than 3, and the
// file is opened, appended, fsync'd and CLOSED each time instead of being held
// open - so the exposure to the SD/WiFi contention this project has fought for
// months is roughly a tenth of the old continuous mode, and there is no held-open
// handle for a card pull or a crash to damage.
#define SLOW_LOG_MS       30000
// Mount-retry watchdog after the boot window (operator, 2026-09-01). Wide and
// capped on purpose: a mount attempt touches the SD/WiFi contention, so this is
// 5 minutes apart and gives up after an hour rather than probing for ever.
#define MOUNT_RETRY_MS    300000            // 5 min between post-boot attempts
#define MOUNT_RETRY_MAX   12                // ~1 hour, then stop for good
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
        "                  upload to QRZ / eQSL / LoTW. Restore it onto a Tab5\r\n"
        "                  with 'Restore from SD' in the log window.\r\n"
        "  qso.prev.adi    The copy from just before the log last got SMALLER\r\n"
        "                  (a deletion, a reset). Kept so a mistake is not\r\n"
        "                  mirrored away; only replaced by the next shrink.\r\n"
        "  qmx-config.txt  All settings + memory channels (editable INI text).\r\n"
        "                  Restore a device via the web UI's 'Config' upload.\r\n"
        "  lotw_cert.b64   Your LoTW (TQSL) signing certificate and\r\n"
        "  lotw_key.b64    private key (base64 DER). Needed to sign QSOs for\r\n"
        "                  LoTW after moving to / restoring another device.\r\n"
        "  qmx-log.txt     Diagnostic log, newest session (rolling, for bug\r\n"
        "  qmx-log.1.txt   reports); .1 is the previous segment after rotation.\r\n"
        "  cw-decode.txt   Decoded CW, UTC-stamped per line. Gaps are expected\r\n"
        "                  - see the note at the top of that file.\r\n"
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
// Forward declaration: the temp instrument in mirror_diag() reports it.
static bool s_parked;

// ===========================================================================
// TEMP INSTRUMENT (#282) - DURABLE ON PURPOSE. Remove with the diagnosis.
//
// The 2026-08-28 capture contains a contradiction this file's code does not
// allow: "diag write failed" (reachable only through mirror_diag() with a
// non-NULL s_log_file, which only mount() sets) on a boot where mount() never
// logged success and all five boot attempts failed; plus the "stopping probes"
// line printed twice, though its branch is unreachable once s_parked is set and
// nothing ever clears s_parked.
//
// ⛔ IT MUST SURVIVE NOT BEING WATCHED. A serial capture expires, rotates and
// is only running when someone started it, and /api/log/saved holds ~11 minutes
// (CLAUDE.md). If this takes days to recur, log lines alone would miss it. So
// the evidence is COUNTED into RTC no-init RAM - the same store the crash
// record uses - which survives every warm reset (a reboot, a flash, a panic)
// and is served in /api/status as "sd_instr". Ask the device at any later date;
// a non-zero handle_no_mount or park_reentered is the thing being hunted.
//
// Cleared only by a full power cycle, which is honest: RTC RAM does not survive
// one, and the counters say which boot they belong to via boot_id.
// ===========================================================================
#define SD_INSTR_MAGIC 0x5D1A0282u
RTC_NOINIT_ATTR static struct {
    uint32_t magic;
    uint32_t boot_id;          // increments each boot, so a count can be dated
    uint32_t mount_enter;      // mount() called
    uint32_t mount_ok;         // ...and reached the end
    uint32_t handle_no_mount;  // ⭐ THE ANOMALY: a write path held a handle with !s_mounted
    uint32_t unmount_calls;
    uint32_t park_set;         // s_parked latched true
    uint32_t park_reentered;   // ⭐ THE OTHER ANOMALY: the no-card branch ran while parked
    uint32_t first_anom_uptime_s;  // uptime of the FIRST anomaly of either kind, 0 = none
    uint32_t first_anom_boot;      // and which boot it was
} s_instr;

static void instr_init(void)
{
    if (s_instr.magic != SD_INSTR_MAGIC) {
        memset(&s_instr, 0, sizeof s_instr);
        s_instr.magic = SD_INSTR_MAGIC;
    }
    s_instr.boot_id++;
}

// Record the first anomaly seen, whichever kind, so there is a timestamp to
// correlate against a capture if one happens to be running.
static void instr_note_anomaly(void)
{
    if (s_instr.first_anom_uptime_s == 0) {
        s_instr.first_anom_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        s_instr.first_anom_boot     = s_instr.boot_id;
    }
}

void sd_archive_instr_get(sd_archive_instr_t *out)
{
    if (!out) return;
    out->boot_id             = s_instr.boot_id;
    out->mount_enter         = s_instr.mount_enter;
    out->mount_ok            = s_instr.mount_ok;
    out->handle_no_mount     = s_instr.handle_no_mount;
    out->unmount_calls       = s_instr.unmount_calls;
    out->park_set            = s_instr.park_set;
    out->park_reentered      = s_instr.park_reentered;
    out->first_anom_uptime_s = s_instr.first_anom_uptime_s;
    out->first_anom_boot     = s_instr.first_anom_boot;
}

#define SD_BOOT_MOUNT_TRIES   5
#define SD_BOOT_MOUNT_GAP_MS  150

// Append any decoded CW waiting in cw_decode.c to cw-decode.txt (#323, Michael
// KZ4LY). Opened/appended/fsync'd/CLOSED per burst rather than held open: the
// file is written rarely (only while CW is actually being decoded) and the
// whole point is that it survives a card being pulled or the power going -
// bytes sitting in a FatFs buffer behind an open handle would not (the same
// reasoning as the slow diag path; `fflush` alone is not enough, see CLAUDE.md).
//
// Deliberately NOT rotated by size the way qmx-log.txt is: this is human-typed
// Morse at a few characters a second, so it grows by orders of magnitude less
// than the diag log, and truncating an operating session's transcript to save
// kilobytes would defeat what it is for.
static void mirror_cw(void)
{
    char buf[512];
    size_t got = cw_decode_take_pending(buf, sizeof(buf));
    if (got == 0) return;   // the normal case - nothing decoded since last time

    bool fresh = (access(SD_CW_PATH, F_OK) != 0);
    FILE *f = fopen(SD_CW_PATH, "ab");
    if (!f) {
        // The text is already gone from the staging buffer, so say so rather
        // than lose it silently. Not fatal: the next burst still writes.
        ESP_LOGW(TAG, "cw transcript: open %s failed (%s) - %u chars lost",
                 SD_CW_PATH, strerror(errno), (unsigned)got);
        return;
    }
    if (fresh) {
        // Written once, on the file's first creation. States the limitation up
        // front so nobody reads a gap as a decoder fault or as proof of silence.
        fprintf(f, "QMX Panadapter - decoded CW transcript\r\n"
                   "Times are UTC, stamped at the first character of each line.\r\n"
                   "\r\n"
                   "This is what the QMX's OWN decoder resolved and what the screen\r\n"
                   "showed - not a verbatim record of everything sent. The radio's\r\n"
                   "decode buffer holds 40 characters and is not circular, so fast or\r\n"
                   "sustained sending overflows it and the excess is discarded before\r\n"
                   "it ever reaches the Tab5. Unresolved characters are dropped too.\r\n"
                   "Expect gaps; they do not mean the band was quiet.\r\n"
                   "\r\n");
    }
    if (fwrite(buf, 1, got, f) != got)
        ESP_LOGW(TAG, "cw transcript: write failed (%s)", strerror(errno));
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

// Append all newly-captured diag bytes to qmx-log.txt, rotating at 5 MB.
// Returns false on a write error (possible card removal).
static bool mirror_diag(void)
{
    // === TEMP INSTRUMENT (2026-08-28, #282) - remove once the contradiction is
    // closed. This early return is SILENT, which is why the 2026-08-28 capture
    // could not be read: it shows "diag write failed" (reachable only with a
    // non-NULL handle) on a boot where mount() never logged success, so either
    // a mount happened without logging or a handle exists without a mount.
    // Change-detected so a parked session does not spam.
    // ⭐ THE ANOMALY, counted durably: a live handle with no mount. This is the
    // state the 2026-08-28 capture implies and that the code says cannot exist.
    if (s_log_file && !s_mounted) {
        s_instr.handle_no_mount++;
        instr_note_anomaly();
        ESP_LOGE(TAG, "INSTR ANOMALY: log handle %p with s_mounted=0 (parked=%d) "
                      "- this is the #282 case", (void *)s_log_file, (int)s_parked);
    }
    if (!s_log_file) {
        static bool s_noted_nofile = false;
        if (!s_noted_nofile) {
            s_noted_nofile = true;
            ESP_LOGW(TAG, "INSTR mirror_diag declined: no log handle "
                          "(mounted=%d parked=%d)", (int)s_mounted, (int)s_parked);
        }
        return false;
    }

    // ⛔ BOUNDED. This loop used to be `for (;;)` - it drained the ENTIRE diag
    // backlog to the card in one go, under one hold of s_sd_mutex.
    //
    // On the first burst after a boot mount, that backlog is the whole boot log,
    // and the burst therefore runs straight through WiFi bring-up - the one
    // window this project has hardware-proven the card cannot survive (2026-07-26:
    // with WiFi never started the same card mirrored flawlessly for 230 s; with
    // WiFi on it dies within 10-140 s).
    //
    // Measured on this bench 2026-09-01, and it is what the operator sees as the
    // SD dot appearing and then going out:
    //
    //     13.703s  SD mounted on boot attempt 5/5
    //     18.869s  Got IP                        <- WiFi comes up DURING the burst
    //     42.547s  diag write failed: I/O error (errno 5), parked=0, live handle
    //     90.2s    unmount(write failures)
    //
    // `sd_arch` logs NOTHING between 13.703 and 42.547 - 28.8 s of silence. That
    // cannot be many quiet iterations: a burst that completes with WiFi on calls
    // park_snapshot(), which logs. So it is ONE burst, and it is still running
    // when WiFi comes up 5 s later.
    //
    // Two consequences, and the second is the nastier one:
    //  - the card is being written continuously across exactly the wrong window;
    //  - park_snapshot() is the design's OWN protection against that, and it can
    //    only run after a burst that SUCCEEDS - so the failure keeps the device
    //    permanently in the mode the parking was invented to leave.
    // The retry path then re-attempts the same oversized write (the cursor is
    // deliberately not advanced on error), so every retry is the same doomed
    // write, five times, and then the card is declared removed.
    //
    // Bounding it fixes both: a burst is short, a partial catch-up still counts
    // as success, the cursor advances, and the very first burst can reach the
    // park. Whatever backlog is left is then written by mirror_diag_slow() at
    // the #153 cadence, in 4 KB pieces with the stream paused - which is the
    // safe path, not the one that has to be raced.
    #define MIRROR_DIAG_MAX_CHUNKS 4      // 16 KB per burst, ~1 burst per 3 s
    static char buf[DIAG_CHUNK];
    for (int chunk = 0; chunk < MIRROR_DIAG_MAX_CHUNKS; chunk++) {
        uint64_t next = s_diag_cursor;
        size_t got = diag_log_read_from(s_diag_cursor, buf, sizeof(buf), &next);
        if (got == 0) break;
        if (fwrite(buf, 1, got, s_log_file) != got) {
            ESP_LOGW(TAG, "diag write failed: %s  [INSTR mounted=%d parked=%d file=%p]",
                     strerror(errno), (int)s_mounted, (int)s_parked, (void *)s_log_file);
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
static bool s_parked = false;   // (forward-declared above for the temp instrument)
static int64_t s_slow_last_us = 0;   // #153 slow diag mirror pacing
static int     s_slow_fail    = 0;   // consecutive slow-mirror failures
/* Set once the slow diag mirror has given up for this session. The card stays
 * MOUNTED - only the 30 s background append stops. Never cleared: a path that
 * has failed three times running has earned being left alone, and the on-demand
 * consumers are unaffected. */
static bool    s_slow_stopped = false;
/* Post-boot mount retries (see the watchdog in the task loop). */
static int     s_mount_retries = 0;
static int64_t s_mount_retry_last_us = 0;

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
// Append whatever the diag ring has produced since the last call, opening and
// closing the file around the write.
//
// ⛔ WHY THIS EXISTS (#153). Parking used to stop SD logging entirely once WiFi
// was up, on the reasoning that "the full log is available over the network at
// /api/log". That reasoning fails in exactly the case the log is for: after a
// crash the RAM ring is GONE, and the flash copy is a rolling ~11-minute window.
// Michael KZ4LY sent "the full log from the microSD" to explain a reboot and it
// was 17 boot headers each ending at uptime ~4.8 s - a log that stops before the
// crash every single time, and which LOOKS like evidence.
//
// So the card keeps getting the log, just slowly. Open/append/fsync/close per
// burst is deliberately not the held-open handle the continuous path uses: it
// costs a little more per write and removes the corruption window entirely.
// ⛔ AND WHY IT IS QUIESCED (2026-09-01). #153 restored this write, and THIS
// WRITE IS WHAT TRIPS THE EIO WEDGE - measured on the bench the same morning:
//
//     6.1-8.2s  boot mount attempts 1-4 FAILED err=0x108
//    13.7s      mounted on attempt 5/5                <- the dot comes on
//    42-90s     diag write failed: I/O error (errno 5) x5, mounted=1
//    90.2s      INSTR unmount(write failures)         <- the dot goes out
//
// which is exactly the operator's report: the SD dot appears, then vanishes.
// It is NOT a v1.10.5 regression - no SD code changed in that release - and it
// is not memory: CLAUDE.md records this same EIO with 135 KB of DMA free and a
// 65 KB largest block. It is the documented SPI2-SD vs WiFi-SDIO contention.
//
// Every OTHER place in this firmware that writes the card while WiFi is up
// already knows this and quiets the link first - the QRZ/eQSL/LoTW uploads, the
// log download, the Reader's Save offline, the /files browser. This path, added
// later, did none of it. So it is the one SD write on the device that runs
// straight into the contention with the stream at full rate.
//
// ⚠ SAVE AND RESTORE, never set-then-clear. The pause is a plain boolean shared
// with ~29 other call sites, and upload_task() raises it BEFORE it takes
// sd_archive_lock() - so a 30 s write can land inside an upload's own pause
// window, and clearing it unconditionally would drop that upload's protection
// while it is still running. Which is the very hazard being guarded against.
//
// ⛔ THE WS PAUSE ONLY - NOT dsp_set_transfer_quiet(), which the upload path
// pairs it with. Considered and rejected on 2026-09-01, so it does not get
// "restored" later as an oversight:
//   - It buys nothing here. It exists to stop fft_task (pri 4, core 1)
//     preempting the upload task (pri 3, core 0). THIS task is pri 2 pinned to
//     core 0, so fft_task never preempts it and there is nothing to yield.
//   - And it would cost real decodes. In the quiet branch fft_task DISCARDS
//     audio in 50 ms chunks (dsp.c). An upload is occasional and operator-
//     initiated; this write runs every 30 s forever, so during FT8 it would
//     throw audio away on every cycle - which is #51, the single most expensive
//     bug in this project's history, reintroduced deliberately.
// The contention being avoided is SPI2-SD DMA against WiFi-SDIO DMA, and the
// ~10 fps spectrum stream is the SDIO traffic that matters. Pausing it is the
// whole point; quieting the FFT is not.
static bool mirror_diag_slow(void)
{
    static char buf[DIAG_CHUNK];
    uint64_t next = s_diag_cursor;
    size_t got = diag_log_read_from(s_diag_cursor, buf, sizeof(buf), &next);
    if (got == 0) return true;              // nothing new; not a failure

    const bool was_paused = webserver_ws_is_paused();
    if (!was_paused) webserver_ws_set_paused(true);

    FILE *f = fopen(SD_LOG_PATH, "ab");
    bool ok;
    if (!f) {
        sd_fail_diag("slowopen", errno);
        ok = false;
    } else {
        ok = (fwrite(buf, 1, got, f) == got);
        if (ok) { fflush(f); fsync(fileno(f)); }
        else    { sd_fail_diag("slowwrite", errno); }
        fclose(f);
        if (ok) { s_diag_cursor = next; s_log_bytes += got; }
    }

    if (!was_paused) webserver_ws_set_paused(false);
    return ok;
}

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
    ESP_LOGW(TAG, "backup snapshot complete - file mirroring off while WiFi is on; "
                  "diag log continues every %d s (#153) and the card stays mounted "
                  "for Save-offline / web file browser", SLOW_LOG_MS / 1000);
}

static void unmount(const char *why)
{
    // === TEMP INSTRUMENT (#282): "SD card unmounted" said nothing about which
    // path decided that, and the dot going dark is what the operator sees.
    s_instr.unmount_calls++;
    ESP_LOGW(TAG, "INSTR unmount(%s) (mounted=%d parked=%d file=%p)",
             why ? why : "?", (int)s_mounted, (int)s_parked, (void *)s_log_file);
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
    // === TEMP INSTRUMENT (#282): mount() logs success at the very END, so any
    // path that opens the log handle and then leaves early is invisible - and
    // that is exactly the shape the 2026-08-28 capture implies.
    s_instr.mount_enter++;
    ESP_LOGW(TAG, "INSTR mount() entered (mounted=%d parked=%d file=%p)",
             (int)s_mounted, (int)s_parked, (void *)s_log_file);
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
    // The handle now EXISTS while s_mounted is still false. If anything below
    // fails or blocks, that is the state the capture appears to have caught.
    ESP_LOGW(TAG, "INSTR mount() opened log handle %p, s_mounted still %d",
             (void *)s_log_file, (int)s_mounted);

    s_adif_dirty = true;           // force a full mirror right after mounting
    s_config_dirty = true;
    s_lotw_dirty = true;
    write_readme();                // self-describing card (fresh version stamp)
    s_mounted = true;
    s_instr.mount_ok++;
    ui_set_sd_active(true);
    ESP_LOGI(TAG, "SD card mounted, mirroring to %s", SD_DIR);
    return true;
}

// ---- task ------------------------------------------------------------------

// Before the mirror overwrites qso.adi, keep the copy that is there as
// qso.prev.adi - but ONLY when the log has shrunk.
//
// The card is a mirror of the present, and until now that was all it was: the
// boot mirror pushes whatever the device holds over whatever the card holds, so
// a deletion that survived one reboot was permanent on both. It protected
// against the case it was built for (a wipe-and-reinstall, where nothing
// reboots in between) and against nothing else. Found on the bench 2026-09-05
// when a reflash synced a card down from 25 records to 23 and the two deleted
// records existed nowhere afterwards.
//
// Rotating on EVERY write would be worse than useless: the ADIF mirror fires
// once per logged QSO, so after two more contacts the previous copy would be
// one QSO old and the deleted ones gone from both files. Only a SHRINK is the
// dangerous direction, and only a shrink rotates - so qso.prev.adi holds the
// last larger copy for as long as it takes to notice.
//
// Size, not a record count, is the test: records are only ever appended, so
// fewer bytes means fewer records. An edit that clears a report shrinks the
// file by a few bytes and will rotate too - harmless, and erring towards
// keeping a copy is the right way to be wrong here.
//
// Never blocks the mirror. If the rotation cannot be done the mirror still
// runs: a stale card helps nobody either, and the failure is logged.
static void keep_previous_adif(void)
{
    struct stat cur, incoming;
    if (stat(SD_ADIF_PATH, &cur) != 0 || cur.st_size <= 0) return;   // nothing to keep
    const char *src = adif_log_file_path();
    if (!src || stat(src, &incoming) != 0) return;
    if (incoming.st_size >= cur.st_size) return;   // growing or unchanged: normal logging

    unlink(SD_ADIF_PREV);   // FatFs rename will not replace an existing file
    if (rename(SD_ADIF_PATH, SD_ADIF_PREV) == 0) {
        ESP_LOGW(TAG, "QSO log shrank %ld -> %ld bytes; previous copy kept as %s",
                 (long)cur.st_size, (long)incoming.st_size, SD_ADIF_PREV);
    } else {
        ESP_LOGE(TAG, "could not keep the previous QSO log (errno %d) - "
                      "mirroring anyway", errno);
    }
}

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
            // ⭐ MOUNT RETRY WATCHDOG (operator's suggestion, 2026-09-01:
            // "maybe you should establish a watchdog? The card is playing with
            // you"). He is right, and the thing it replaces is a CLAIM that was
            // never tested.
            //
            // The no-card park below says "further mount attempts cannot
            // succeed", on the strength of a 2026-07-26 measurement that the
            // MALLOC_CAP_DMA pool falls to ~400 B once WiFi is up. After the
            // #284 reclamation that is no longer what the device reads: this
            // very session sat at ~16 KB DMA free, and the boot attempts that
            // failed did so with 44-50 KB free. So "cannot" is an assumption
            // carried forward from different numbers.
            //
            // The mount is genuinely intermittent - measured across six boots
            // today with TWO cards and both a warm reset and a cold power
            // cycle, it has failed all five boot attempts and it has succeeded
            // on the first, with no variable yet found that predicts which.
            // Four different error codes in one boot (0x108 INVALID_RESPONSE,
            // 0x109 INVALID_CRC, 0x103 INVALID_STATE, 0x107 TIMEOUT). Against
            // an intermittent fault, retrying IS the fix.
            //
            // ⛔ BOUNDED, and deliberately slow. CLAUDE.md records the FT8
            // respawn watchdog firing ~390 times and degrading the device it
            // was rescuing - "a watchdog that degrades the device it is trying
            // to rescue is not a watchdog". A mount attempt touches the SD/WiFi
            // contention this file exists to avoid, so it gets a wide interval
            // and a hard cap, and then it really does stop.
            //
            // It also fixes something the operator hit head-on: a card inserted
            // while the device is running was IGNORED FOR THE WHOLE SESSION,
            // silently, with the dot never lighting. Now it is picked up within
            // one retry interval.
            if (!s_mounted && s_mount_retries < MOUNT_RETRY_MAX) {
                int64_t now_us = esp_timer_get_time();
                if (now_us - s_mount_retry_last_us >= (int64_t)MOUNT_RETRY_MS * 1000) {
                    s_mount_retry_last_us = now_us;
                    s_mount_retries++;
                    if (xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                        bool ok = try_mount();
                        xSemaphoreGive(s_sd_mutex);
                        if (ok) {
                            // Say it plainly: this is the answer to whether a
                            // post-boot mount is possible at all, and it was
                            // asserted to be impossible for a year.
                            ESP_LOGW(TAG, "MOUNT RETRY %d/%d SUCCEEDED - a card "
                                          "mounted after the boot window, which "
                                          "the old code assumed could never happen",
                                     s_mount_retries, MOUNT_RETRY_MAX);
                            s_parked = false;   // let the normal burst path run
                            s_mount_retry_last_us = 0;
                            s_mount_retries = 0;
                            continue;
                        }
                        ESP_LOGI(TAG, "mount retry %d/%d failed - next in %d s",
                                 s_mount_retries, MOUNT_RETRY_MAX,
                                 MOUNT_RETRY_MS / 1000);
                        if (s_mount_retries >= MOUNT_RETRY_MAX)
                            ESP_LOGW(TAG, "mount retries exhausted (%d) - no "
                                          "further attempts this session",
                                     MOUNT_RETRY_MAX);
                    }
                }
            }

            // ⭐ #153: keep the DIAG LOG going, slowly, so the card can still
            // contain a crash. Parking used to stop it dead, which made every SD
            // log 17 boot headers ending at ~4.8 s - unable to hold the thing it
            // was sent to explain. Only while a card is actually mounted; the
            // no-card park below must stay silent.
            if (s_mounted && !s_slow_stopped) {
                int64_t now_us = esp_timer_get_time();
                if (now_us - s_slow_last_us >= (int64_t)SLOW_LOG_MS * 1000) {
                    s_slow_last_us = now_us;
                    if (xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                        // ONE call - an earlier version called it twice in the
                        // recovery branch, which would have written the same
                        // chunk to the card a second time.
                        bool ok = mirror_diag_slow();
                        if (!ok) {
                            if (++s_slow_fail >= 3) {
                                // ⛔ STOP THE MIRROR - DO NOT UNMOUNT.
                                //
                                // This used to call unmount("slow mirror
                                // failures"), and park_snapshot() a few lines
                                // above already argues why that is wrong, in its
                                // own words: "a remount is impossible once WiFi
                                // is up... Unmounting would therefore make all
                                // three report 'no SD card' with a card
                                // physically inserted, and nothing could ever
                                // bring it back." The failure path did it anyway.
                                //
                                // What is being given up here is an OPTIONAL
                                // background write. The full backup - qso.adi,
                                // qmx-config.txt, the LoTW cert and key, the
                                // README - was completed seconds after boot, and
                                // what the card is still FOR at this point is
                                // Save-offline, the /files browser and the SD log
                                // download. Destroying all three because a
                                // best-effort diag append failed three times is
                                // the wrong trade, and it is exactly what the
                                // operator sees as the SD dot going out.
                                //
                                // Measured 2026-09-01: `SDFAIL[slowopen] err=0x5`
                                // three times at the 30 s cadence, then unmount,
                                // on a card that had been serving files happily
                                // for 1 h 56 m. The card was almost certainly
                                // still there.
                                //
                                // A genuinely REMOVED card still gets noticed -
                                // by the on-demand paths, which fail loudly to
                                // the operator who asked for something. That is
                                // the only case where "card gone" is a safe
                                // conclusion; a background write that failed is
                                // not.
                                s_slow_stopped = true;
                                ui_set_sd_state(UI_SD_SNAPSHOT_ONLY);
                                ESP_LOGW(TAG, "slow diag mirror failed %d times - "
                                              "stopping it for this session. The card "
                                              "stays MOUNTED and usable for Save-offline, "
                                              "/files and the log download; only the "
                                              "30 s diag append is given up.",
                                         s_slow_fail);
                            }
                        } else if (s_slow_fail) {
                            ESP_LOGI(TAG, "slow diag mirror recovered after %d failure(s)",
                                     s_slow_fail);
                            s_slow_fail = 0;
                        }
                        xSemaphoreGive(s_sd_mutex);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
            continue;
        }

        // WiFi on and the boot window closed without a mount: further probes are
        // futile (they fail 0x101 ESP_ERR_NO_MEM regardless of the card), and
        // retrying every 10 s forever is pure log noise. Stop cleanly instead.
        if (wifi_on && !s_mounted) {
            // ⭐ THE OTHER ANOMALY: reaching here a second time means s_parked
            // was false again, and nothing in this file ever clears it.
            if (s_parked) {
                s_instr.park_reentered++;
                instr_note_anomaly();
                ESP_LOGE(TAG, "INSTR ANOMALY: no-card branch re-entered while "
                              "parked - s_parked was cleared by something (#282)");
            } else {
                s_instr.park_set++;
            }
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

        // Decoded CW transcript (#323). Append-only and usually empty, so it
        // costs nothing on a band with no CW on it. A failure here is NOT
        // treated as a card removal - the transcript is a convenience, and
        // letting it declare the card gone would put the diag log and the QSO
        // log through a remount for the sake of it.
        if (ok) mirror_cw();

        if (ok && s_adif_dirty) {
            s_adif_dirty = false;
            keep_previous_adif();
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
            unmount("write failures");
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
    instr_init();   // TEMP INSTRUMENT (#282) - durable counters in RTC RAM
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

// Read the mirrored ADIF log off the card into a PSRAM buffer the caller frees.
//
// This is the other half of a backup: the archive has always been able to put
// qso.adi ONTO the card and never to bring it back, so a log wiped by a clean
// reinstall was recoverable only via a PC, the web UI, and knowing the file was
// there at all. Gyula HA3HZ had 432 QSOs sitting on the card, inside the
// device, and no way to reach them - he assumed the firmware would notice them
// ("the application doesn't detect backwards"), which is a fair thing to assume
// of something that calls itself a backup.
//
// SD I/O lives here rather than in adif_log.c because this file already owns
// the mount, the paths and the lock. The caller does the ADIF parsing.
//
// Same during-WiFi discipline as every other bulk SD read on this board (the
// reader's offline save, the log download, the file browser): the spectrum
// stream is paused for the duration, because SD traffic and the C6's SDIO link
// share one physical peripheral. Returns NULL with *out_len untouched if there
// is no card, no file, or no memory.
char *sd_archive_read_adif_file(bool previous, size_t *out_len)
{
    if (!sd_archive_is_mounted()) {
        ESP_LOGW(TAG, "ADIF restore: no card mounted");
        return NULL;
    }
    if (!sd_archive_lock(5000)) {
        ESP_LOGW(TAG, "ADIF restore: card busy");
        return NULL;
    }

    bool ws_was_paused = webserver_ws_is_paused();
    if (!ws_was_paused) webserver_ws_set_paused(true);

    const char *path = previous ? SD_ADIF_PREV : SD_ADIF_PATH;
    char       *buf = NULL;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        // Not an error for the previous copy - most cards will never have one.
        ESP_LOGI(TAG, "ADIF restore: %s not on the card", path);
    } else {
        size_t len = (size_t)st.st_size;
        buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGE(TAG, "ADIF restore: out of memory for %u bytes", (unsigned)len);
        } else {
            FILE *f = fopen(path, "r");
            size_t got = f ? fread(buf, 1, len, f) : 0;
            if (f) fclose(f);
            // A short read is a failing card, not a short file - do not hand
            // back a truncated log and let it import as if it were complete.
            if (got != len) {
                ESP_LOGE(TAG, "ADIF restore: read %u of %u bytes - card error",
                         (unsigned)got, (unsigned)len);
                free(buf);
                buf = NULL;
            } else {
                buf[len] = '\0';
                if (out_len) *out_len = len;
                ESP_LOGI(TAG, "ADIF restore: read %u bytes from %s", (unsigned)len, path);
            }
        }
    }

    if (!ws_was_paused) webserver_ws_set_paused(false);
    sd_archive_unlock();
    return buf;
}

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

