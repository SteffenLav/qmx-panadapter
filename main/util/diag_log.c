#include "diag_log.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>     // fsync
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "psram_task.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "hal/efuse_hal.h"

#include "cat.h"             // cat_get_qmx_fw / cat_is_ready / freq / mode
#include "wifi.h"            // wifi_is_connected / ssid / ip / rssi
#include "settings.h"        // callsign / grid

static const char *TAG = "diag";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external-pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic/exception";
    case ESP_RST_INT_WDT:   return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:  return "task-watchdog";
    case ESP_RST_WDT:       return "other-watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep-wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

// Emit the self-identifying header. Runs with capture already enabled, so
// every line lands in the ring as well as the serial console. Keeps every
// fact a remote bug report needs in one place, regardless of whether logging
// was on from boot or toggled on later (radio/WiFi fields just read "no"
// until those subsystems are up).
void diag_log_write_session_header(void)
{
    ESP_LOGI(TAG, "==== diagnostic logging session start ====");

    const esp_app_desc_t *d = esp_app_get_description();
    if (d) {
        ESP_LOGI(TAG, "tab5_fw=%s project=%s idf=%s built=%s %s",
                 d->version, d->project_name, d->idf_ver, d->date, d->time);
    }

    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    ESP_LOGI(TAG, "serial(MAC)=%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t rev = efuse_hal_chip_revision();
    ESP_LOGI(TAG, "chip=%s rev v%u.%u cores=%d",
             chip.model == CHIP_ESP32P4 ? "ESP32-P4" : "unknown",
             (unsigned)(rev / 100), (unsigned)(rev % 100), chip.cores);

    ESP_LOGI(TAG, "reset_reason=%s uptime=%llus",
             reset_reason_str(esp_reset_reason()),
             (unsigned long long)(esp_timer_get_time() / 1000000));

    ESP_LOGI(TAG, "heap: internal_free=%.1fkB psram_free=%.2fMB psram_total=%.0fMB",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0f,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024.0f * 1024.0f),
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024.0f * 1024.0f));

    qmx_settings_t s;
    settings_load_all(&s);
    ESP_LOGI(TAG, "operator: callsign='%s' grid='%s'",
             s.my_callsign[0] ? s.my_callsign : "(unset)",
             s.my_grid[0]     ? s.my_grid     : "(unset)");

    ESP_LOGI(TAG, "wifi: configured_ssid='%s' connected=%s ip=%s rssi=%ddBm",
             s.wifi_ssid[0] ? s.wifi_ssid : "(none)",
             wifi_is_connected() ? "yes" : "no",
             wifi_get_ip(), wifi_get_rssi_dbm());

    const char *qfw = cat_get_qmx_fw();
    ESP_LOGI(TAG, "qmx: connected=%s fw=%s freq=%luHz mode=%s",
             cat_is_ready() ? "yes" : "no",
             (qfw && qfw[0]) ? qfw : "(unknown)",
             (unsigned long)cat_get_frequency(),
             cat_get_mode_str());

    ESP_LOGI(TAG, "==== (CAT TX/RX now logged per-line below) ====");
}

// 5 MB of rolling history, PSRAM-backed (the Tab5 has ~30 MB free, so this is
// cheap). Now that capture is always-on (no opt-in) and the SD mirror persists
// it off-chip, a generous ring keeps a long session's worth in RAM too. With
// the CAT poll logging de-duplicated (see cat.c — identical FA/MD/FW poll
// responses are dropped, only changes + a heartbeat remain), steady state is
// ~0 lines/s so 5 MB holds many hours.
#define DIAG_RING_CAP (5 * 1024 * 1024)

static char        *s_ring   = NULL;
static size_t       s_cap    = 0;
static size_t       s_head   = 0;   // next write index (mod s_cap); == s_total % s_cap
static size_t       s_count  = 0;   // bytes stored (<= s_cap)
static uint64_t     s_total  = 0;   // monotonic bytes ever appended (never wraps)
static portMUX_TYPE s_mux    = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_enabled = false;
static vprintf_like_t s_orig_vprintf = NULL;

// Append into the ring under a short spinlock. Lines come from a 256-byte
// stack buffer in the hook, so len is small and the critical section stays
// tiny (two memcpy spans at most).
static void ring_append(const char *buf, size_t len)
{
    if (!s_ring || len == 0) return;
    if (len >= s_cap) {            // pathological: keep only the tail
        buf += (len - s_cap);
        len  = s_cap;
    }
    portENTER_CRITICAL(&s_mux);
    size_t first = s_cap - s_head;
    if (first > len) first = len;
    memcpy(s_ring + s_head, buf, first);
    if (len > first) memcpy(s_ring, buf + first, len - first);
    s_head = (s_head + len) % s_cap;
    s_count += len;
    if (s_count > s_cap) s_count = s_cap;
    s_total += len;
    portEXIT_CRITICAL(&s_mux);
}

// esp_log hook: capture into the ring (when enabled), then always forward to
// the original vprintf so serial-console output is unchanged. Never logs
// itself (no recursion). Must not take a blocking lock — this can run before
// the scheduler starts and from arbitrary task contexts.
// USB enumeration-failure tally. The stale-QMX wedge (TODO #74) ends in the
// driver's "ENUM: [0:0] CHECK_SHORT_DEV_DESC FAILED" with the device object
// freed and no retry - leaving nothing distinguishable from an empty port in
// usb_host_lib_info(), so the log line itself is the only reliable signal.
// This hook already sees every log line; a cheap substring probe here is
// what usb_replug.c's stale-QMX detector reads.
static volatile uint32_t s_usb_enum_failures = 0;

uint32_t diag_log_usb_enum_failures(void)
{
    return s_usb_enum_failures;
}

static int diag_vprintf(const char *fmt, va_list ap)
{
    if (s_enabled && s_ring) {
        va_list ap2;
        va_copy(ap2, ap);
        char line[256];
        int n = vsnprintf(line, sizeof(line), fmt, ap2);
        va_end(ap2);
        if (n > 0) {
            size_t ln = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;
            if (strstr(line, "CHECK_SHORT_DEV_DESC") ||
                strstr(line, "CHECK_FULL_DEV_DESC"))
                s_usb_enum_failures++;
            ring_append(line, ln);
        }
    }
    return s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);
}

void diag_log_init(void)
{
    if (s_ring) return;
    s_ring = heap_caps_malloc(DIAG_RING_CAP, MALLOC_CAP_SPIRAM);
    if (!s_ring) s_ring = heap_caps_malloc(DIAG_RING_CAP, MALLOC_CAP_8BIT);
    if (s_ring) {
        s_cap = DIAG_RING_CAP;
        s_head = 0;
        s_count = 0;
        s_total = 0;
    } else {
        ESP_LOGW(TAG, "ring buffer alloc failed; capture disabled");
    }
    s_orig_vprintf = esp_log_set_vprintf(diag_vprintf);
    // Always-on: capture from the very first boot log. The session header
    // (diag_log_write_session_header) is written slightly later from app_main,
    // once settings are up and the version/operator fields are valid.
    s_enabled = (s_ring != NULL);
}

bool diag_log_enabled(void)
{
    return s_enabled;
}

size_t diag_log_size(void)
{
    portENTER_CRITICAL(&s_mux);
    size_t n = s_count;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

size_t diag_log_snapshot(char *dst, size_t cap)
{
    if (!s_ring || !dst || cap == 0) return 0;

    // Grab the head/count under the lock, then copy without it: a 128 KB
    // memcpy is far too long to hold a spinlock (interrupts off). Worst case,
    // heavy concurrent logging overwrites a few of the oldest bytes mid-copy
    // — acceptable garble for the earliest lines of a diagnostic dump.
    portENTER_CRITICAL(&s_mux);
    size_t head  = s_head;
    size_t count = s_count;
    portEXIT_CRITICAL(&s_mux);

    size_t n = (count > cap) ? cap : count;
    size_t start = (head + s_cap - count) % s_cap;
    size_t first = s_cap - start;
    if (first > n) first = n;
    memcpy(dst, s_ring + start, first);
    if (n > first) memcpy(dst + first, s_ring, n - first);
    return n;
}

void diag_log_clear(void)
{
    portENTER_CRITICAL(&s_mux);
    s_head  = 0;
    s_count = 0;
    // s_total is intentionally NOT reset — the SD mirror's read cursor lives
    // in s_total space and must stay monotonic across a web-download clear.
    portEXIT_CRITICAL(&s_mux);
}

uint64_t diag_log_total(void)
{
    portENTER_CRITICAL(&s_mux);
    uint64_t t = s_total;
    portEXIT_CRITICAL(&s_mux);
    return t;
}

size_t diag_log_read_from(uint64_t from, char *dst, size_t cap, uint64_t *out_next)
{
    if (!s_ring || !dst || cap == 0) {
        if (out_next) *out_next = from;
        return 0;
    }

    // Snapshot the head/total under the lock, copy without it (the ring is up
    // to 5 MB; a memcpy that large must never run with interrupts off). At our
    // ~0 lines/s steady state the [from, total) span cannot be overwritten
    // mid-copy, so this is safe in practice.
    portENTER_CRITICAL(&s_mux);
    uint64_t total = s_total;
    size_t   count = s_count;
    portEXIT_CRITICAL(&s_mux);

    uint64_t oldest = total - (uint64_t)count;   // earliest total still retained
    if (from < oldest) from = oldest;            // caller fell behind; skip lost bytes
    if (from >= total) {
        if (out_next) *out_next = total;
        return 0;
    }

    uint64_t avail = total - from;
    size_t   n     = (avail > (uint64_t)cap) ? cap : (size_t)avail;
    // head == total % s_cap, so the byte at total-position P lives at P % s_cap.
    size_t start = (size_t)(from % (uint64_t)s_cap);
    size_t first = s_cap - start;
    if (first > n) first = n;
    memcpy(dst, s_ring + start, first);
    if (n > first) memcpy(dst + first, s_ring, n - first);
    if (out_next) *out_next = from + n;
    return n;
}

// ---- Flash persistence -----------------------------------------------------
// The PSRAM ring is wiped on power-off, so a field/POTA session's log would be
// lost the moment the battery is disconnected unless a microSD card is in. To
// guarantee the log survives power-off with NO card, a background task also
// appends the ring to a small rolling file on the internal SPIFFS flash.
//
// Kept deliberately small (256 KB, one rotation) since SPIFFS (1 MB) is shared
// with the ADIF QSO log, and flushed only every 30 s and only when there are
// new bytes, so flash wear is proportional to actual log volume (≈0 at the
// deduplicated steady state). Served to the web UI via /api/log/saved.
#define DIAG_FLASH_PATH     "/spiffs/diag.log"
#define DIAG_FLASH_PATH_0   "/spiffs/diag.0.log"
#define DIAG_FLASH_MAX      (256 * 1024)
#define DIAG_FLASH_FLUSH_MS 30000

static uint64_t s_flash_cursor = 0;   // position in s_total space

static void diag_persist_task(void *arg)
{
    (void)arg;
    FILE *f = fopen(DIAG_FLASH_PATH, "a");
    if (!f) {
        ESP_LOGW(TAG, "flash-persist: cannot open %s (%s)", DIAG_FLASH_PATH, strerror(errno));
        vTaskDelete(NULL);
        return;
    }
    long pos = ftell(f);
    size_t bytes = (pos > 0) ? (size_t)pos : 0;
    static char buf[2048];

    for (;;) {
        bool wrote = false;
        for (;;) {
            uint64_t next = s_flash_cursor;
            size_t got = diag_log_read_from(s_flash_cursor, buf, sizeof(buf), &next);
            if (got == 0) break;
            if (fwrite(buf, 1, got, f) != got) break;   // best-effort; retry next tick
            s_flash_cursor = next;
            bytes += got;
            wrote = true;
            if (bytes >= DIAG_FLASH_MAX) {              // rotate: keep one generation
                fclose(f);
                remove(DIAG_FLASH_PATH_0);
                rename(DIAG_FLASH_PATH, DIAG_FLASH_PATH_0);
                f = fopen(DIAG_FLASH_PATH, "w");
                bytes = 0;
                if (!f) {
                    ESP_LOGW(TAG, "flash-persist: reopen after rotate failed (%s)", strerror(errno));
                    vTaskDelete(NULL);
                    return;
                }
            }
        }
        if (wrote) { fflush(f); fsync(fileno(f)); }     // commit to flash
        vTaskDelay(pdMS_TO_TICKS(DIAG_FLASH_FLUSH_MS));
    }
}

void diag_log_persist_start(void)
{
    psram_task_create(diag_persist_task, "diag_persist", 4096, NULL, 2, tskNO_AFFINITY);
}

const char *diag_log_persist_path(void)
{
    return DIAG_FLASH_PATH;
}
