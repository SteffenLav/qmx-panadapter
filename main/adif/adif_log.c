// ADIF QSO log backed by SPIFFS (/spiffs/qso.adi).
//
// Appends one ADIF record per completed FT8 QSO. Each record is on its own
// line for easy parsing. Keeps an in-memory worked-call cache (loaded from
// the file at boot) so adif_log_contains_call() never touches SPIFFS at
// query time.
//
// Thread safety: s_lock covers only the in-memory state (s_count, s_worked).
// SPIFFS's own VFS lock covers concurrent file access; we don't need to hold
// s_lock during I/O.

#include "adif_log.h"
#include "adif_check.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "sd_archive.h"
#include "settings.h"   // upload-cursor adjustment in adif_log_delete_record()
#include "ui.h"         // ui_toast - a failed log write must reach the operator
#include <unistd.h>     // fsync
#include <dirent.h>     // the boot-time /spiffs listing
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static const char *TAG      = "adif";
static const char *FILE_PATH = "/spiffs/qso.adi";

static SemaphoreHandle_t s_lock;
static int               s_count   = 0;
static bool              s_mounted = false;

// Worked-call cache: unique (callsign, band) pairs from every logged QSO.
// Band-aware so the same station on a new band counts as NOT worked before
// (a fresh band-slot). ADIF_BAND_MAX fits "160M" + NUL.
#define ADIF_WORKED_CACHE 1024
#define ADIF_CALL_MAX     16
#define ADIF_BAND_MAX     6
typedef struct {
    char call[ADIF_CALL_MAX];
    char band[ADIF_BAND_MAX];
} worked_entry_t;
// PSRAM, allocated in adif_log_init(). As a static array this was 22.5 KB of
// internal .bss - the single largest consumer in the whole firmware - and every
// byte of it was pointless: the cache is scanned linearly, on demand, from the
// UI and the QSO machine. Nothing here is latency-critical or DMA-adjacent.
// Indexing is unchanged (s_worked[i] reads the same on a pointer); only the
// declaration, the allocation and the NULL guards differ. See the internal-RAM
// audit note at the top of this file's history: internal free sat at 23 KB with
// a 0 KB watermark, which is what turned esp_hosted allocation failures into
// reboots.
static worked_entry_t *s_worked;
static int            s_worked_count = 0;

// ---------------------------------------------------------------------------

// Exposed as adif_log_band_for_freq() below - callers that need to compare two
// frequencies "by band" must use the same table this file logs BAND from, or the
// comparison can disagree with the log it is reasoning about.
static const char *freq_to_band(uint32_t hz)
{
    if (hz >= 1800000  && hz < 2000000)  return "160M";
    if (hz >= 3500000  && hz < 4000000)  return "80M";
    if (hz >= 5330000  && hz < 5410000)  return "60M";
    if (hz >= 7000000  && hz < 7300000)  return "40M";
    if (hz >= 10100000 && hz < 10150000) return "30M";
    if (hz >= 14000000 && hz < 14350000) return "20M";
    if (hz >= 18068000 && hz < 18168000) return "17M";
    if (hz >= 21000000 && hz < 21450000) return "15M";
    if (hz >= 24890000 && hz < 24990000) return "12M";
    if (hz >= 28000000 && hz < 29700000) return "10M";
    if (hz >= 50000000 && hz < 54000000) return "6M";
    return "";
}

// Write <FIELDNAME:N>value to f. Skips if value is NULL or empty.
static void write_field(FILE *f, const char *name, const char *value)
{
    if (!value || !value[0]) return;
    fprintf(f, "<%s:%zu>%s", name, strlen(value), value);
}

// Add a (callsign, band) pair to the worked cache (deduplicates on the pair,
// ignores overflow). band may be NULL/"" (e.g. an out-of-band log entry).
static void cache_add(const char *call, const char *band)
{
    if (!s_worked) return;          // allocation failed at init; cache disabled
    if (!call || !call[0]) return;
    if (!band) band = "";
    for (int i = 0; i < s_worked_count; i++) {
        if (strcmp(s_worked[i].call, call) == 0 &&
            strcmp(s_worked[i].band, band) == 0) return;  // already present
    }
    if (s_worked_count < ADIF_WORKED_CACHE) {
        strncpy(s_worked[s_worked_count].call, call, ADIF_CALL_MAX - 1);
        s_worked[s_worked_count].call[ADIF_CALL_MAX - 1] = '\0';
        strncpy(s_worked[s_worked_count].band, band, ADIF_BAND_MAX - 1);
        s_worked[s_worked_count].band[ADIF_BAND_MAX - 1] = '\0';
        s_worked_count++;
    }
}

// Extract an ADIF field value (<FIELD:len>value) from a single record line.
// Returns false if the field is absent or doesn't fit. The "<FIELD:" tag is
// '<'-anchored, so "CALL" never matches inside "STATION_CALLSIGN", etc.
bool adif_log_extract_field(const char *line, const char *field,
                            char *out, size_t out_sz)
{
    char tag[24];
    int  tl = snprintf(tag, sizeof(tag), "<%s:", field);
    const char *p = strstr(line, tag);
    if (!p) return false;
    p += tl;
    int len = atoi(p);
    const char *close = strchr(p, '>');
    if (!close || len <= 0 || (size_t)len >= out_sz) return false;
    int copy = (len < (int)out_sz - 1) ? len : (int)out_sz - 1;
    memcpy(out, close + 1, copy);
    out[copy] = '\0';
    return true;
}

// Scan the ADIF file and populate s_count + s_worked cache.
static void load_from_file(void)
{
    FILE *f = fopen(FILE_PATH, "r");
    if (!f) return;

    char line[512];
    int  count = 0;

    // One record per line, terminated by <EOR>. Extract CALL + BAND together
    // and cache the pair (the header line has no <EOR> and is skipped).
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "<EOR>")) continue;
        count++;
        char call[ADIF_CALL_MAX], band[ADIF_BAND_MAX];
        if (adif_log_extract_field(line, "CALL", call, sizeof(call))) {
            if (!adif_log_extract_field(line, "BAND", band, sizeof(band))) band[0] = '\0';
            cache_add(call, band);
        }
    }

    fclose(f);
    s_count = count;
    ESP_LOGI(TAG, "Loaded %d QSOs from ADIF log", s_count);
}

// Write the ADIF file header to an open file (assumed empty / newly created).
static void write_header(FILE *f)
{
    fprintf(f,
        "<ADIF_VER:5>3.1.4 "
        "<PROGRAMID:13>QMX-Panadapter "
        "<EOH>\n");
}

// One-time migration (2026-06-30): a prior version wrote a redundant
// SUBMODE field duplicating MODE for every FT8/FT4 QSO (see the write_field
// call site below) - QRZ's logbook import rejects records with that pairing
// ("Undefined message or mode"). Fixing the write side alone isn't enough:
// the QRZ upload always resumes from the first not-yet-uploaded record, so
// one already-logged bad record permanently blocks every future upload
// attempt at that exact same QSO, no matter how many newer records are
// clean. Strips the bad field from any existing record still carrying it,
// in place, before the upload offset (qrz_uploaded_n) ever points at one.
static void repair_legacy_submode_field(void)
{
    FILE *f = fopen(FILE_PATH, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return; }
    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    // ⛔ ONLY the self-referential pairing is bad, and since v1.9.6 that
    // distinction is load-bearing: FT4 is now legitimately logged as
    // MODE=MFSK + SUBMODE=FT4, and a blind sweep for "<SUBMODE:3>FT4" (which
    // is what this used to do) would strip the submode off every new record
    // at the next boot, leaving a bare MODE=MFSK that says nothing about
    // which MFSK mode it was. So the SUBMODE is removed only from a record
    // whose MODE is the SAME value - the pairing QRZ rejects - and each
    // record is examined on its own line rather than the file as one string.
    static const char *const dupes[][2] = {
        { "<MODE:3>FT8", "<SUBMODE:3>FT8" },
        { "<MODE:3>FT4", "<SUBMODE:3>FT4" },
    };
    bool changed = false;
    char *line = buf;
    while (line && *line) {
        size_t linelen = strcspn(line, "\n");
        for (size_t d = 0; d < sizeof(dupes) / sizeof(dupes[0]); d++) {
            char saved = line[linelen];
            line[linelen] = '\0';                       // confine the search
            char *mode = strstr(line, dupes[d][0]);
            char *sub  = mode ? strstr(line, dupes[d][1]) : NULL;
            size_t slen = strlen(dupes[d][1]);
            line[linelen] = saved;
            if (!sub) continue;
            memmove(sub, sub + slen, strlen(sub + slen) + 1);  // incl. NUL
            linelen -= slen;
            changed = true;
        }
        char *nl = strchr(line, '\n');
        line = nl ? nl + 1 : NULL;
    }
    if (changed) {
        FILE *out = fopen(FILE_PATH, "wb");
        if (out) {
            fwrite(buf, 1, strlen(buf), out);
            fclose(out);
            ESP_LOGI(TAG, "repaired legacy duplicate-SUBMODE field(s) in %s", FILE_PATH);
            sd_archive_mark_adif_dirty();   // re-mirror the repaired file to SD too
        } else {
            ESP_LOGW(TAG, "could not rewrite %s for SUBMODE repair", FILE_PATH);
        }
    }
    free(buf);
}

// ---------------------------------------------------------------------------

void adif_log_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    s_worked = heap_caps_calloc(ADIF_WORKED_CACHE, sizeof(worked_entry_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_worked) {
        // Not fatal: worked-before lookups just answer "no". Logging still works.
        ESP_LOGE(TAG, "no PSRAM for the worked-call cache - worked-before disabled");
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "storage",
        .max_files              = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return;
    }
    s_mounted = true;

    // Repair before anything reads or writes a byte. Found necessary in
    // practice (2026-08-25): a unit reported `used` far exceeding what its
    // actual files add up to (701 KB used, but qso.adi+diag.0.log+LoTW
    // cert/key totalled ~170 KB) alongside a directory entry that couldn't
    // even be stat()'d - that gap is orphaned/inconsistent index blocks, not
    // legitimate usage, and it made every write fail with ENOSPC (a single
    // record delete's temp file, and the diag log's own flash-persist)
    // despite ~230 KB of nominally free space. esp_spiffs_check() is
    // ESP-IDF's own consistency check/repair for exactly this - cheap
    // (runs once, at boot, before any file is opened) and self-healing, so
    // it costs nothing on an already-healthy card and fixes this class of
    // fault on one that isn't.
    esp_err_t chk = esp_spiffs_check("storage");
    if (chk != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS check: %s (continuing anyway)", esp_err_to_name(chk));
    }
    // check() fixes CONSISTENCY (an orphaned/corrupt index entry); it does
    // not promise to reclaim space. gc() is the one that actively walks
    // pages looking to free some - request enough for the delete-record
    // temp file (a full copy of the log) plus real headroom. Field-tested
    // 2026-08-25: check() alone reported success but a subsequent delete
    // still hit ENOSPC, so both are needed, not either alone.
    esp_err_t gc = esp_spiffs_gc("storage", 65536);
    if (gc != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS gc: %s (continuing anyway)", esp_err_to_name(gc));
    }

    // Check free space and warn if low (< 64 KB).
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: %zu KB used / %zu KB total (check=%s gc=%s)",
                 used / 1024, total / 1024, esp_err_to_name(chk), esp_err_to_name(gc));
        if (total - used < 65536)
            ESP_LOGW(TAG, "SPIFFS nearly full - ADIF writes may fail");
    }

    // ...and say WHO is using it. The total alone is not actionable: on
    // 2026-08-18 a device reported 536 KB used of 934 KB and then failed diag
    // writes with ENOSPC, and answering "which file" needed a rebuilt firmware
    // and a serial capture because the only way in was esp_spiffs_info(). This
    // partition holds the QSO log, the LoTW certificate AND PRIVATE KEY, and the
    // diag log, so a stale or runaway file here is a real fault and the operator
    // can now read it straight off the boot log. One-shot at boot, a handful of
    // entries, no periodic cost - and deliberately NOT a directory walk on a
    // timer (see the "no long interrupts-off critical sections" rule).
    DIR *d = opendir("/spiffs");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char p[64];
            struct stat st;
            // %.31s, not %s: dirent declares d_name[256], so the compiler cannot
            // see that a SPIFFS name is bounded by CONFIG_SPIFFS_OBJ_NAME_LEN
            // (32, i.e. 31 chars + NUL) and rejects the build with
            // -Werror=format-truncation. Stating the real bound keeps the buffer
            // small - this runs on app_main's task, where CLAUDE.md's "a
            // multi-hundred-byte local is a bug until proven otherwise" applies.
            snprintf(p, sizeof(p), "/spiffs/%.31s", e->d_name);
            if (stat(p, &st) == 0) {
                ESP_LOGI(TAG, "SPIFFS:   %-20s %8ld B", e->d_name, (long)st.st_size);
            } else {
                ESP_LOGI(TAG, "SPIFFS:   %-20s (stat failed)", e->d_name);
            }
        }
        closedir(d);
    }

    // Create file with header if it doesn't exist yet.
    FILE *f = fopen(FILE_PATH, "r");
    if (!f) {
        f = fopen(FILE_PATH, "w");
        if (f) { write_header(f); fclose(f); }
        else ESP_LOGE(TAG, "Cannot create %s", FILE_PATH);
    } else {
        fclose(f);
        repair_legacy_submode_field();
        load_from_file();
    }
}

void adif_log_record(const adif_qso_t *qso)
{
    if (!s_mounted || !qso || !qso->their_call || !qso->their_call[0]) return;

    FILE *f = fopen(FILE_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for append", FILE_PATH);
        return;
    }

    // Frequency in MHz, 3 decimal places (ADIF convention for HF).
    char freq_str[16];
    snprintf(freq_str, sizeof(freq_str), "%.3f", (double)qso->freq_hz / 1e6);

    // UTC date + time from qso_time.
    struct tm tm;
    gmtime_r(&qso->qso_time, &tm);
    char date_str[9], time_str[7];
    strftime(date_str, sizeof(date_str), "%Y%m%d", &tm);
    strftime(time_str, sizeof(time_str), "%H%M%S", &tm);

    write_field(f, "CALL",         qso->their_call);
    write_field(f, "FREQ",         freq_str);
    write_field(f, "BAND",         freq_to_band(qso->freq_hz));
    // ADIF is not symmetrical about these two: FT8 is a MODE in its own right,
    // FT4 is only ever a SUBMODE of MFSK. So FT8 is written as MODE=FT8 and FT4
    // as MODE=MFSK + SUBMODE=FT4 - which is also exactly what WSJT-X writes, so
    // it is the form every other program expects to read.
    //
    // We wrote MODE=FT4 until v1.9.6. POTA accepts it, which is why it survived
    // this long, but ADIFMaster refuses to load a file that declares FT4 as a
    // mode at all, and an activator editing his log before submitting it hits
    // that immediately (Don Adams WB0LQW, 2026-08-24). eQSL meanwhile silently
    // remaps it and says so in its import report ("Mode: xxx was mapped to
    // Mode: yyy Submode: zzz", added there 2026-03-01) - i.e. the receiving end
    // was already correcting us.
    //
    // ⛔ This is the ADIF FILE only. It must NOT reach the LoTW upload as-is:
    // MFSK is not one of LoTW's own modes, so a TQ8 carrying MODE=MFSK would be
    // rejected. lotw_upload.c maps MODE+SUBMODE back to the LoTW mode (FT4)
    // before signing - see lotw_mode_from_adif().
    //
    // A much earlier version wrote SUBMODE as a DUPLICATE of MODE (MODE=FT4
    // SUBMODE=FT4). QRZ's logbook import rejects that pairing outright
    // ("Undefined message or mode"), its validation table having no
    // self-referential entry; repair_legacy_submode_field() still strips it
    // from old records, and now tells that pairing apart from this legitimate
    // one.
    {
        const char *mode = qso->mode ? qso->mode : "FT8";
        if (strcasecmp(mode, "FT4") == 0) {
            write_field(f, "MODE",    "MFSK");
            write_field(f, "SUBMODE", "FT4");
        } else {
            write_field(f, "MODE",    mode);
        }
    }
    // An unknown report is OMITTED, never filled in. These used to default to
    // "599", which is a harmless convention in CW/SSB but in an FT8 log is a
    // fabricated measurement - and one that gets uploaded to QRZ, eQSL and LoTW
    // as if it were real. Roy KI0ER spotted it in his own log and reasonably
    // asked whether stations really were sending him 599 (2026-07-29). ADIF
    // requires neither field, and LoTW ignores both.
    if (qso->rst_sent && qso->rst_sent[0]) write_field(f, "RST_SENT", qso->rst_sent);
    if (qso->rst_rcvd && qso->rst_rcvd[0]) write_field(f, "RST_RCVD", qso->rst_rcvd);
    write_field(f, "QSO_DATE",     date_str);
    write_field(f, "TIME_ON",      time_str);
    // STATION_CALLSIGN, not MY_CALL. Both carry the same thing - the callsign
    // the contact was made under - but STATION_CALLSIGN is the ADIF-spec field
    // and MY_CALL is not, so POTA accepts our file and then warns about it:
    // "WARNING [QSO(s) 1-14] No station_callsign field, assuming operator
    // WB0LQW" (Don Adams WB0LQW, after four real activations, 2026-08-24). It
    // is only ever guessing the right answer there, and a guess about who made
    // the contact is not something to leave in a log that gets uploaded.
    // Nothing in this firmware reads MY_CALL back, so this is a pure rename.
    write_field(f, "STATION_CALLSIGN", qso->my_call);
    write_field(f, "MY_GRIDSQUARE", qso->my_grid);
    write_field(f, "GRIDSQUARE",   qso->their_grid);
    if (qso->their_arrl_section && qso->their_arrl_section[0]) {
        // Standard ADIF fields for a Field Day-style contest exchange.
        // STX_STRING/SRX_STRING carry the literal "<class> <section>" text;
        // ARRL_SECT/MY_ARRL_SECT carry just the section for loggers that
        // parse it as an enum.
        char stx[16], srx[16];
        snprintf(stx, sizeof(stx), "%s %s",
                 qso->my_arrl_class ? qso->my_arrl_class : "", qso->my_arrl_section ? qso->my_arrl_section : "");
        snprintf(srx, sizeof(srx), "%s %s",
                 qso->their_arrl_class ? qso->their_arrl_class : "", qso->their_arrl_section);
        write_field(f, "CONTEST_ID",  "ARRL-FD");
        write_field(f, "STX_STRING",  stx);
        write_field(f, "SRX_STRING",  srx);
        write_field(f, "ARRL_SECT",   qso->their_arrl_section);
        if (qso->my_arrl_section && qso->my_arrl_section[0])
            write_field(f, "MY_ARRL_SECT", qso->my_arrl_section);
    }

    // Activation fields. MY_SIG/MY_SIG_INFO say what WE were activating and are
    // what POTA and SOTA read to credit the activation; SIG/SIG_INFO say what
    // THEY were, i.e. our chase. Our own side is read from settings here rather
    // than passed in, so every present and future caller of this function gets
    // it automatically - a QSO silently logged without MY_SIG_INFO during an
    // activation is an uncreditable contact the operator only finds out about
    // after the fact, when the log is rejected.
    {
        const char *my_sig = settings_activation_sig_name();
        char my_ref[16];
        if (my_sig && settings_get_activation_ref(my_ref, sizeof(my_ref))) {
            write_field(f, "MY_SIG",      my_sig);
            write_field(f, "MY_SIG_INFO", my_ref);
        }
    }
    if (qso->their_sig_info && qso->their_sig_info[0]) {
        write_field(f, "SIG",      qso->their_sig ? qso->their_sig : "POTA");
        write_field(f, "SIG_INFO", qso->their_sig_info);
    }

    fprintf(f, "<EOR>\n");

    // ⛔ A LOGGER MUST NOT CLAIM A CONTACT IT DID NOT WRITE.
    //
    // None of the fprintf()s above were checked and neither was fclose(), so a
    // full or failing filesystem lost the QSO in silence while the code went on
    // to increment the count and log "Logged QSO #N" - the operator is told it
    // is safe, and finds out when they come to upload. The `storage` partition
    // is 1 MB shared between this log, the 256 KB rolling diagnostic log and its
    // rotation, and the LoTW certificate and key, so filling it is a real
    // prospect on a long trip rather than a theoretical one (Gyula HA3HZ asked
    // how much the log holds, which is what turned this up).
    //
    // fsync before fclose for the reason CLAUDE.md already gives about the SD
    // log: fclose flushes to the filesystem, but a power cut immediately after
    // can still lose it, and a QSO is not something to lose cheaply.
    bool write_ok = (ferror(f) == 0);
    if (write_ok) {
        fflush(f);
        fsync(fileno(f));
    }
    if (fclose(f) != 0) write_ok = false;

    if (!write_ok) {
        ESP_LOGE(TAG, "FAILED to log QSO with %s - the log file could not be "
                      "written (filesystem full?). The contact is NOT saved.",
                 qso->their_call);
        char msg[96];
        snprintf(msg, sizeof(msg), "QSO with %s NOT logged - storage full?",
                 qso->their_call);
        ui_toast(msg);
        return;                      // do NOT count it, do NOT mirror it
    }

/* A mutex that does not exist yet is not a reason to kill the device.
 * xSemaphoreTake(NULL) asserts inside FreeRTOS (queue.c:1709), which is an
 * abort() - and it fires from the HTTP task, because a browser that is already
 * open starts polling the moment the server binds, which can be before some
 * subsystem's init has run. Observed 7 times in this bench's capture history,
 * most recently 2026-09-06 about 100 ms after "HTTP server started".
 *
 * Failing safe is not merely tolerable here, it is CORRECT: if the mutex has
 * not been created then no other task can be inside the critical section
 * either, so running unlocked cannot race anything. spots.c, psk_rx.c,
 * update_check.c and ft8_status.c already guard this way; these did not. */
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count++;
    cache_add(qso->their_call, freq_to_band(qso->freq_hz));
    if (s_lock) xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Logged QSO #%d: %s @ %.4f MHz (%s/%s)",
             s_count, qso->their_call,
             (double)qso->freq_hz / 1e6,
             qso->rst_sent ? qso->rst_sent : "?",
             qso->rst_rcvd ? qso->rst_rcvd : "?");

    sd_archive_mark_adif_dirty();  // re-mirror the ADIF file to SD if a card is in
}

int adif_log_count(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count;
    if (s_lock) xSemaphoreGive(s_lock);
    return n;
}

const char *adif_log_file_path(void)
{
    return FILE_PATH;
}

bool adif_log_contains_call(const char *call)
{
    if (!call || !call[0]) return false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < s_worked_count && !found; i++) {
        if (strcmp(s_worked[i].call, call) == 0) found = true;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return found;
}

bool adif_log_contains_call_on_band(const char *call, uint32_t freq_hz)
{
    if (!call || !call[0]) return false;
    const char *band = freq_to_band(freq_hz);
    // Unknown band (freq 0: no QMX / CAT not up - e.g. FT8 simulation mode):
    // fall back to a call-only match instead of returning false. Returning
    // false made "Exclude worked before" silently pass EVERYONE whenever the
    // frequency was unreadable - observed in sim mode as the same phantom
    // being worked and logged back-to-back with the filter checked. Matching
    // any band is the conservative direction for a filter whose job is
    // avoiding duplicates.
    bool any_band = (band[0] == '\0');
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < s_worked_count && !found; i++) {
        if (strcmp(s_worked[i].call, call) == 0 &&
            (any_band || strcmp(s_worked[i].band, band) == 0)) found = true;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return found;
}

bool adif_log_get_record(int idx, char *out, size_t out_sz)
{
    if (!s_mounted || idx < 0 || !out || out_sz == 0) return false;

    FILE *f = fopen(FILE_PATH, "r");
    if (!f) return false;

    char line[1024];
    bool found = false;
    int  rec_idx = -1;  // -1 = header line not yet consumed
    while (fgets(line, sizeof(line), f)) {
        if (rec_idx < 0) { rec_idx = 0; continue; }  // skip the <EOH> header line
        if (rec_idx == idx) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
            strncpy(out, line, out_sz - 1);
            out[out_sz - 1] = '\0';
            found = true;
            break;
        }
        rec_idx++;
    }

    fclose(f);
    return found;
}

// A station worked twice during one activation counts ONCE toward POTA's
// 10-QSO (SOTA's 4-QSO) minimum, no matter the band or mode - the rule
// exists to prove contact with N different people, not to reward working
// the same one repeatedly. This was NOT applied before (Eric, GitHub
// issue): the raw record count let a duplicate inflate the number the
// device shows, so "10 contacts logged, park activated" could read true on
// screen while POTA.app credited 9 and the activation failed on upload -
// exactly what he reported (two QSOs with KO4JON, device said 10, POTA
// credited 9, one short). Deliberately callsign-only, not
// callsign+band/mode: that is the conservative direction - it can only ever
// show a number LESS THAN OR EQUAL to what POTA would credit, never claim
// activation early the way the old count could. If a future report shows
// POTA crediting the same station twice on different bands, this needs
// revisiting; until then, a missing extra credit is a far smaller error
// than the one Eric hit.
#define ADIF_ACT_MAX_TRACKED 512   // stations tracked for dedup; past this,
                                   // still counted, just not re-checked -
                                   // a >512-unique-station activation isn't
                                   // a realistic field session

/* #263 - the ONE completeness walk. See adif_log.h for why it is not two.
 *
 * Runs on whichever task asked (httpd for the endpoint, taskLVGL for the
 * Activation modal): one buffered read of a file measured at ~6 ms for a few
 * hundred records, no allocation, and the caller owns the problem array. */
void adif_log_check(bool activating, adif_log_check_t *out,
                    adif_log_problem_t *problems, int max_problems)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->total = adif_log_count();
    if (!s_mounted) return;

    FILE *f = fopen(FILE_PATH, "r");
    if (!f) return;

    char line[1024];
    int  rec = -1;
    bool header_done = false;
    while (fgets(line, sizeof(line), f)) {
        if (!header_done) { header_done = true; continue; }
        if (!line[0] || line[0] == '\n') continue;
        rec++;
        out->checked++;

        char call[24] = "", date[16] = "", tm[12] = "", band[12] = "",
             mode[12] = "", stn[24] = "", mysig[24] = "", sig[24] = "";
        adif_log_extract_field(line, "CALL",             call,  sizeof(call));
        adif_log_extract_field(line, "QSO_DATE",         date,  sizeof(date));
        adif_log_extract_field(line, "TIME_ON",          tm,    sizeof(tm));
        adif_log_extract_field(line, "BAND",             band,  sizeof(band));
        adif_log_extract_field(line, "MODE",             mode,  sizeof(mode));
        adif_log_extract_field(line, "STATION_CALLSIGN", stn,   sizeof(stn));
        adif_log_extract_field(line, "MY_SIG_INFO",      mysig, sizeof(mysig));
        adif_log_extract_field(line, "SIG_INFO",         sig,   sizeof(sig));

        adif_check_fields_t fl = {
            .call = call, .qso_date = date, .time_on = tm, .band = band,
            .mode = mode, .station_call = stn,
            .my_sig_info = mysig, .sig_info = sig,
        };
        uint32_t bad = adif_check_record(&fl, activating);
        if (!bad) continue;

        out->all_flags |= bad;
        out->with_problems++;
        if (problems && out->listed < max_problems) {
            adif_log_problem_t *e = &problems[out->listed++];
            e->idx   = rec;
            e->flags = bad;
            snprintf(e->call, sizeof(e->call), "%s", call[0] ? call : "(none)");
        }
    }
    fclose(f);
}

int adif_log_count_activation(const char *sig_info)
{
    if (!s_mounted || !sig_info || !sig_info[0]) return 0;

    // One pass over the file. Deliberately NOT a loop over
    // adif_log_get_record(), which reopens and re-scans the file per record -
    // that is O(n^2) file I/O on SPIFFS for a number shown on a modal.
    FILE *f = fopen(FILE_PATH, "r");
    if (!f) return 0;

    char needle[32];
    snprintf(needle, sizeof(needle), "<MY_SIG_INFO:%u>%s", (unsigned)strlen(sig_info), sig_info);

    // Heap (PSRAM), never the stack: this runs from activation_modal.c's
    // refresh() on taskLVGL, which CLAUDE.md already documents as having
    // only ~8 KB - a 512*16 array would be a guaranteed stack-protection
    // fault, not a maybe.
    char (*seen)[ADIF_CALL_MAX] =
        heap_caps_malloc((size_t)ADIF_ACT_MAX_TRACKED * ADIF_CALL_MAX, MALLOC_CAP_SPIRAM);
    int seen_n = 0;

    char line[1024];
    int  n = 0;
    bool header_done = false;
    while (fgets(line, sizeof(line), f)) {
        if (!header_done) { header_done = true; continue; }   // <EOH>
        // Case-insensitive because a config import or a hand-edited log may
        // carry a differently-cased reference than the one being counted.
        const char *p = line;
        size_t nl = strlen(needle);
        bool hit = false;
        for (; *p; p++) {
            if (strncasecmp(p, needle, nl) == 0) { hit = true; break; }
        }
        if (!hit) continue;

        char call[ADIF_CALL_MAX];
        if (!seen || !adif_log_extract_field(line, "CALL", call, sizeof(call))) {
            n++;   // no CALL field, or the tracking buffer didn't allocate -
            continue;  // over-counting one record is safer than losing it
        }

        bool dup = false;
        for (int i = 0; i < seen_n; i++) {
            if (strcasecmp(seen[i], call) == 0) { dup = true; break; }
        }
        if (dup) continue;

        n++;
        if (seen_n < ADIF_ACT_MAX_TRACKED) {
            strcpy(seen[seen_n], call);   // call[] and seen[][] are both
            seen_n++;                    // ADIF_CALL_MAX, already NUL-terminated
        }
    }
    if (seen) free(seen);
    fclose(f);
    return n;
}

/* Case-insensitive strstr - ADIF tags are conventionally upper case but the
 * spec does not require it, and a record we wrote is not the only record this
 * could ever be asked to edit. */
static char *strcasestr_local(char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) return p;
    }
    return NULL;
}

/* Replace, add or remove ONE field in ONE logged record.
 *
 * Gyula HA3HZ: the web log viewer is called "View / edit log" and the report
 * column looked editable, but nothing could be changed - only whole-record
 * delete existed. If a field looks editable it must edit, and correcting a value
 * by hand is a fair thing to want in a log.
 *
 * value == NULL or "" REMOVES the field. That matters here rather than being a
 * convenience: an absent RST is the honest representation of "never exchanged"
 * (v1.3.4 deliberately stopped writing a fabricated "599"), so the operator must
 * be able to get back to absent, not just to some other number.
 *
 * Same temp-file-and-rename walk as adif_log_delete_record(), including the
 * fsync before rename that CLAUDE.md requires. Deliberately does NOT touch the
 * QRZ/eQSL/LoTW upload cursors: an edit does not change how many records have
 * been uploaded, and rewinding a cursor to "correct" a remote copy would
 * re-upload the entire log. The remote copy of an already-uploaded QSO stays as
 * it was - the caller is responsible for saying so.
 */
bool adif_log_set_field(int idx, const char *field, const char *value)
{
    if (!s_mounted || idx < 0 || !field || !field[0]) return false;
    size_t flen = strlen(field);
    if (flen > 24) return false;

    const char *TMP_PATH = "/spiffs/qso.tmp";
    FILE *in = fopen(FILE_PATH, "r");
    if (!in) return false;
    FILE *out = fopen(TMP_PATH, "w");
    if (!out) { fclose(in); return false; }

    char open_tag[32];
    snprintf(open_tag, sizeof(open_tag), "<%s:", field);

    char line[1024];
    int  rec = -1;
    bool edited = false;
    while (fgets(line, sizeof(line), in)) {
        if (rec < 0) { fputs(line, out); rec = 0; continue; }   /* header */
        if (rec != idx) { fputs(line, out); rec++; continue; }

        /* Rebuild this one record: copy it, dropping any existing instance of
         * the field, then append the new one before <EOR>. Working on a copy
         * keeps the parse simple and the original intact if anything fails. */
        char rebuilt[1024];
        size_t o = 0;
        const char *p = line;
        while (*p) {
            if (*p == '<' && strncasecmp(p, open_tag, flen + 2) == 0) {
                /* <FIELD:len>value - skip the tag AND its len bytes of value. */
                const char *colon = p + flen + 2;
                int vlen = atoi(colon);
                const char *gt = strchr(colon, '>');
                if (!gt || vlen < 0) break;            /* malformed - bail out */
                p = gt + 1 + vlen;
                while (*p == ' ') p++;                 /* and its separator */
                continue;
            }
            if (o + 1 >= sizeof(rebuilt)) break;
            rebuilt[o++] = *p++;
        }
        rebuilt[o] = '\0';

        /* Strip the trailing <EOR> (and any whitespace) so the new field goes
         * before it, which is what makes the record still parse. */
        char *eor = strcasestr_local(rebuilt, "<EOR>");
        if (!eor) { fputs(line, out); rec++; continue; }   /* not a record */
        *eor = '\0';

        char newrec[1200];
        if (value && value[0]) {
            snprintf(newrec, sizeof(newrec), "%s<%s:%u>%s <EOR>\n",
                     rebuilt, field, (unsigned)strlen(value), value);
        } else {
            snprintf(newrec, sizeof(newrec), "%s<EOR>\n", rebuilt);
        }
        fputs(newrec, out);
        edited = true;
        rec++;
    }
    fclose(in);

    /* ⛔ Verify the rewrite BEFORE committing it - the same rule, and the same
     * bug, as adif_log_delete_record() carried until v1.9.5: `edited` is set
     * purely from the READ side, so a write that failed partway (SPIFFS
     * returns ENOSPC on this filesystem even with free space; see CLAUDE.md)
     * would have deleted the good log and renamed a truncated temp over it
     * while still reporting success. This path is no longer only used for the
     * occasional report correction - it is how a Park-to-Park reference gets
     * added to a whole activation's worth of QSOs - so it gets the same
     * treatment rather than waiting to be reported. */
    bool write_ok = (ferror(out) == 0);
    if (write_ok) {
        fflush(out);
        fsync(fileno(out));
        write_ok = (ferror(out) == 0);
    }
    if (fclose(out) != 0) write_ok = false;

    if (!edited || !write_ok) {
        remove(TMP_PATH);
        if (edited && !write_ok) {
            ESP_LOGE(TAG, "set_field: rewrite failed (storage full?) - original log left untouched");
            ui_toast("Edit failed - storage full? Log unchanged");
        }
        return false;
    }
    remove(FILE_PATH);
    if (rename(TMP_PATH, FILE_PATH) != 0) {
        ESP_LOGE(TAG, "set_field: rename %s -> %s failed", TMP_PATH, FILE_PATH);
        ui_toast("Edit failed - could not replace the log file");
        return false;
    }

    ESP_LOGI(TAG, "QSO #%d: %s = '%s'", idx, field, value ? value : "(removed)");
    sd_archive_mark_adif_dirty();
    return true;
}

// Delete MANY records in ONE rewrite (#325).
//
// ⛔ Do not implement a multi-delete as a loop over adif_log_delete_record().
// That was the original shape and it is O(N²): each call rewrites the whole
// file, so removing 500 of 525 records measured **0.25 deletions/second** on
// the dev bench - about half an hour, and ~46 MB of SPIFFS writes on the
// partition that also holds the LoTW private key. The caller ran it on
// taskLVGL too, so the UI was dead throughout and the operator saw a device
// that looked crashed ("I pressed Sure? then nothing happens.... for like
// 2min?"). This does one pass instead, and the cost is the same as deleting
// one record.
//
// `want_gone(idx, raw, ctx)` decides per record. Returns how many were removed,
// or -1 on failure with the log left untouched.
//
// ⚠ Still not for taskLVGL: one rewrite of a large log is hundreds of ms.
int adif_log_delete_matching(bool (*want_gone)(int idx, const char *raw, void *ctx),
                             void *ctx)
{
    if (!s_mounted || !want_gone) return -1;

    const char *TMP_PATH = "/spiffs/qso.tmp";
    FILE *in = fopen(FILE_PATH, "r");
    if (!in) { ESP_LOGE(TAG, "batch delete: could not open %s (errno %d)", FILE_PATH, errno); return -1; }
    FILE *out = fopen(TMP_PATH, "w");
    if (!out && errno == ENOSPC) {
        // Same live self-heal as the single-record path above.
        esp_spiffs_check("storage");
        esp_spiffs_gc("storage", 65536);
        out = fopen(TMP_PATH, "w");
    }
    if (!out) {
        ESP_LOGE(TAG, "batch delete: could not open %s for write (errno %d)", TMP_PATH, errno);
        fclose(in);
        return -1;
    }

    char line[1024];
    int  rec = -1;
    int  removed = 0;
    // Cursors must follow every deletion that sits BELOW them - same rule as
    // the single delete, counted as we go rather than applied per record.
    int  removed_below_qrz = 0, removed_below_eqsl = 0, removed_below_lotw = 0;
    // ⛔ NOT settings_load_all() - that is a multi-kilobyte struct on the
    // stack and this runs on a small worker task. Doing it the obvious way
    // crashed adif_delt with a Stack protection fault (2026-09-06); see the
    // task-stack section in CLAUDE.md, which this is now another instance of.
    uint32_t cur_qrz = 0, cur_eqsl = 0, cur_lotw = 0;
    settings_get_upload_cursors(&cur_qrz, &cur_eqsl, &cur_lotw);

    while (fgets(line, sizeof(line), in)) {
        if (rec < 0) { fputs(line, out); rec = 0; continue; }   // keep header
        if (want_gone(rec, line, ctx)) {
            removed++;
            if ((uint32_t)rec < cur_qrz)  removed_below_qrz++;
            if ((uint32_t)rec < cur_eqsl) removed_below_eqsl++;
            if ((uint32_t)rec < cur_lotw) removed_below_lotw++;
        } else {
            fputs(line, out);
        }
        rec++;
    }
    fclose(in);

    bool write_ok = (ferror(out) == 0);
    if (write_ok) {
        fflush(out);
        fsync(fileno(out));
        write_ok = (ferror(out) == 0);
    }
    if (fclose(out) != 0) write_ok = false;

    if (!write_ok) {
        remove(TMP_PATH);
        ESP_LOGE(TAG, "batch delete: rewrite failed (storage full?) - log left untouched");
        ui_toast("Delete failed - storage full? Log unchanged");
        return -1;
    }
    if (removed == 0) { remove(TMP_PATH); return 0; }   // nothing matched

    remove(FILE_PATH);
    if (rename(TMP_PATH, FILE_PATH) != 0) {
        ESP_LOGE(TAG, "batch delete: rename %s -> %s failed", TMP_PATH, FILE_PATH);
        ui_toast("Delete failed - could not replace the log file");
        return -1;
    }

    if (removed_below_qrz)  settings_set_qrz_uploaded_n(cur_qrz   - removed_below_qrz);
    if (removed_below_eqsl) settings_set_eqsl_uploaded_n(cur_eqsl - removed_below_eqsl);
    if (removed_below_lotw) settings_set_lotw_uploaded_n(cur_lotw - removed_below_lotw);

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count        = 0;
    s_worked_count = 0;
    if (s_lock) xSemaphoreGive(s_lock);
    load_from_file();

    ESP_LOGI(TAG, "batch delete: removed %d record(s) in one rewrite (%d remain)", removed, s_count);
    sd_archive_mark_adif_dirty();
    return removed;
}

bool adif_log_delete_record(int idx)
{
    // Every early return here used to be silent - the caller (adif_view_modal)
    // only ever knew "failed", never WHY, and neither did anyone reading the
    // log afterwards. Found necessary in practice: a delete attempt that
    // logged plain "delete record #N failed" with none of these lines firing
    // means the failure is at fopen(TMP_PATH), which this file had zero
    // visibility into until now.
    if (!s_mounted) { ESP_LOGE(TAG, "delete record #%d: log not mounted", idx); return false; }
    if (idx < 0)    { ESP_LOGE(TAG, "delete record #%d: negative index", idx); return false; }

    // Rewrite the file to a temp, skipping record idx (line-oriented: header
    // first, then one record per line - same walk as adif_log_get_record).
    const char *TMP_PATH = "/spiffs/qso.tmp";
    FILE *in = fopen(FILE_PATH, "r");
    if (!in) { ESP_LOGE(TAG, "delete record #%d: could not open %s for read (errno %d)", idx, FILE_PATH, errno); return false; }
    FILE *out = fopen(TMP_PATH, "w");
    if (!out && errno == ENOSPC) {
        // Live self-heal, not just a boot-time one: a unit whose SPIFFS has
        // accumulated orphaned/inconsistent blocks (see adif_log_init()'s
        // esp_spiffs_check()/esp_spiffs_gc() and its comment for the field
        // case this came from) reports ENOSPC on a write this small even
        // mid-session, and making the operator reboot just to delete one
        // record is a bad answer when the fix is two function calls. Repair
        // AND actively reclaim (check() alone fixes consistency but does not
        // promise to free space - field-tested 2026-08-25: it reported
        // success and the retry still hit ENOSPC until gc() was added too),
        // then retry once.
        size_t t0 = 0, u0 = 0, t1 = 0, u1 = 0;
        esp_spiffs_info("storage", &t0, &u0);
        esp_err_t chk = esp_spiffs_check("storage");
        esp_err_t gc  = esp_spiffs_gc("storage", 65536);
        esp_spiffs_info("storage", &t1, &u1);
        ESP_LOGW(TAG, "delete record #%d: %s ENOSPC on open - check=%s gc=%s, "
                      "used %zuKB/%zuKB -> %zuKB/%zuKB, retrying once",
                 idx, TMP_PATH, esp_err_to_name(chk), esp_err_to_name(gc),
                 u0 / 1024, t0 / 1024, u1 / 1024, t1 / 1024);
        out = fopen(TMP_PATH, "w");
    }
    if (!out) {
        ESP_LOGE(TAG, "delete record #%d: could not open %s for write (errno %d)", idx, TMP_PATH, errno);
        fclose(in);
        return false;
    }

    char line[1024];
    int  rec = -1;         // -1 = header line pending
    bool removed = false;
    while (fgets(line, sizeof(line), in)) {
        if (rec < 0) { fputs(line, out); rec = 0; continue; }   // keep header
        if (rec == idx) { removed = true; rec++; continue; }    // skip = delete
        fputs(line, out);
        rec++;
    }
    fclose(in);

    // ⛔ SAME RULE AS LOGGING A QSO: do not act on a write that may not have
    // happened. This used to skip straight to fflush/fsync/close with none of
    // their results checked, then delete the GOOD file and rename the temp
    // one over it regardless - so a write that silently failed partway
    // (SPIFFS ENOSPC-despite-free-space is a documented case on this
    // filesystem; see CLAUDE.md) would have replaced the real log with a
    // truncated one while `removed` (set purely from the READ side above)
    // still reported success. Reported as "the delete UI runs through its
    // whole motion but the record is still there afterwards" - which is
    // consistent with the rewrite failing and the ORIGINAL file therefore
    // being left alone by the fixed code below, not with anything on the
    // read/index side (the confirm bar's own preview text, built from the
    // same index, was never reported wrong).
    bool write_ok = (ferror(out) == 0);
    if (write_ok) {
        fflush(out);
        fsync(fileno(out));
        write_ok = (ferror(out) == 0);
    }
    if (fclose(out) != 0) write_ok = false;

    if (!removed || !write_ok) {
        remove(TMP_PATH);
        if (removed && !write_ok) {
            ESP_LOGE(TAG, "delete record #%d: rewrite failed (storage full?) - original log left untouched", idx);
            ui_toast("Delete failed - storage full? Log unchanged");
        }
        return false;
    }
    remove(FILE_PATH);
    if (rename(TMP_PATH, FILE_PATH) != 0) {
        ESP_LOGE(TAG, "delete: rename %s -> %s failed", TMP_PATH, FILE_PATH);
        ui_toast("Delete failed - could not replace the log file");
        return false;
    }

    // Upload cursors are counts into the record sequence: a deletion BELOW a
    // cursor shifts every later record down one, so the cursor must follow.
    // A deletion at/after the cursor is a not-yet-uploaded record - no shift.
    qmx_settings_t qs;
    settings_load_all(&qs);
    if ((uint32_t)idx < qs.qrz_uploaded_n)  settings_set_qrz_uploaded_n(qs.qrz_uploaded_n - 1);
    if ((uint32_t)idx < qs.eqsl_uploaded_n) settings_set_eqsl_uploaded_n(qs.eqsl_uploaded_n - 1);
    if ((uint32_t)idx < qs.lotw_uploaded_n) settings_set_lotw_uploaded_n(qs.lotw_uploaded_n - 1);

    // Rebuild count + worked cache from the rewritten file (the deleted
    // record may have been the only QSO with that call/band).
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count        = 0;
    s_worked_count = 0;
    if (s_lock) xSemaphoreGive(s_lock);
    load_from_file();

    ESP_LOGI(TAG, "Deleted QSO record #%d (%d remain)", idx, s_count);
    sd_archive_mark_adif_dirty();   // re-mirror the edited file to SD
    return true;
}

void adif_log_clear(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count       = 0;
    s_worked_count = 0;
    if (s_lock) xSemaphoreGive(s_lock);

    remove(FILE_PATH);
    FILE *f = fopen(FILE_PATH, "w");
    if (f) { write_header(f); fclose(f); }

    // The QRZ/eQSL/LoTW upload cursors are record counts into this log; with
    // the log gone they must go back to 0 or every QSO logged after the clear
    // sits below the stale cursor and silently never uploads.
    settings_set_qrz_uploaded_n(0);
    settings_set_eqsl_uploaded_n(0);
    settings_set_lotw_uploaded_n(0);
    ESP_LOGI(TAG, "ADIF log cleared");
}

// Case-insensitive substring search over a bounded, read-only buffer. Not
// strstr_local() above - that one wants a mutable char* for its other
// caller and this one is read-only end to end, including the caller's
// const char *adif_text.
static const char *find_ci(const char *hay, const char *hay_end, const char *needle)
{
    size_t nlen = strlen(needle);
    if ((size_t)(hay_end - hay) < nlen) return NULL;
    for (const char *p = hay; p + nlen <= hay_end; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

// Restore records from a raw ADIF file - Randy N4OPI, after a clean
// erase-and-reinstall: he had downloaded his log first but found no way to
// get the worked-station history back onto the device, only overwrite it
// (the existing config import explicitly does NOT touch the QSO log; see
// its own header comment). This is that missing other half.
//
// The imported file is treated as UNTRUSTED input in one specific way: an
// external tool (WSJT-X, ADIFMaster, or a spreadsheet round-trip) is free to
// pretty-print one record across several lines, while everything else in
// this module assumes one record per line (see load_from_file()'s own
// comment). So each record is read as a whole blob between <EOR> markers,
// stripped of embedded newlines, and rewritten as a single line before it
// ever reaches this file - the SAME normalisation adif_log_record() already
// produces, just arriving from outside instead of from a live QSO.
//
// Deduplicated against the CURRENT log by CALL+QSO_DATE+TIME_ON before
// writing, so re-importing the same file twice (or importing a file that
// overlaps what a partial earlier import already restored) is a no-op for
// the overlapping records rather than a second copy of them.
int adif_log_import(const char *adif_text)
{
    adif_import_result_t discard;
    return adif_log_import_ex(adif_text, &discard);
}

int adif_log_import_ex(const char *adif_text, adif_import_result_t *res)
{
    adif_import_result_t local;
    if (!res) res = &local;
    memset(res, 0, sizeof(*res));

    if (!s_mounted || !adif_text) return -1;

    size_t total_len = strlen(adif_text);
    const char *end = adif_text + total_len;

    // Skip the IMPORTED file's own header if it has one. Headerless input
    // (e.g. a handful of records pasted without the <ADIF_VER.../<EOH>
    // preamble) is accepted too - the loop below just starts at the top.
    const char *cursor = adif_text;
    const char *eoh = find_ci(adif_text, end, "<eoh>");
    if (eoh) cursor = eoh + 5;

    // Snapshot every CALL+QSO_DATE+TIME_ON already in the log, once, so each
    // imported record is checked against it in memory rather than re-reading
    // the file from disk per candidate. Sized to the current count with a
    // little slack; a record beyond the cap just risks an occasional
    // duplicate rather than failing the whole import.
    typedef struct { char call[ADIF_CALL_MAX]; char date[9]; char time[7]; } key_t;
    int cap = s_count + 64;
    key_t *existing = heap_caps_malloc(sizeof(key_t) * cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int n_existing = 0;
    if (existing) {
        FILE *rf = fopen(FILE_PATH, "r");
        if (rf) {
            char line[512];
            while (n_existing < cap && fgets(line, sizeof(line), rf)) {
                if (!strstr(line, "<EOR>")) continue;
                key_t *k = &existing[n_existing];
                if (!adif_log_extract_field(line, "CALL", k->call, sizeof(k->call))) continue;
                if (!adif_log_extract_field(line, "QSO_DATE", k->date, sizeof(k->date))) k->date[0] = '\0';
                if (!adif_log_extract_field(line, "TIME_ON", k->time, sizeof(k->time))) k->time[0] = '\0';
                n_existing++;
            }
            fclose(rf);
        }
    }

    FILE *f = fopen(FILE_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "ADIF import: cannot open %s for append", FILE_PATH);
        if (existing) free(existing);
        return -1;
    }

    int added = 0;
    char norm[512];

    while (cursor < end) {
        const char *eor = find_ci(cursor, end, "<eor>");
        if (!eor) break;

        // Normalise the blob [cursor, eor) into one line: newlines become
        // spaces (ADIF fields are self-delimiting by their <NAME:len> prefix,
        // so collapsing the layout around them changes nothing a parser
        // reads), leading whitespace trimmed, then <EOR>\n appended - the
        // exact shape adif_log_record() itself writes.
        const char *p = cursor;
        while (p < eor && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        size_t blen = (size_t)(eor - p);
        res->found++;
        if (blen == 0 || blen >= sizeof(norm) - 8) {
            // Too long to hold as one record. Counted, never silently dropped:
            // a skipped record reported as "already in the log" is a false
            // statement about someone's log.
            if (blen) res->unreadable++;
            cursor = eor + 5;
            continue;
        }
        size_t w = 0;
        for (size_t i = 0; i < blen; i++) {
            char c = p[i];
            norm[w++] = (c == '\r' || c == '\n') ? ' ' : c;
        }
        norm[w] = '\0';
        cursor = eor + 5;   // past "<eor>"

        if (!strchr(norm, '<')) { res->unreadable++; continue; }   // nothing tagged

        char call[ADIF_CALL_MAX], date[9], time_on[7], band[ADIF_BAND_MAX], freq_str[16];
        if (!adif_log_extract_field(norm, "CALL", call, sizeof(call))) { res->unreadable++; continue; }  // no call to key on
        if (!adif_log_extract_field(norm, "QSO_DATE", date, sizeof(date))) date[0] = '\0';
        if (!adif_log_extract_field(norm, "TIME_ON", time_on, sizeof(time_on))) time_on[0] = '\0';

        bool dup = false;
        for (int i = 0; i < n_existing; i++) {
            if (strcmp(existing[i].call, call) == 0 &&
                strcmp(existing[i].date, date) == 0 &&
                strcmp(existing[i].time, time_on) == 0) { dup = true; break; }
        }
        if (dup) { res->duplicate++; continue; }

        fprintf(f, "%s<EOR>\n", norm);

        if (!adif_log_extract_field(norm, "BAND", band, sizeof(band))) {
            band[0] = '\0';
            if (adif_log_extract_field(norm, "FREQ", freq_str, sizeof(freq_str))) {
                strncpy(band, freq_to_band((uint32_t)(atof(freq_str) * 1e6)), sizeof(band) - 1);
                band[sizeof(band) - 1] = '\0';
            }
        }
        cache_add(call, band);
        added++;
        res->added++;

        // Also guard against a duplicate appearing TWICE within this same
        // import (a file concatenated from two exports, say), not just
        // against what was already on the device.
        if (existing && n_existing < cap) {
            strncpy(existing[n_existing].call, call, sizeof(existing[n_existing].call) - 1);
            existing[n_existing].call[sizeof(existing[n_existing].call) - 1] = '\0';
            strncpy(existing[n_existing].date, date, sizeof(existing[n_existing].date) - 1);
            existing[n_existing].date[sizeof(existing[n_existing].date) - 1] = '\0';
            strncpy(existing[n_existing].time, time_on, sizeof(existing[n_existing].time) - 1);
            existing[n_existing].time[sizeof(existing[n_existing].time) - 1] = '\0';
            n_existing++;
        }
    }

    bool write_ok = (ferror(f) == 0);
    if (write_ok) { fflush(f); fsync(fileno(f)); }
    if (fclose(f) != 0) write_ok = false;
    if (existing) free(existing);

    if (!write_ok) {
        ESP_LOGE(TAG, "ADIF import: write failed (storage full?), %d record(s) may be lost", added);
        load_from_file();   // resync count/cache to whatever actually landed on disk
        return -1;
    }

    if (added > 0) {
        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        s_count += added;
        if (s_lock) xSemaphoreGive(s_lock);
        sd_archive_mark_adif_dirty();
    }
    ESP_LOGI(TAG, "ADIF import: %d found, %d added, %d duplicate, %d unreadable; log now has %d",
             res->found, res->added, res->duplicate, res->unreadable, s_count);
    return added;
}

// Restore from the card's own mirror. All the ADIF work is the existing
// import; the only new part is getting the bytes off the card, which
// sd_archive owns (it holds the mount, the paths and the lock).
int adif_log_import_from_sd(adif_import_result_t *res)
{
    adif_import_result_t local;
    if (!res) res = &local;
    memset(res, 0, sizeof(*res));

    // BOTH files on the card, not just qso.adi.
    //
    // Gyula HA3HZ renamed his own saved backups to qso.prev.adi, put them on the
    // card, pressed this button and was told "nothing to restore" - because it
    // only ever read qso.adi, which was simply the device's current log. He did
    // nothing wrong: qso.prev.adi is the name the firmware itself writes, the
    // docs called it recoverable, and the only route offered was a file upload
    // from a PC. "Restore from the card" should mean everything the card knows.
    //
    // Safe to merge blind: both are our own files, the import already skips a
    // contact that is present, and reading them oldest-last means the newer copy
    // wins any tie. Neither present is the only real failure.
    int total_added = 0;
    bool read_any = false;

    for (int pass = 0; pass < 2; pass++) {
        size_t len = 0;
        char *text = sd_archive_read_adif_file(pass == 1, &len);
        if (!text) continue;
        read_any = true;

        adif_import_result_t r;
        int added = adif_log_import_ex(text, &r);
        free(text);

        res->found      += r.found;
        res->added      += r.added;
        res->duplicate  += r.duplicate;
        res->unreadable += r.unreadable;
        if (added > 0) total_added += added;
        else if (added < 0) return -1;   // the log could not be written - stop
    }

    if (!read_any) return -1;   // no card, or neither file on it
    return total_added;
}

const char *adif_log_band_for_freq(uint32_t hz)
{
    return freq_to_band(hz);
}
