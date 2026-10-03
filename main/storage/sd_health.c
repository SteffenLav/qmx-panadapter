#include "sd_health.h"
#include "sd_archive.h"
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
#define MAX_DEPTH          6
#define PATTERN_BYTES  65536
#define YIELD_EVERY   (256*1024)

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

static bool read_whole_file(const char *path, uint8_t *buf, uint64_t *bytes_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n;
    uint64_t since_yield = 0;
    bool ok = true;
    while ((n = fread(buf, 1, READ_CHUNK, f)) > 0) {
        *bytes_out  += n;
        since_yield += n;
        if (since_yield >= YIELD_EVERY) { since_yield = 0; vTaskDelay(1); }
        if (s_cancel) break;
    }
    if (ferror(f)) ok = false;
    fclose(f);
    return ok;
}

static void walk(const char *dir, uint8_t *buf, int depth)
{
    if (depth > MAX_DEPTH || s_cancel) return;
    DIR *d = opendir(dir);
    if (!d) { s_rep.read_errors++; return; }

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
            walk(path, buf, depth + 1);
        } else {
            s_rep.files_seen++;
            snprintf(s_rep.current, sizeof(s_rep.current), "%s", path);
            if (!read_whole_file(path, buf, &s_rep.bytes_read)) {
                s_rep.read_errors++;
                ESP_LOGE(TAG, "UNREADABLE: %s", path);
            }
        }
    }
    closedir(d);
}

static void write_verify(uint8_t *buf)
{
    const char *path = SD_MOUNT "/qmx-health.tmp";
    s_rep.write_verify_run = true;
    s_rep.write_verify_ok  = false;

    for (int i = 0; i < READ_CHUNK; i++) buf[i] = (uint8_t)(i * 7 + 13);

    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "write-verify: cannot create the test file"); return; }
    size_t written = 0;
    while (written < PATTERN_BYTES) {
        if (fwrite(buf, 1, READ_CHUNK, f) != READ_CHUNK) { fclose(f); unlink(path); return; }
        written += READ_CHUNK;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    f = fopen(path, "rb");
    if (!f) { unlink(path); return; }
    uint8_t cmp[64];
    bool ok = true;
    size_t total = 0;
    while (total < PATTERN_BYTES && ok) {
        size_t n = fread(cmp, 1, sizeof(cmp), f);
        if (n == 0) { ok = false; break; }
        for (size_t i = 0; i < n; i++) {
            size_t off = (total + i) % READ_CHUNK;
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
    if (s_rep.read_errors && !s_rep.write_verify_ok)
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "This card is failing: %lu file(s) unreadable and the write test did not "
                 "come back. Copy what you want off it and replace it.",
                 (unsigned long)s_rep.read_errors);
    else if (!s_rep.write_verify_ok)
        snprintf(s_rep.verdict, sizeof(s_rep.verdict),
                 "The card accepted a write and did not give it back. That is how a "
                 "counterfeit or worn-out card behaves. Replace it.");
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
    uint8_t *buf = malloc(READ_CHUNK);
    if (!buf) {
        s_rep.state = SD_HEALTH_FAILED;
        snprintf(s_rep.verdict, sizeof(s_rep.verdict), "Not enough memory to run the check.");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint64_t total = 0, freeb = 0;
    if (sd_archive_get_free_bytes(&freeb, &total)) {
        s_rep.total_bytes = total;
        s_rep.free_bytes  = freeb;
    }

    ESP_LOGW(TAG, "card check starting - reading every file, this takes a while");
    walk(SD_MOUNT, buf, 0);
    if (!s_cancel) write_verify(buf);
    verdict();
    s_rep.current[0] = 0;
    s_rep.state = SD_HEALTH_DONE;
    ESP_LOGW(TAG, "card check done: %lu files, %llu bytes read, %lu read error(s), "
                  "%lu fragment(s), write-verify %s",
             (unsigned long)s_rep.files_seen, (unsigned long long)s_rep.bytes_read,
             (unsigned long)s_rep.read_errors, (unsigned long)s_rep.suspect_names,
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
