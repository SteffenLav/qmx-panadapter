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
#include "sdio_ready.h"
#include "wifi.h"
#include "esp_attr.h"      // RTC_NOINIT_ATTR - the #282 durable instrument
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_app_desc.h"   // esp_app_get_description() for the README version stamp

#include "bsp/m5stack_tab5.h"   // bsp_sdcard_init / bsp_sdcard_deinit

#include "diag_log.h"
#include "adif_log.h"
#include "config_io.h"
#include "settings.h"   // wifi_enabled: the WiFi-aware mirroring gate
#include "cw_decode.h"  // cw_decode_peek_pending/commit_pending - the #323 CW transcript
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
// ⚠ 2026-09-09, Uwe DL8UG, whose patch this is: the write to the card has to
// keep being started on a cycle, or the RAM never gets emptied onto it. The
// 3-strikes STOP below used to be permanent for the rest of
// the session - measured on this bench giving up after as little as ~4
// minutes some boots - after which nothing in RAM (diag ring OR the CW
// transcript) ever reaches the card again until a reboot. The contention is
// intermittent, not constant (mirror_diag_slow() recovers mid-session plenty
// of times before any 3-in-a-row run), so "stop forever" throws away every
// later window where the bus happens to be free. SLOW_LOG_MAX_MS is the cap
// for the backoff that replaces it, below.
#define SLOW_LOG_MAX_MS   300000   // cap: retry at least every 5 min, forever
// A web client sees ~10 fps, so 100 ms is three or four dropped frames - the
// point at which a stall stops being invisible and starts being a stutter.
#define WS_PAUSE_WARN_MS  100
/* ⛔ HOW LONG A BROWSER MAY HOLD OFF THE CARD WRITES (Gyula HA3HZ, 2026-09-10:
 * the web page "freezing" in CW mode).
 *
 * A background card write takes SECONDS on this board and the spectrum stream
 * is down for all of it - measured the same evening, with the radio on CW and
 * a card mounted:
 *
 *   diag mirror   3107 ms / 9783 ms / 14767 ms   writing 4096 B
 *   cw transcript  286 ms / 10103 ms /  3815 ms  writing 25-60 B
 *
 * That is the SD-vs-WiFi contention this file already documents at length, and
 * SPI mode made it rarer rather than gone. It is not new and it is not the CW
 * transcript's doing: the diag mirror has run every 30 s since #153, and it is
 * the worse of the two.
 *
 * ⛔ THE ANSWER IS NOT TO WRITE LESS OFTEN WHILE SOMEONE WATCHES. v1.12.3 did
 * exactly that and the deferral was REMOVED on 2026-09-12, because a web page
 * is a MONITOR and must not change what the device records. Leaving one open
 * overnight took the mirror from 4 KB/30 s to 4 KB/180 s - 23 B/s - which is
 * below what WSPR alone produces, so the card fell four hours behind and the SD
 * record of a ten-hour soak ended at 6.5 h. A monitor that silently truncates
 * the log is worse than a stuttering spectrum.
 *
 * ⭐ The numbers above are also the way out, and they were in the log all along:
 * 3107 ms for 4096 B, and elsewhere "8,942 ms to write ONE byte". The cost is
 * per WRITE, not per byte - the open, the fsync, the close, the contention. So
 * mirror_diag_slow() drains up to 64 KB inside ONE open at the same 30 s
 * cadence. The stream is interrupted once per 30 s whether or not anybody is
 * looking, which is precisely what makes the browser irrelevant. */

/* ⛔ TEMPORARY EXPERIMENT (#378), DEFAULT 0 - REMOVE WHEN IT HAS ANSWERED.
 *
 * The stream pause exists to protect the SD write by quieting WiFi. But the
 * write takes SECONDS *with the stream already paused* - up to 29,610 ms for
 * 4 KB, measured 2026-09-10 - so the pause may be buying nothing while costing
 * the whole freeze the operator's users report.
 *
 * Set to 1 and the diag mirror ALTERNATES: paused, unpaused, paused... logging
 * which arm each write used. Alternating rather than two flashes on purpose -
 * band conditions and WiFi load drift over minutes, and two builds an hour
 * apart would not be comparing the same thing.
 *
 * It also bypasses the browser deferral, so a sample lands every 30 s instead
 * of every 3 minutes.
 *
 * ⚠ The unpaused arm runs the SD write straight into WiFi contention, which is
 * the hazard the pause was added for. Bounded to a deliberate test session with
 * someone watching; do not ship it enabled. */
#ifndef SD_PAUSE_EXPERIMENT
#define SD_PAUSE_EXPERIMENT 0
#endif
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

// A write failure while MALLOC_CAP_DMA is this starved is almost certainly the
// boot-time trough (WiFi+BLE+the TLS feeds all converging on the same pool),
// not a pulled card - measured 2026-09-20: DMA free=151 B / lblk=84 B for the
// entire ~30 s span that killed every one of that boot's 5 write attempts and
// got the card declared removed, while INT free stayed 1.7-14 KB throughout
// (healthy operation elsewhere on this bench runs 10-18 KB DMA free). A real
// unplug is not a memory event and does not care about this threshold.
//
// Failures attributed to the trough do NOT count toward SD_WRITE_FAIL_UNMOUNT
// - they get their own, much longer budget instead, so the card survives a
// trough that outlasts 15 s (this one ran ~56 s) without declaring itself
// gone. 40 x WORK_MS = 2 min, comfortably past every trough measured so far,
// while still bounded so a card that is ACTUALLY gone during a starved boot
// is not retried forever.
#define SD_DMA_STARVED_BYTES     4096
#define SD_MEM_RETRY_MAX         40
static int s_consec_mem_fail = 0;
// One rotate-and-retry per mount before concluding the card is gone - see the
// write-failure branch in sd_archive_task(). Cleared on every successful mount
// so a later session gets its own attempt, never re-armed within one.
static bool s_log_rotate_tried = false;

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
// A/B switch for the DMA measurement at the boot-probe loop below. 1 skips
// the probe entirely - safe on a bench with no card, and the only way to
// separate the probe's DMA cost from the WiFi bring-up it now overlaps.
#define SD_BOOT_PROBE_DISABLE 0
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
// ⚠ Pauses the WS stream around the write (2026-09-09, same reasoning as
// mirror_diag_slow() below): this is now also called from the #153 30 s slow
// cadence while WiFi is on, not just from the once-per-mount boot burst, so it
// has to observe the same SD-vs-WiFi-SDIO contention discipline every other
// background writer on this path uses. A no-op when WiFi is off (nothing is
// streaming to pause), so the boot-burst and continuous-mirroring callers are
// unaffected. SAVE AND RESTORE, never set-then-clear - see the #153 note on
// mirror_diag_slow() for why (the pause boolean is shared with ~29 other call
// sites).
static void mirror_cw(void)
{
    char buf[512];
    // PEEK, not take: the bytes stay queued until the write below actually
    // lands, so a transient I/O error (measured on this board - the same
    // SD-vs-WiFi-SDIO contention #153 documents) delays the transcript
    // instead of silently eating it. See cw_decode_commit_pending()'s header.
    size_t got = cw_decode_peek_pending(buf, sizeof(buf));
    if (got == 0) return;   // the normal case - nothing decoded since last time

    /* ⚠ AND THIS PAUSE IS NOW ON A 30 s CADENCE, WHICH IT NEVER USED TO BE.
     *
     * Before the #323 fix, mirror_cw() ran once in the boot burst, so the
     * stream stall it causes happened once and nobody saw it. It now runs
     * every 30 s for as long as CW is being decoded - and Gyula HA3HZ reported
     * the web page "freezing" in CW mode on v1.12.2, tuned to a signal, with
     * the Tab5's own screen unaffected. That is the shape this would produce,
     * and the SD open is not always quick: this bench logged
     * `SDFAIL[slowopen] err=0x5` during a CW session the same evening.
     *
     * ⛔ SO MEASURE IT RATHER THAN ASSUME IT. The pause is timed and reported
     * when it exceeds WS_PAUSE_WARN_MS, because a stall that is only ever
     * inferred from a user's description is indistinguishable from one that is
     * not happening - the #189 lesson. If these lines show tens of
     * milliseconds, Gyula's freeze is something else and this is exonerated;
     * if they show seconds, this is it. */
    const int64_t pause_t0 = esp_timer_get_time();
    const bool was_paused = webserver_ws_is_paused();
    if (!was_paused) webserver_ws_set_paused(true);

    bool committed = false;
    bool fresh = (access(SD_CW_PATH, F_OK) != 0);

    /* ⛔ CLOSE THE PREVIOUS SESSION'S DANGLING LINE, ONCE PER BOOT.
     *
     * cw_decode.c writes a line's CRLF when the NEXT line starts, not when the
     * line ends - the stamp carries the time of the line's first character, so
     * it cannot be written until there is a first character. `s_sd_line_len`
     * is RAM, so a reboot loses the fact that a line was open while the file
     * keeps the unterminated line, and the next boot's stamp lands on the end
     * of it. Measured on the bench 2026-09-10, three sessions on one line:
     *
     *   2026-09-07 14:10:18Z   A2026-09-07 19:57:07Z  O G O2026-09-09 22:45:27Z  K D E OE5POP ...
     *
     * ⚠ It is not new and it is not Uwe's patch - but his fix is what makes it
     * MATTER, because the file is now appended to all session instead of once
     * in the boot burst, so every reboot from here on would weld another
     * session onto the same line.
     *
     * Checked once, from the file itself rather than from any flag we keep:
     * the question is what is on the CARD, and only the card can answer it. */
    static bool s_bol_checked = false;
    if (!fresh && !s_bol_checked) {
        FILE *r = fopen(SD_CW_PATH, "rb");
        if (r) {
            char last = '\n';
            if (fseek(r, -1, SEEK_END) == 0) {
                int ch = fgetc(r);
                if (ch != EOF) last = (char)ch;
            }
            fclose(r);
            s_bol_checked = true;
            if (last != '\n') {
                FILE *t = fopen(SD_CW_PATH, "ab");
                if (t) { fputs("\r\n", t); fflush(t); fsync(fileno(t)); fclose(t); }
            }
        }
        /* A failed open leaves it unchecked so the next tick tries again. */
    }

    FILE *f = fopen(SD_CW_PATH, "ab");
    if (!f) {
        // Left in the staging buffer - retried on the next tick. Only becomes
        // a real loss if failures keep coming until CW_SD_PENDING_CAP fills,
        // which cw_decode.c's own overflow notice already covers.
        ESP_LOGW(TAG, "cw transcript: open %s failed (%s) - %u B queued, will retry",
                 SD_CW_PATH, strerror(errno), (unsigned)got);
    } else {
        if (fresh) {
            // Written once, on the file's first creation. States the limitation
            // up front so nobody reads a gap as a decoder fault or as proof of
            // silence.
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
        if (fwrite(buf, 1, got, f) == got) {
            fflush(f);
            fsync(fileno(f));
            committed = true;
        } else {
            ESP_LOGW(TAG, "cw transcript: write failed (%s) - %u B queued, will retry",
                     strerror(errno), (unsigned)got);
        }
        fclose(f);
    }
    // Only drop the bytes from staging once they are provably on the card -
    // a failed attempt leaves them for the next tick instead of losing them.
    if (committed) cw_decode_commit_pending(got);

    if (!was_paused) webserver_ws_set_paused(false);

    const int64_t held_ms = (esp_timer_get_time() - pause_t0) / 1000;
    if (held_ms >= WS_PAUSE_WARN_MS)
        ESP_LOGW(TAG, "cw transcript: held the web stream %lld ms writing %u B"
                      "%s", (long long)held_ms, (unsigned)got,
                 was_paused ? " (stream was already paused)" : "");
}

// Append all newly-captured diag bytes to qmx-log.txt, rotating at 5 MB.
// Returns false on a write error (possible card removal).
/* ⛔ THE 5 MB ROTATION WAS UNREACHABLE ON EVERY WiFi UNIT, AND THAT IS HOW THE
 * CARD FILLS UP (2026-09-22, chasing Dennis WN4FLA's "SD not mounted").
 *
 * The rotate used to live in ONE place: inside mirror_diag()'s write loop,
 * after a chunk had already been written. Two consequences, and the second is
 * the one that bites:
 *
 *  - mirror_diag() is the CONTINUOUS burst path, and on a WiFi unit it stops
 *    running within seconds of boot (park_snapshot()). From then on the only
 *    writer is mirror_diag_slow(), which had NO rotate at all. So qmx-log.txt
 *    grows without limit for the entire life of the card. The bench's own copy,
 *    pulled 2026-09-20, is 5,368,581 B - already past SD_LOG_MAX_BYTES.
 *
 *  - ⭐ The rotate is what FREES space (remove() of the previous
 *    qmx-log.1.txt), and it was gated behind a SUCCESSFUL write. On a card with
 *    no room left, the write is exactly the thing that cannot happen, so the
 *    one action that would recover the card was unreachable by construction.
 *
 * Both write paths and the mount now go through this, and the size is checked
 * BEFORE writing as well as after. See sd_archive_task()'s unmount path for
 * what the unrecoverable version looked like from the outside: mount OK,
 * README written OK, first append fails, five retries, "card removed". */
static void rotate_diag_log(void)
{
    const bool was_open = (s_log_file != NULL);
    if (s_log_file) { fflush(s_log_file); fclose(s_log_file); s_log_file = NULL; }
    // remove() FIRST - this is the call that actually returns space to the
    // volume. rename() alone frees nothing.
    remove(SD_LOG_PATH_1);
    if (rename(SD_LOG_PATH, SD_LOG_PATH_1) != 0)
        ESP_LOGW(TAG, "rotate: rename %s failed: %s", SD_LOG_PATH, strerror(errno));
    s_log_bytes = 0;
    if (was_open) {
        s_log_file = fopen(SD_LOG_PATH, "ab");
        if (!s_log_file)
            ESP_LOGW(TAG, "rotate: reopen %s failed: %s", SD_LOG_PATH, strerror(errno));
    }
    ESP_LOGW(TAG, "rotated diag log on SD - previous qmx-log.1.txt deleted");
}

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
            rotate_diag_log();
            if (!s_log_file) return false;   // reopen failed, rotate_diag_log() logged it
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

/* ⭐ WHEN A WRITE LAST ACTUALLY SUCCEEDED - what the bottom-bar dot is driven
 * from (esp_timer us; 0 = not once yet this session).
 *
 * Before this, the dot was green only from mount until park_snapshot(), which
 * on any WiFi unit is about ten seconds, and YELLOW for the entire session
 * afterwards - because park_snapshot() sets UI_SD_SNAPSHOT_ONLY once and
 * nothing ever set it back. So after ten seconds the dot said the same thing
 * for ever, no matter what the card was doing: yellow on a perfectly healthy
 * unit writing every 30 s, and yellow on bench dev 2026-09-23 where the
 * MALLOC_CAP_DMA pool collapsed when WiFi came up (58 KB free/31 KB largest
 * block -> 151 B/28 B) and every open returned EIO for 53 minutes straight,
 * slow diag mirror and CW transcript both failing with the backlog piling up
 * in RAM. Two opposite situations, one colour. The operator's words, and the
 * reason this changed: "I never had a green dot tonight - always yellow
 * except just after boot up."
 *
 * ⚠ OUTCOME, NOT POLICY - and that is the point. The obvious fix, going yellow
 * when the mirror backs off, was tried before and deliberately removed (see the
 * BACK OFF block in the task loop): a backoff is not a stop, and one failed
 * cycle followed by a good one should not flicker the dot. Keying off the last
 * SUCCESS satisfies both - a transient failure inside the window stays green,
 * and only a sustained one goes yellow.
 *
 * 90 s = three missed 30 s cycles, the same "three in a row" the backoff logic
 * already treats as meaningful, so the dot and the backoff agree about what
 * counts as trouble. */
static int64_t s_last_write_ok_us = 0;
#define SD_WRITE_FRESH_US  (90 * 1000000LL)
/* Current slow-mirror retry interval, in ms - starts at SLOW_LOG_MS and
 * DOUBLES (capped at SLOW_LOG_MAX_MS) every time s_slow_fail reaches 3,
 * resetting back to SLOW_LOG_MS on the next success. Replaces a permanent
 * stop (2026-09-09, operator): a card that failed 3 times running gets
 * backed off, not abandoned, so a later window where the SD/WiFi contention
 * has cleared still gets the RAM backlog onto the card - see the note on
 * SLOW_LOG_MAX_MS above. */
static int     s_slow_interval_ms = SLOW_LOG_MS;
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
//
// ⭐ ONE OPEN, MANY CHUNKS - AND THE COST IS PER WRITE, NOT PER BYTE.
// This used to write exactly DIAG_CHUNK (4 KB) per call, with its own fopen /
// fwrite / fsync / fclose around it. At the #153 cadence of 30 s that is
// 136 B/s, and in WSPR mode the ring produces more than that - so the card's
// copy fell steadily behind the device. Measured overnight 2026-09-11: the
// mirror ended up FOUR HOURS behind, and the SD record of a ten-hour soak
// stopped at 6.5 h.
//
// Writing more per call is very nearly free, and the log said so long before
// anyone asked: "8,942 ms to write ONE byte". A write that takes nine seconds
// for a single byte is not bandwidth-limited, it is paying a fixed cost -
// the open, the fsync, the close, and the SPI2-vs-WiFi contention the pause
// exists to dodge. So drain the backlog inside ONE open instead of taking that
// fixed cost 16 times for 64 KB.
//
// The cap matters in the other direction: the stream is paused for the whole
// burst, so an unbounded drain after a long stall would freeze the browser for
// as long as it took. SLOW_DRAIN_MAX keeps the worst case bounded while still
// being 16x what a single chunk moved.
#define SLOW_DRAIN_MAX_CHUNKS 16            // <= 64 KB inside one open
static bool mirror_diag_slow(void)
{
    static char buf[DIAG_CHUNK];
    uint64_t next = s_diag_cursor;
    size_t got = diag_log_read_from(s_diag_cursor, buf, sizeof(buf), &next);
    if (got == 0) return true;              // nothing new; not a failure

    /* Timed for the same reason mirror_cw() is, and it is the more important
     * of the two: this one has been running every 30 s since #153, long before
     * the CW transcript existed, so if it stalls for seconds then the web
     * "freeze" is not new and not the CW path's doing. Measured on the CW
     * instrumentation the same evening: 8,942 ms to write ONE byte. */
    const int64_t diag_pause_t0 = esp_timer_get_time();
#if SD_PAUSE_EXPERIMENT
    static bool s_exp_arm = false;
    s_exp_arm = !s_exp_arm;                     /* alternate every write */
    const bool exp_pause = s_exp_arm;
#else
    const bool exp_pause = true;
#endif
    const bool was_paused = webserver_ws_is_paused();
    if (exp_pause && !was_paused) webserver_ws_set_paused(true);

    // Rotate BEFORE opening, not after a successful write - on a full card the
    // write is the thing that cannot happen, and this is the path that frees
    // the space. s_log_file is NULL here (parked), so rotate_diag_log() just
    // removes/renames and leaves nothing open, which is what this path wants.
    if (s_log_bytes >= SD_LOG_MAX_BYTES) rotate_diag_log();

    FILE *f = fopen(SD_LOG_PATH, "ab");
    bool ok;
    size_t wrote = 0;
    if (!f) {
        sd_fail_diag("slowopen", errno);
        ok = false;
    } else {
        ok = true;
        for (int chunk = 0; chunk < SLOW_DRAIN_MAX_CHUNKS && got > 0; chunk++) {
            if (fwrite(buf, 1, got, f) != got) {
                sd_fail_diag("slowwrite", errno);
                // Same reason as the burst path: clear the stream error or every
                // later fwrite on this FILE* returns short WITHOUT touching the
                // card, and a transient fault reads as a permanent one. The
                // cursor is advanced only for chunks that actually landed, so
                // the next call resumes exactly here.
                clearerr(f);
                ok = false;
                break;
            }
            // Advance per chunk, not once at the end: a failure half way through
            // must not re-write the chunks that already reached the card.
            s_diag_cursor = next;
            s_log_bytes  += got;
            wrote        += got;
            got = diag_log_read_from(s_diag_cursor, buf, sizeof(buf), &next);
        }
        // One fsync for the whole burst - it is the expensive part, and the
        // bytes are equally durable whether it runs once or sixteen times.
        if (wrote > 0) { fflush(f); fsync(fileno(f)); }
        fclose(f);
    }
    got = wrote;                            // report what actually went to the card

    if (exp_pause && !was_paused) webserver_ws_set_paused(false);

    const int64_t dheld = (esp_timer_get_time() - diag_pause_t0) / 1000;
#if SD_PAUSE_EXPERIMENT
    ESP_LOGW(TAG, "SDEXP %s %lld ms  %u B  ok=%d%s",
             exp_pause ? "PAUSED  " : "UNPAUSED", (long long)dheld,
             (unsigned)got, ok ? 1 : 0,
             was_paused ? "  (someone else had it paused)" : "");
#else
    if (dheld >= WS_PAUSE_WARN_MS)
        ESP_LOGW(TAG, "diag mirror: held the web stream %lld ms writing %u B%s",
                 (long long)dheld, (unsigned)got,
                 was_paused ? " (stream was already paused)" : "");
#endif
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
/* ⭐ SAY HOW FULL THE CARD IS - ONCE, AND ONLY WHEN A WRITE HAS FAILED.
 *
 * Dennis WN4FLA reported "SD not mounted" and the only thing the device could
 * tell him was a grey dot, while the firmware knew the error code, the heap
 * AND this. A full card and a broken card look identical from the outside and
 * are nothing alike.
 *
 * ⚠ NOT at mount. esp_vfs_fat_info() walks the FAT, and on the 31 GB bench
 * card that measured TEN SECONDS (9.1 s -> 19.3 s in the 2026-09-22 boot),
 * which delayed the mount, the first write and app_main's
 * sd_archive_wait_mounted() behind it. Lazy and one-shot costs nothing on a
 * healthy unit and still answers the only question it was added for. */
static void sd_space_report_once(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb) != ESP_OK) return;
    ESP_LOGW(TAG, "SD space: %llu KB free of %llu KB; qmx-log.txt %u KB "
                  "(rotates at %u KB)",
             (unsigned long long)(freeb / 1024), (unsigned long long)(total / 1024),
             (unsigned)(s_log_bytes / 1024), (unsigned)(SD_LOG_MAX_BYTES / 1024));
    if (freeb < (uint64_t)SD_LOG_MAX_BYTES)
        ESP_LOGW(TAG, "SD is nearly full - appends will fail until the log rotates");
}

static void sd_fail_diag(const char *where, int err)
{
    static int n = 0;
    if (n++ >= SD_FAIL_DIAG_MAX) return;
    sd_space_report_once();
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

    // Rotate at mount if the log is already at the limit. The old code could
    // only reach the rotate after a successful write, so an oversized log on a
    // full card stayed oversized for ever - see rotate_diag_log()'s header.
    if (s_log_bytes >= SD_LOG_MAX_BYTES) rotate_diag_log();
    if (!s_log_file) {
        ESP_LOGW(TAG, "diag log unavailable after rotate - unmounting");
        bsp_sdcard_deinit(SD_MOUNT_POINT);
        return false;
    }
    // The handle now EXISTS while s_mounted is still false. If anything below
    // fails or blocks, that is the state the capture appears to have caught.
    ESP_LOGW(TAG, "INSTR mount() opened log handle %p, s_mounted still %d",
             (void *)s_log_file, (int)s_mounted);

    s_log_rotate_tried = false;    // each mount gets one rotate-and-retry
    s_adif_dirty = true;           // force a full mirror right after mounting
    s_config_dirty = true;
    s_lotw_dirty = true;
    write_readme();                // self-describing card (fresh version stamp)

    /* ⛔⛔ OPEN THE LONG-LIVED LOG HANDLE **LAST**, AFTER EVERY OTHER FILE THIS
     * MOUNT TOUCHES. THIS IS THE FIX FOR "SD MOUNTS THEN GOES GREY".
     *
     * Symptom, on this bench every boot from ~v1.15.1 to v1.16.1: the card
     * mounts, README.txt is written to it successfully, and 10-30 ms later the
     * FIRST append to qmx-log.txt returns EIO. Five retries over 15 s, then
     * unmount("write failures"), and because WiFi is up by then the no-card
     * branch latches s_parked for the rest of the session. The card was never
     * gone - 267 remounts across the capture each failed the same way, hours
     * in, with 100 KB of DMA free.
     *
     * ⭐ MEASURED, at the moment of failure, with a one-shot probe:
     *     512 B to THIS handle ("ab", opened above)  -> 0, EIO
     *     4096 B to a fresh "wb" file                -> 4096, OK
     *     4096 B to a fresh "ab" file                -> 4096, OK
     *     4096 B to qmx-log.txt reopened "wb"        -> 4096, OK
     * So the card, the volume, the file and append are all healthy. The only
     * thing wrong is the HANDLE, and the only thing that distinguishes it from
     * the three that work is that it was opened EARLIER in try_mount() - before
     * ensure_dir()'s sibling work and before write_readme() opened and closed
     * another file on the same volume.
     *
     * ⚠ WHAT INSIDE FatFs INVALIDATES IT IS NOT ESTABLISHED. The suspicion is
     * the exFAT path: FF_USE_LFN is 3 here (CONFIG_FATFS_LFN_HEAP), so
     * INIT_NAMBUF/FREE_NAMBUF put fs->dirbuf in a heap block that is freed when
     * the call returns, and exFAT keeps per-file directory-entry state there.
     * f_sync() reloads it under its own NAMBUF, so that one is safe; the write
     * path was not traced to the end. DO NOT record the exFAT theory as the
     * diagnosis. What IS established is the before/after above.
     *
     * It also fits Gyula HA3HZ's advice to Dennis WN4FLA - "format it to FAT
     * and it will be recognised" - which would sidestep the exFAT path
     * entirely, and which nobody could explain at the time.
     *
     * The early open exists only so ftell() can size the log for the rotate
     * check above. That is done by now, so close it and open the handle we
     * actually keep, last. Anything added to this function later must go
     * ABOVE this block. */
    if (s_log_file) { fclose(s_log_file); s_log_file = NULL; }
    s_log_file = fopen(SD_LOG_PATH, "ab");
    if (!s_log_file) {
        ESP_LOGW(TAG, "reopen %s after mount failed: %s - unmounting",
                 SD_LOG_PATH, strerror(errno));
        bsp_sdcard_deinit(SD_MOUNT_POINT);
        return false;
    }

    s_mounted = true;
    s_instr.mount_ok++;
    /* Mounting wrote README.txt (and the snapshot is about to write qso.adi and
     * qmx-config.txt), so this IS a demonstrated-good write - stamp it, or the
     * dot would sit yellow for the first 30 s of every healthy boot waiting for
     * the slow mirror's first tick. */
    s_last_write_ok_us = esp_timer_get_time();
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
    // ⛔ THE COMMENT ABOVE IS NO LONGER TRUE, MEASURED 2026-09-06. It claims
    // these retries all happen "before WiFi starts". They do not: on the
    // rx-audio build esp_hosted_init() logs at 5.88 s, i.e. BETWEEN attempt 1
    // (5.62 s) and attempt 2 (5.97 s). So the probe and the WiFi bring-up now
    // contend for MALLOC_CAP_DMA at the same moment, and on a bench with NO
    // CARD AT ALL the pool went 135 KB -> 4.4 KB across the five attempts,
    // then to 103 B once NimBLE initialised. At that point the device is one
    // allocation away from anything: the same boot recorded sdio_read taking
    // an Instruction access fault with MEPC=0x00000000, and a later probe
    // asserting inside sdmmc_init_host_frequency.
    //
    // Whose bytes those are is NOT yet established - the two overlap, so one
    // boot cannot separate them. These two lines make it separable: flip
    // SD_BOOT_PROBE_DISABLE and compare "boot probe DMA" after against
    // before. Do not draw the conclusion without running both.
    // ⭐ LET esp_hosted's SDIO CARD INIT GO FIRST (2026-09-06). Measured: the
    // SD mount leaves that init 2039 B of MALLOC_CAP_DMA against 19927 B when
    // the archive is off, and a card init that cannot allocate used to REBOOT
    // the device. Both allocations are brief and early; they were overlapping
    // only because nothing sequenced them. See util/sdio_ready.h for why this
    // is aimed at the card init rather than at "before WiFi", and for the
    // caveat that this is a hypothesis with a clean test.
    //
    // The timeout is generous but bounded, and proceeding on a timeout is
    // deliberate - WiFi may be off, or the sdio_drv.c patch may be missing
    // after a fullclean, and neither should mean "never mount the card".
    {
        // Only wait if WiFi is actually going to run. With it off nothing ever
        // signals, and the card would sit through the whole timeout for a
        // contention that cannot happen - a pointless 6 s delay on exactly the
        // POTA/field configuration where the SD archive matters most.
        // ⛔ panadapter_wifi_is_enabled(), NOT settings_load_all(). This runs
        // on sd_archive's 6 KB task, qmx_settings_t is multi-kilobyte, and the
        // compiler reserves every local's frame at the prologue whether the
        // path is taken or not - so a second copy here adds to the peak even
        // though the one in the loop below already exists. That is the exact
        // bug class CLAUDE.md records FOUR instances of, three of them in
        // wifi.c. A narrow accessor costs a bool.
        if (panadapter_wifi_is_enabled()) {
            qmx_sdio_card_ready_wait(6000);
        } else {
            ESP_LOGI(TAG, "WiFi off - mounting immediately, nothing to contend with");
        }
    }

    // Two largest_free_block() walks per BOOT, not on a periodic path - the
    // cyan-flash rule bans the latter, and the SDFAIL path below already
    // takes the same exemption with a hard cap of 3.
    ESP_LOGW(TAG, "boot probe DMA before: free=%u lblk=%u (int free=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    for (int i = 0; i < (SD_BOOT_PROBE_DISABLE ? 0 : SD_BOOT_MOUNT_TRIES) && !s_mounted; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(SD_BOOT_MOUNT_GAP_MS));
        if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
        bool got = try_mount();
        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
        if (got) {
            if (i) ESP_LOGW(TAG, "SD mounted on boot attempt %d/%d",
                            i + 1, SD_BOOT_MOUNT_TRIES);
            break;
        }
        ESP_LOGW(TAG, "boot mount attempt %d/%d failed", i + 1, SD_BOOT_MOUNT_TRIES);
    }
    ESP_LOGW(TAG, "boot probe DMA after:  free=%u lblk=%u (int free=%u)%s",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             SD_BOOT_PROBE_DISABLE ? "  [PROBE DISABLED]" : "");
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

        /* Bottom-bar dot, reconciled every pass from what the card actually
         * did - see s_last_write_ok_us. GREEN = a write landed inside the
         * freshness window, YELLOW = mounted but nothing has landed lately,
         * and UI_SD_NONE (grey + stroke) is left to the unmount paths that
         * know the card is gone. ui_set_sd_state() only stores an int8 and the
         * UI side redraws only on a change, so calling it every WORK_MS costs
         * nothing. This deliberately overrides park_snapshot()'s one-shot
         * yellow: parking stops the continuous burst, it does not stop the
         * 30 s mirror, and the dot should report the mirror. */
        if (s_mounted) {
            const bool fresh = s_last_write_ok_us != 0 &&
                               (esp_timer_get_time() - s_last_write_ok_us) < SD_WRITE_FRESH_US;
            ui_set_sd_state(fresh ? UI_SD_MIRRORING : UI_SD_SNAPSHOT_ONLY);
        }

        if (s_parked) {
            // Background mirroring stays off for the rest of the session, but the
            // card remains MOUNTED and fully usable on demand (Save offline, web
            // file browser, log download). Resuming the background mirror after
            // WiFi is switched off is deliberately not attempted here - it is an
            // untested path, and a reboot with WiFi off gives the verified
            // continuous-mirroring behaviour.
            // Uwe DL8UG's original report (Gyula HA3HZ too) - "switched the CW
            // transcript on, worked a session, pulled the card and found no
            // cw-decode.txt" - is fixed properly below (2026-09-09): mirror_cw()
            // now rides the #153 30 s slow cadence like the diag log, instead of
            // running only once in the boot burst that parking here turns off.
            // No warning needed any more - the transcript IS being written,
            // just on a 30 s cadence instead of continuously.
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
                    if (s_sd_mutex && xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                        bool ok = try_mount();
                        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
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
            if (s_mounted) {
                int64_t now_us = esp_timer_get_time();
                /* ⛔ NO BROWSER DEFERRAL. A web page is a MONITOR: having one
                 * open must not change what the device records.
                 *
                 * v1.12.3 deferred card writes while webserver_ws_client_streaming(),
                 * up to WS_DEFER_MAX_MS, to stop the spectrum freezing. The
                 * arithmetic was the bug: one 4 KB chunk per forced write meant
                 * 4 KB / 180 s = 23 B/s with a browser open against 4 KB / 30 s
                 * = 136 B/s without one, and WSPR produces more than either. So
                 * a browser left open did not slow the record down, it stopped
                 * it keeping up at all - measured overnight 2026-09-11, the card
                 * ended FOUR HOURS behind and the SD record of a ten-hour soak
                 * stopped at 6.5 h.
                 *
                 * The freeze that deferral was protecting is addressed where it
                 * belongs instead: the cost is per WRITE, not per byte (the log
                 * has "8,942 ms to write ONE byte"), so mirror_diag_slow() now
                 * drains up to 64 KB inside ONE open. The number of times the
                 * stream is interrupted is unchanged at one per 30 s, watcher or
                 * not - which is exactly what makes the browser irrelevant. */
                if (now_us - s_slow_last_us >= (int64_t)s_slow_interval_ms * 1000) {
                    s_slow_last_us = now_us;
                    if (s_sd_mutex && xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                        // ONE call - an earlier version called it twice in the
                        // recovery branch, which would have written the same
                        // chunk to the card a second time.
                        bool ok = mirror_diag_slow();
                        if (ok) s_last_write_ok_us = esp_timer_get_time();
                        if (!ok) {
                            if (++s_slow_fail >= 3) {
                                // ⛔ BACK OFF - DO NOT STOP, DO NOT UNMOUNT.
                                //
                                // This used to set a permanent s_slow_stopped and
                                // give up on the card for the rest of the session.
                                // park_snapshot() already argues why UNMOUNTING is
                                // wrong, in its own words: "a remount is impossible
                                // once WiFi is up... nothing could ever bring it
                                // back." Stopping the retries outright is the same
                                // mistake one level up: it also never comes back,
                                // and the SD-vs-WiFi contention this is fighting is
                                // INTERMITTENT (this same mirror recovers mid-
                                // session plenty of times before any 3-in-a-row
                                // run - see the ELSE branch below), so "permanent"
                                // was throwing away every later window where the
                                // bus happens to be free.
                                //
                                // Doubling the interval (capped at
                                // SLOW_LOG_MAX_MS) instead means: less hammering of
                                // a link that is currently contended (the original
                                // concern #153 was written to address), but the
                                // RAM backlog - diag ring AND the CW transcript
                                // staging buffer - still reaches the card the next
                                // time the bus is quiet, instead of never again.
                                // Uwe DL8UG, 2026-09-09: the write has to keep
                                // being started on a cycle, or the RAM never
                                // gets emptied onto the card.
                                //
                                // ⚠ AND THE SD DOT NO LONGER GOES YELLOW HERE,
                                // deliberately: UI_SD_SNAPSHOT_ONLY means "live
                                // mirroring unavailable", which was true of a
                                // permanent stop and is NOT true of a backoff -
                                // the mirror is still running, just slower. The
                                // state is still used by park_snapshot(), where
                                // it is accurate.
                                //
                                // Measured 2026-09-01 and again 2026-09-09:
                                // `SDFAIL[slowopen/slowwrite] err=0x5` three times
                                // running, sometimes within 4 minutes of boot, on a
                                // card that had been (or went straight back to)
                                // mounting and serving files fine. The card was
                                // never actually gone.
                                s_slow_fail = 0;
                                int prev_ms = s_slow_interval_ms;
                                s_slow_interval_ms *= 2;
                                if (s_slow_interval_ms > SLOW_LOG_MAX_MS)
                                    s_slow_interval_ms = SLOW_LOG_MAX_MS;
                                size_t cw_pending = cw_decode_pending_len();
                                ESP_LOGW(TAG, "slow diag mirror failed 3 times running - "
                                              "backing off %d s -> %d s (not stopping). "
                                              "Card stays MOUNTED; RAM backlog (diag ring "
                                              "+ %u B CW pending) retried at the new interval.",
                                         prev_ms / 1000, s_slow_interval_ms / 1000,
                                         (unsigned)cw_pending);
                            }
                        } else if (s_slow_fail || s_slow_interval_ms != SLOW_LOG_MS) {
                            ESP_LOGI(TAG, "slow diag mirror recovered after %d failure(s) - "
                                          "back to the %d s cadence",
                                     s_slow_fail, SLOW_LOG_MS / 1000);
                            s_slow_fail = 0;
                            s_slow_interval_ms = SLOW_LOG_MS;
                        }
                        // #323 CW transcript, folded into this cadence (2026-09-09,
                        // Uwe DL8UG / Gyula HA3HZ's report). mirror_cw() previously
                        // ran only once, in the boot burst before park_snapshot() -
                        // once that parked, cw_decode.c kept staging bytes into its
                        // pending buffer but nothing ever drained it, so the
                        // transcript went stale for the rest of any WiFi-on session
                        // (or never appeared at all if the boot burst caught no
                        // CW). v1.12.1's fix was a warning that it wasn't happening;
                        // this instead makes it happen, on the same mutex and the
                        // same (now backed-off, never-permanently-stopped) tick as
                        // the diag mirror above, and mirror_cw() now pauses the WS
                        // stream around its own write - see its header. A failure
                        // here does NOT feed s_slow_fail / the backoff: losing the
                        // diag log's crash record is a real cost, losing a few
                        // seconds of CW transcript is not, and the two must not be
                        // able to take each other down (#323's original design).
                        mirror_cw();
                        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
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
        if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

        if (!s_mounted) {
            if (!try_mount()) {
                if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
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
            size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
            if (dma_free < SD_DMA_STARVED_BYTES && s_consec_mem_fail < SD_MEM_RETRY_MAX) {
                s_consec_mem_fail++;
                ESP_LOGW(TAG, "SD write failed with DMA pool starved (%u B free, "
                              "< %u) - NOT counting toward removal (%d/%d mem retries)",
                         (unsigned)dma_free, SD_DMA_STARVED_BYTES,
                         s_consec_mem_fail, SD_MEM_RETRY_MAX);
                if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
                vTaskDelay(pdMS_TO_TICKS(WORK_MS));
                continue;
            }
            s_consec_write_fail++;
            if (s_consec_write_fail < SD_WRITE_FAIL_UNMOUNT) {
                ESP_LOGW(TAG, "SD write failed (%d/%d) - retrying on live handle",
                         s_consec_write_fail, SD_WRITE_FAIL_UNMOUNT);
                if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
                vTaskDelay(pdMS_TO_TICKS(WORK_MS));
                continue;
            }
            /* ⭐ BEFORE DECLARING THE CARD GONE, TRY A DIFFERENT FILE - ONCE.
             *
             * Measured across 547 boots (capture-dev.txt, 2026-09-22): in the
             * failing sessions, every file opened "wb" is written successfully
             * on the same mount - README.txt 265 for 265 - while every append
             * to qmx-log.txt fails with EIO. The card remounted 267 times and
             * the first append failed again every time, hours into the session
             * with 100 KB of DMA free. A card that is genuinely gone cannot
             * write README.txt; a card whose qmx-log.txt has a damaged cluster
             * chain behaves exactly like this, because "wb" discards the old
             * chain and "ab" has to walk and extend it.
             *
             * Rotating renames the suspect file to qmx-log.1.txt and starts a
             * new one, which is the cheapest thing that can distinguish the two
             * and is also the repair if it is the file. If the next burst still
             * fails, the card really is unhappy and the unmount below stands.
             *
             * ⚠ The cause of the corruption is NOT established - holding the
             * log open across unmount() and unclean power-downs is the obvious
             * suspect, and the unreachable rotation (see rotate_diag_log()) is
             * why the file was allowed past 5 MB in the first place. Do not
             * record this comment as the diagnosis. */
            if (!s_log_rotate_tried) {
                s_log_rotate_tried = true;
                ESP_LOGW(TAG, "SD write failed %d times - the card writes OTHER "
                              "files fine, so rotating qmx-log.txt out of the way "
                              "and trying once more before calling it removed",
                         s_consec_write_fail);
                rotate_diag_log();
                s_consec_write_fail = 0;
                s_consec_mem_fail = 0;
                if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
                vTaskDelay(pdMS_TO_TICKS(WORK_MS));
                continue;
            }
            ESP_LOGW(TAG, "SD write failed %d times consecutively - treating as removal",
                     s_consec_write_fail);
            s_consec_write_fail = 0;
            s_consec_mem_fail = 0;
            unmount("write failures");
            if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
            vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
            continue;
        }
        if (s_consec_write_fail || s_consec_mem_fail) {
            // The measured answer to "is the EIO transient?" - if this line ever
            // appears, retrying on the live handle is the right fix.
            ESP_LOGW(TAG, "SD write RECOVERED after %d consecutive failure(s) "
                          "(%d attributed to a starved DMA pool)",
                     s_consec_write_fail + s_consec_mem_fail, s_consec_mem_fail);
            s_consec_write_fail = 0;
            s_consec_mem_fail = 0;
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
            if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
            continue;
        }


        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);

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
    // 6144 -> 12288: a qmx_settings_t local in this file (gs). CONFIRMED
    // crashing this task on hardware 2026-09-14 (Stack protection fault,
    // ~5 s uptime) TWICE - a first +1024 bump was not enough, because the
    // struct grew ~1350 B total this session (pwr_cal field, then the
    // 5->23-point expansion) and +1024 undershot that. Generous this time,
    // not incremental - PSRAM-backed, costs nothing but PSRAM.
    psram_task_create(sd_archive_task, "sd_archive", 14336, NULL,
                       2 /* low priority */, 0);
}

bool sd_archive_is_mounted(void)        { return s_mounted; }

bool sd_archive_wait_mounted(uint32_t timeout_ms)
{
#if SD_ARCHIVE_DISABLED
    /* Nothing will ever set s_boot_probe_done when the archive is compiled
     * out, so without this the caller in app_main burns the WHOLE timeout
     * before starting WiFi. Measured 2026-09-06 while A/B-testing the SD
     * archive's DMA cost: esp_hosted_init moved 5.81 s -> 9.23 s purely
     * because of this, which confounded the very experiment it was in. */
    return false;
#else
    const uint32_t step = 25;
    uint32_t waited = 0;
    while (!s_boot_probe_done && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    return s_mounted;
#endif
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

