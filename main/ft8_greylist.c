// FT8 grey-list - see ft8_greylist.h. Small fixed table, mutex-protected
// (written from the decode task's timeout path, read from the LVGL render
// and the pickers). RAM-only by design: reboot = clean slate.

#include "ft8_greylist.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ft8_grey";

#define GREY_MAX        24   // distinct calls tracked (fail counts + listed)
#define GREY_FAIL_LIMIT 2    // pounce timeouts before a station is listed

typedef struct {
    char    call[12];
    uint8_t fails;
} grey_entry_t;

static grey_entry_t      s_tbl[GREY_MAX];
static int               s_count = 0;
static SemaphoreHandle_t s_lock;

// Created lazily because there is no greylist_init() to hang it off, which
// leaves two hazards this guards against (#154):
//
//  - The create can FAIL under memory pressure, and xSemaphoreTake(NULL, t) is
//    not a failed take, it is an immediate abort(). Every caller therefore
//    re-checks s_lock after calling this, exactly as the pickers already did.
//  - The pickers run on taskLVGL and the timeout notes come from the decode
//    task, so an unprotected first call could have BOTH create a mutex, with
//    the second overwriting the first: two tasks each holding a different
//    mutex, i.e. no mutual exclusion at all, plus a leak. The spinlock makes
//    the check-and-create atomic; it is taken once in the life of the table.
static portMUX_TYPE s_lock_mux = portMUX_INITIALIZER_UNLOCKED;

static void ensure_lock(void)
{
    if (s_lock) return;
    portENTER_CRITICAL(&s_lock_mux);
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    portEXIT_CRITICAL(&s_lock_mux);
}

static int find_locked(const char *call)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_tbl[i].call, call) == 0) return i;
    }
    return -1;
}

void ft8_greylist_note_timeout(const char *call)
{
    if (!call || !call[0]) return;
    ensure_lock();
    if (!s_lock) return;   // create failed - a missed grey-list note, not a reboot
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_locked(call);
    if (i < 0) {
        if (s_count < GREY_MAX) {
            i = s_count++;
        } else {
            i = 0;   // full: recycle the oldest slot (rare - 24 distinct fails)
            memmove(&s_tbl[0], &s_tbl[1], sizeof(s_tbl[0]) * (GREY_MAX - 1));
            i = GREY_MAX - 1;
        }
        memset(&s_tbl[i], 0, sizeof(s_tbl[i]));
        strncpy(s_tbl[i].call, call, sizeof(s_tbl[i].call) - 1);
    }
    if (s_tbl[i].fails < 255) s_tbl[i].fails++;
    uint8_t fails = s_tbl[i].fails;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s pounce timeout #%u%s", call, (unsigned)fails,
             fails >= GREY_FAIL_LIMIT ? " - grey-listed" : "");
}

bool ft8_greylist_contains(const char *call)
{
    if (!call || !call[0] || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_locked(call);
    bool listed = (i >= 0 && s_tbl[i].fails >= GREY_FAIL_LIMIT);
    xSemaphoreGive(s_lock);
    return listed;
}

void ft8_greylist_clear(const char *call)
{
    if (!call || !call[0] || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_locked(call);
    if (i >= 0) {
        memmove(&s_tbl[i], &s_tbl[i + 1], sizeof(s_tbl[0]) * (s_count - i - 1));
        s_count--;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s cleared from grey-list", call);
}

void ft8_greylist_clear_all(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "grey-list cleared");
}

// Snapshot the listed calls for the web UI's viewer - the toggle existed there
// with no way to see WHO was being skipped, which made "why is it ignoring that
// station?" undiagnosable from another room. Returns entries actually LISTED
// (i.e. being skipped), not ones merely carrying a first strike.
int ft8_greylist_get_all(char out[][12], int max)
{
    ensure_lock();
    if (!out || max <= 0 || !s_lock) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++) {
        if (s_tbl[i].fails >= GREY_FAIL_LIMIT) {
            snprintf(out[n], 12, "%.11s", s_tbl[i].call);
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}
