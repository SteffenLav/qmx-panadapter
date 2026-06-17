#include "time_sync.h"
#include "rtc.h"
#include "settings.h"
#include "cat.h"

#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time_sync";

// UTC epoch bounds for sanity checks
#define EPOCH_SANE_MIN  1700000000LL  // 2023-11-14
#define EPOCH_SANE_MAX  2208988800LL  // 2040-01-01 — anything beyond is garbage

// SNTP is considered "fresh" within this window. While fresh, QMX TM; time
// is not applied to the system clock — SNTP is authoritative when WiFi is up.
// Outside the window (no WiFi / POTA field use), QMX takes over as fallback.
#define SNTP_FRESH_MS  (10LL * 60 * 1000)

// Timestamps of the last accepted sync from each source; 0 = never.
static int64_t s_last_qmx_sync_ms  = 0;
static int64_t s_last_sntp_sync_ms = 0;

static bool epoch_is_sane(int64_t t)
{
    return t > EPOCH_SANE_MIN && t < EPOCH_SANE_MAX;
}

// Return a date anchor (UTC epoch of some recent day) from the best available
// source: current system clock (if sane) or NVS last-known timestamp.
static time_t get_date_anchor(void)
{
    time_t now = time(NULL);
    if (epoch_is_sane((int64_t)now)) return now;

    qmx_settings_t cfg;
    settings_load_all(&cfg);
    if (epoch_is_sane((int64_t)cfg.last_unix_time)) return (time_t)cfg.last_unix_time;

    ESP_LOGW(TAG, "No valid date anchor (NVS=0x%08lx) — using fallback 2023-11-14; time-of-day will be correct",
             (unsigned long)cfg.last_unix_time);
    return (time_t)EPOCH_SANE_MIN;
}

// Sync priorities (highest first):
//   1. SNTP                 — always wins when WiFi is up; authoritative internet time
//   2. Tab5 RTC             — applied at boot from rtc_apply_to_system before SNTP/QMX
//   3. QMX TM;              — offline fallback only: applied when SNTP hasn't synced
//                             within SNTP_FRESH_MS (covers no-WiFi / POTA use)
//   4. Manual               — always applied (rare POTA with no QMX GPS or SNTP)
//
// Non-GPS QMX: the QMX internal RTC drifts freely and has no GPS discipline.
// Trusting it above SNTP would break FT8 timing whenever the QMX clock is off.
// QMX TM; is therefore only used when SNTP has not synced recently.

static void write_to_rtc_and_nvs(time_t utc, const char *source)
{
    struct tm tm_utc;
    gmtime_r(&utc, &tm_utc);
    if (!rtc_set_time(&tm_utc)) {
        ESP_LOGW(TAG, "%s: RTC write failed", source);
    }
    if (epoch_is_sane((int64_t)utc)) {
        settings_set_last_unix_time((uint32_t)utc);
    }
}

static void apply_and_persist(time_t utc, const char *source)
{
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    write_to_rtc_and_nvs(utc, source);

    struct tm tm_utc;
    gmtime_r(&utc, &tm_utc);
    ESP_LOGI(TAG, "Time set from %s: %04d-%02d-%02d %02d:%02d:%02d UTC",
             source,
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
}

// Priority 1: SNTP — always authoritative. Sets system clock, RTC, and NVS.
void time_sync_notify_sntp(time_t utc)
{
    apply_and_persist(utc, "SNTP");
    s_last_sntp_sync_ms = esp_timer_get_time() / 1000;
}

// Priority 3: QMX TM; time-of-day — offline fallback only.
// Applied to the system clock only when SNTP has not synced within SNTP_FRESH_MS.
// Reconstructs full UTC from the best available date anchor (system clock, NVS, or
// compile-time floor). Always tracks s_last_qmx_sync_ms for diagnostic purposes.
void time_sync_notify_qmx(int h, int m, int s)
{
    time_t anchor = get_date_anchor();
    int64_t day_start = ((int64_t)anchor / 86400) * 86400;
    time_t utc = (time_t)(day_start + h * 3600 + m * 60 + s);

    int64_t now_ms     = esp_timer_get_time() / 1000;
    bool sntp_fresh    = s_last_sntp_sync_ms > 0 &&
                         (now_ms - s_last_sntp_sync_ms) < SNTP_FRESH_MS;

    s_last_qmx_sync_ms = now_ms;

    if (sntp_fresh) {
        struct tm tm_utc;
        gmtime_r(&utc, &tm_utc);
        ESP_LOGD(TAG, "QMX TM; %02d:%02d:%02d — SNTP fresh (%llds ago), skipping",
                 h, m, s, (long long)(now_ms - s_last_sntp_sync_ms) / 1000);
        return;
    }

    apply_and_persist(utc, "QMX");
}

// Priority 5 (last resort): manual entry from user (rare POTA offline use).
void time_sync_set_manual(int year, int mon, int mday, int h, int m, int s)
{
    struct tm tm_utc = {
        .tm_year  = year - 1900,
        .tm_mon   = mon - 1,
        .tm_mday  = mday,
        .tm_hour  = h,
        .tm_min   = m,
        .tm_sec   = s,
        .tm_isdst = 0,
    };
    // mktime() is safe: ESP-IDF runs with UTC as the default timezone
    time_t utc = mktime(&tm_utc);
    if (!epoch_is_sane((int64_t)utc)) {
        ESP_LOGW(TAG, "manual time rejected (year=%d looks wrong)", year);
        return;
    }
    apply_and_persist(utc, "manual");
}

// Background task: initial QMX sync at CAT connect, then every 5 minutes.
// Covers Panadapter mode; ft8_task handles its own initial sync in FT8 mode.
static void time_sync_task(void *arg)
{
    // 15 s head start for ft8_task and CAT handshake before we query TM;
    vTaskDelay(pdMS_TO_TICKS(15000));

    const int MAX_WAIT_S = 300;
    int waited = 15;
    while (!cat_is_ready() && waited < MAX_WAIT_S) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        waited += 5;
    }

    if (cat_is_ready()) {
        int h, m, s;
        if (cat_query_qmx_time(&h, &m, &s) == ESP_OK) {
            time_sync_notify_qmx(h, m, s);
        } else {
            ESP_LOGW(TAG, "Initial QMX TM; query failed");
        }
    } else {
        ESP_LOGW(TAG, "CAT not ready after %ds — QMX time sync deferred to periodic", MAX_WAIT_S);
    }

    // Re-sync every 5 minutes (catches GPS lock events on QMX)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(300000));
        if (!cat_is_ready()) continue;
        int h, m, s;
        if (cat_query_qmx_time(&h, &m, &s) == ESP_OK) {
            time_sync_notify_qmx(h, m, s);
        }
    }
}

// Priority 2: Tab5 RTC — applied immediately at boot before QMX/SNTP are available.
void time_sync_init(i2c_master_bus_handle_t bus)
{
    if (rtc_init(bus) != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed — supercap RTC not available");
    } else if (rtc_is_valid()) {
        if (!rtc_apply_to_system()) {
            ESP_LOGW(TAG, "RTC read failed despite valid flag");
        }
    } else {
        ESP_LOGI(TAG, "RTC not valid (supercap dead or first boot) — waiting for QMX/SNTP sync");
    }

    xTaskCreate(time_sync_task, "time_sync", 3072, NULL, 4, NULL);
}
