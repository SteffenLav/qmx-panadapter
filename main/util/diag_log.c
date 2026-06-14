#include "diag_log.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
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
static void write_header(void)
{
    ESP_LOGI(TAG, "==== diagnostic logging ENABLED ====");

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

// 512 KB of rolling history, PSRAM-backed (the Tab5 has ~30 MB free, so this
// is cheap). With the CAT poll logging de-duplicated (see cat.c — identical
// FA/MD/FW poll responses are dropped, only changes + a heartbeat remain),
// this holds a long session rather than the ~70 s the old 128 KB ring lasted
// when every 50 ms poll was logged verbatim.
#define DIAG_RING_CAP (512 * 1024)

static char        *s_ring   = NULL;
static size_t       s_cap    = 0;
static size_t       s_head   = 0;   // next write index (mod s_cap)
static size_t       s_count  = 0;   // bytes stored (<= s_cap)
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
    portEXIT_CRITICAL(&s_mux);
}

// esp_log hook: capture into the ring (when enabled), then always forward to
// the original vprintf so serial-console output is unchanged. Never logs
// itself (no recursion). Must not take a blocking lock — this can run before
// the scheduler starts and from arbitrary task contexts.
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
    } else {
        ESP_LOGW(TAG, "ring buffer alloc failed; capture disabled");
    }
    s_orig_vprintf = esp_log_set_vprintf(diag_vprintf);
}

void diag_log_set_enabled(bool on)
{
    s_enabled = on;
    if (on) {
        // Best-effort verbosity bump for the subsystems most useful to a
        // remote investigation. Only takes effect on builds where the level
        // is compiled in; our own gated CAT logging is INFO so it's captured
        // regardless.
        esp_log_level_set("cat",   ESP_LOG_DEBUG);
        esp_log_level_set("audio", ESP_LOG_DEBUG);
        esp_log_level_set("wifi",  ESP_LOG_DEBUG);

        write_header();
    } else {
        ESP_LOGI(TAG, "==== diagnostic logging DISABLED ====");
        esp_log_level_set("cat",   ESP_LOG_INFO);
        esp_log_level_set("audio", ESP_LOG_INFO);
        esp_log_level_set("wifi",  ESP_LOG_INFO);
    }
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
