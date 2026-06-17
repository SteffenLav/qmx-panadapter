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

// Worked-call cache: unique callsigns from every logged QSO.
#define ADIF_WORKED_CACHE 1024
#define ADIF_CALL_MAX     16
static char s_worked[ADIF_WORKED_CACHE][ADIF_CALL_MAX];
static int  s_worked_count = 0;

// ---------------------------------------------------------------------------

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

// Add a callsign to the worked cache (deduplicates, ignores overflow).
static void cache_add(const char *call)
{
    if (!call || !call[0]) return;
    for (int i = 0; i < s_worked_count; i++) {
        if (strcmp(s_worked[i], call) == 0) return;  // already present
    }
    if (s_worked_count < ADIF_WORKED_CACHE) {
        strncpy(s_worked[s_worked_count], call, ADIF_CALL_MAX - 1);
        s_worked[s_worked_count][ADIF_CALL_MAX - 1] = '\0';
        s_worked_count++;
    }
}

// Scan the ADIF file and populate s_count + s_worked cache.
static void load_from_file(void)
{
    FILE *f = fopen(FILE_PATH, "r");
    if (!f) return;

    char line[512];
    int  count = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "<EOR>")) count++;

        // Extract <CALL:N>value
        char *p = line;
        while ((p = strstr(p, "<CALL:")) != NULL) {
            p += 6;  // skip "<CALL:"
            int len = atoi(p);
            char *close = strchr(p, '>');
            if (!close || len <= 0 || len >= ADIF_CALL_MAX) { p++; continue; }
            char call[ADIF_CALL_MAX];
            int  copy = (len < ADIF_CALL_MAX - 1) ? len : ADIF_CALL_MAX - 1;
            memcpy(call, close + 1, copy);
            call[copy] = '\0';
            cache_add(call);
            p = close + 1 + len;
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

// ---------------------------------------------------------------------------

void adif_log_init(void)
{
    s_lock = xSemaphoreCreateMutex();

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
    write_field(f, "MODE",         qso->mode ? qso->mode : "FT8");
    write_field(f, "SUBMODE",      qso->mode ? qso->mode : "FT8");
    write_field(f, "RST_SENT",     (qso->rst_sent && qso->rst_sent[0]) ? qso->rst_sent : "599");
    write_field(f, "RST_RCVD",     (qso->rst_rcvd && qso->rst_rcvd[0]) ? qso->rst_rcvd : "599");
    write_field(f, "QSO_DATE",     date_str);
    write_field(f, "TIME_ON",      time_str);
    write_field(f, "MY_CALL",      qso->my_call);
    write_field(f, "MY_GRIDSQUARE", qso->my_grid);
    write_field(f, "GRIDSQUARE",   qso->their_grid);
    fprintf(f, "<EOR>\n");

    fclose(f);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count++;
    cache_add(qso->their_call);
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Logged QSO #%d: %s @ %.4f MHz (%s/%s)",
             s_count, qso->their_call,
             (double)qso->freq_hz / 1e6,
             qso->rst_sent ? qso->rst_sent : "?",
             qso->rst_rcvd ? qso->rst_rcvd : "?");
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
        if (strcmp(s_worked[i], call) == 0) found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
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
    ESP_LOGI(TAG, "ADIF log cleared");
}
