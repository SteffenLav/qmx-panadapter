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

static void ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
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
