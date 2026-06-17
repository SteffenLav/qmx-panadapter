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

static bool epoch_is_sane(int64_t t) {
    return t > EPOCH_SANE_MIN && t < EPOCH_SANE_MAX;
}

// Return a date anchor (UTC epoch of some recent day) from the best available
// source: current system clock (if already sane) or NVS last-known timestamp.
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

static void apply_and_persist(time_t utc, const char *source)
{
    // Update system clock
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    // Write to RX8130CE supercap RTC
    struct tm tm_utc;
    gmtime_r(&utc, &tm_utc);
    if (!rtc_set_time(&tm_utc)) {
        ESP_LOGW(TAG, "%s: RTC write failed", source);
    }

    // Keep NVS anchor fresh for future fallback (only if date is actually known)
    if (epoch_is_sane((int64_t)utc)) {
        settings_set_last_unix_time((uint32_t)utc);
    }

    ESP_LOGI(TAG, "Time set from %s: %04d-%02d-%02d %02d:%02d:%02d UTC",
             source,
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
}

void time_sync_notify_sntp(time_t utc)
{
    // SNTP already set the system clock via settimeofday internally; we just
    // need to persist it to the RTC and the NVS anchor.
    apply_and_persist(utc, "SNTP");
}

void time_sync_notify_qmx(int h, int m, int s)
{
    // QMX gives time-of-day only. Reconstruct full UTC from the best date anchor.
    time_t anchor = get_date_anchor();
    int64_t day_start = ((int64_t)anchor / 86400) * 86400;
    time_t utc = (time_t)(day_start + h * 3600 + m * 60 + s);

    apply_and_persist(utc, "QMX");
}

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
    if (utc < EPOCH_SANE_MIN) {
        ESP_LOGW(TAG, "manual time rejected (year=%d looks wrong)", year);
        return;
    }
    apply_and_persist(utc, "manual");
}

// Periodic background task: syncs from QMX whenever CAT is ready.
// Waits up to 5 minutes for initial CAT connect, then re-syncs every 5 minutes.
// This covers Panadapter mode (ft8_task handles its own initial sync in FT8 mode).
static void time_sync_task(void *arg)
{
    // Allow ft8_task and CAT handshake a head start before we compete for TM;
    vTaskDelay(pdMS_TO_TICKS(15000));  // 15 s settle

    // Wait for CAT to be ready (up to 5 minutes)
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
        ESP_LOGW(TAG, "CAT not ready after %d s; QMX time sync deferred to periodic", MAX_WAIT_S);
    }

    // Periodic re-sync every 5 minutes (re-syncs when GPS locks on QMX)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(300000));  // 5 minutes
        if (!cat_is_ready()) continue;
        int h, m, s;
        if (cat_query_qmx_time(&h, &m, &s) == ESP_OK) {
            time_sync_notify_qmx(h, m, s);
        }
    }
}

void time_sync_init(i2c_master_bus_handle_t bus)
{
    // Bring up the RTC driver
    if (rtc_init(bus) != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed — supercap RTC not available");
    } else if (rtc_is_valid()) {
        // RTC has valid time; set the system clock immediately at boot
        if (!rtc_apply_to_system()) {
            ESP_LOGW(TAG, "RTC read failed despite valid flag");
        }
    } else {
        ESP_LOGI(TAG, "RTC not valid (supercap dead or first boot) — waiting for network/QMX sync");
    }

    xTaskCreate(time_sync_task, "time_sync", 3072, NULL, 4, NULL);
}
