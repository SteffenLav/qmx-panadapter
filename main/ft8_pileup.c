#include "ft8_pileup.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "ft8_pileup";

static SemaphoreHandle_t   s_lock;
static ft8_pileup_entry_t  s_entries[FT8_PILEUP_MAX];
static int                 s_count = 0;

static inline void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

void ft8_pileup_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_count = 0;
}

void ft8_pileup_clear(void)
{
    if (!s_lock) return;
    lock();
    s_count = 0;
    unlock();
}

void ft8_pileup_note_caller(const char *call, int16_t snr_db, int16_t freq_hz,
                            int64_t last_seen_utc)
{
    if (!call || !call[0] || !s_lock) return;

    lock();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].call, call) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (s_count < FT8_PILEUP_MAX) {
            idx = s_count++;
        } else {
            // Evict the oldest (lowest last_seen_utc) to make room.
            int oldest = 0;
            for (int i = 1; i < s_count; i++) {
                if (s_entries[i].last_seen_utc < s_entries[oldest].last_seen_utc) oldest = i;
            }
            idx = oldest;
            ESP_LOGI(TAG, "list full - evicting %s for %s", s_entries[oldest].call, call);
        }
    }
    strncpy(s_entries[idx].call, call, sizeof(s_entries[idx].call) - 1);
    s_entries[idx].call[sizeof(s_entries[idx].call) - 1] = '\0';
    s_entries[idx].snr_db        = snr_db;
    s_entries[idx].freq_hz       = freq_hz;
    s_entries[idx].last_seen_utc = last_seen_utc;
    unlock();
}

void ft8_pileup_remove(const char *call)
{
    if (!call || !call[0] || !s_lock) return;

    lock();
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].call, call) == 0) {
            // Compact: shift everything after i down by one.
            for (int j = i; j < s_count - 1; j++) s_entries[j] = s_entries[j + 1];
            s_count--;
            break;
        }
    }
    unlock();
}

int ft8_pileup_get_all(ft8_pileup_entry_t *out, int max)
{
    if (!out || max <= 0 || !s_lock) return 0;

    lock();
    int n = (s_count < max) ? s_count : max;
    memcpy(out, s_entries, (size_t)n * sizeof(ft8_pileup_entry_t));
    unlock();

    // Newest-first (simple insertion sort - n is at most FT8_PILEUP_MAX=12).
    for (int i = 1; i < n; i++) {
        ft8_pileup_entry_t tmp = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].last_seen_utc < tmp.last_seen_utc) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = tmp;
    }
    return n;
}

int ft8_pileup_count(void)
{
    if (!s_lock) return 0;
    lock();
    int n = s_count;
    unlock();
    return n;
}
