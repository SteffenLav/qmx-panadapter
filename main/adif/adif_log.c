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

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "sd_archive.h"
#include "settings.h"   // upload-cursor adjustment in adif_log_delete_record()
#include <unistd.h>     // fsync
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
// '<'-anchored, so "CALL" never matches inside "MY_CALL", etc.
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

    static const char *bad_fields[] = { "<SUBMODE:3>FT8", "<SUBMODE:3>FT4" };
    bool changed = false;
    for (size_t b = 0; b < sizeof(bad_fields) / sizeof(bad_fields[0]); b++) {
        const char *needle = bad_fields[b];
        size_t nlen = strlen(needle);
        char *p;
        while ((p = strstr(buf, needle)) != NULL) {
            memmove(p, p + nlen, strlen(p + nlen) + 1);   // shift left, incl. NUL
            changed = true;
        }
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

    // Check free space and warn if low (< 64 KB).
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: %zu KB used / %zu KB total", used / 1024, total / 1024);
        if (total - used < 65536)
            ESP_LOGW(TAG, "SPIFFS nearly full - ADIF writes may fail");
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
    // FT8 and FT4 are both standalone leaf-level ADIF MODE values (not
    // submodes of MFSK), so no SUBMODE field belongs here. A prior version
    // wrote SUBMODE as a duplicate of MODE (e.g. MODE=FT4 SUBMODE=FT4) -
    // QRZ's logbook import rejected at least FT4 records with this pairing
    // ("Undefined message or mode"), most likely because its MODE/SUBMODE
    // validation table has no self-referential entry for either mode.
    write_field(f, "MODE",         qso->mode ? qso->mode : "FT8");
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
    write_field(f, "MY_CALL",      qso->my_call);
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

    fclose(f);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count++;
    cache_add(qso->their_call, freq_to_band(qso->freq_hz));
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Logged QSO #%d: %s @ %.4f MHz (%s/%s)",
             s_count, qso->their_call,
             (double)qso->freq_hz / 1e6,
             qso->rst_sent ? qso->rst_sent : "?",
             qso->rst_rcvd ? qso->rst_rcvd : "?");

    sd_archive_mark_adif_dirty();  // re-mirror the ADIF file to SD if a card is in
}

int adif_log_count(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

const char *adif_log_file_path(void)
{
    return FILE_PATH;
}

bool adif_log_contains_call(const char *call)
{
    if (!call || !call[0]) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < s_worked_count && !found; i++) {
        if (strcmp(s_worked[i].call, call) == 0) found = true;
    }
    xSemaphoreGive(s_lock);
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
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < s_worked_count && !found; i++) {
        if (strcmp(s_worked[i].call, call) == 0 &&
            (any_band || strcmp(s_worked[i].band, band) == 0)) found = true;
    }
    xSemaphoreGive(s_lock);
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
        if (hit) n++;
    }
    fclose(f);
    return n;
}

bool adif_log_delete_record(int idx)
{
    if (!s_mounted || idx < 0) return false;

    // Rewrite the file to a temp, skipping record idx (line-oriented: header
    // first, then one record per line - same walk as adif_log_get_record).
    const char *TMP_PATH = "/spiffs/qso.tmp";
    FILE *in = fopen(FILE_PATH, "r");
    if (!in) return false;
    FILE *out = fopen(TMP_PATH, "w");
    if (!out) { fclose(in); return false; }

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
    fflush(out);
    fsync(fileno(out));    // mandatory before rename - see CLAUDE.md fsync rule
    fclose(out);

    if (!removed) { remove(TMP_PATH); return false; }
    remove(FILE_PATH);
    if (rename(TMP_PATH, FILE_PATH) != 0) {
        ESP_LOGE(TAG, "delete: rename %s -> %s failed", TMP_PATH, FILE_PATH);
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
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count        = 0;
    s_worked_count = 0;
    xSemaphoreGive(s_lock);
    load_from_file();

    ESP_LOGI(TAG, "Deleted QSO record #%d (%d remain)", idx, s_count);
    sd_archive_mark_adif_dirty();   // re-mirror the edited file to SD
    return true;
}

void adif_log_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count       = 0;
    s_worked_count = 0;
    xSemaphoreGive(s_lock);

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

const char *adif_log_band_for_freq(uint32_t hz)
{
    return freq_to_band(hz);
}
