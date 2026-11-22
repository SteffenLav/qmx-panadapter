#include "ft8_status.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#define STATUS_MAX 96

static const char *TAG = "ft8_status";

static char              s_buf[STATUS_MAX];
static SemaphoreHandle_t s_lock;

void ft8_status_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_buf[0] = '\0';
}

/* ⛔ BOTH ACCESSORS MUST TOLERATE s_lock BEING NULL.
 *
 * xSemaphoreTake(NULL) is not a no-op - it is `assert failed:
 * xQueueSemaphoreTake queue.c:1709 ((pxQueue))`, an immediate abort and reboot.
 * ft8_status_init() runs from app_main (main.c:357), but these are reachable
 * from the UI before that: serial-captured 2026-08-16, a web-requested switch to
 * the FT8 screen while the "Waiting for QMX..." prompt was up crashed the device
 * here, with that very string half-formatted on the stack.
 *
 * Pre-init there is no concurrency to protect against - nothing else is running
 * yet - so the copy is simply done unlocked rather than dropped, which keeps the
 * status text the caller set. */
void ft8_status_set(const char *fmt, ...)
{
    char tmp[STATUS_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    /* ⭐ EVERY CHANGE IS LOGGED, with the task that made it.
     *
     * This string is what the Tab5 label and /api/status both show, and until
     * 2026-11-22 nothing recorded it - so when the operator reported the line
     * stuck on "RX: capturing..." for several cycles, there was no way to tell
     * a MISSING WRITE from a STUCK READER. Both were consistent with the
     * evidence: the decoder's own slot lines showed it finishing every slot
     * while /api/status returned "capturing..." for up to 45.8 s.
     *
     * One line per change, not per call, so the repeated per-slot writes of
     * the same text cost nothing. At three changes a slot this is one line
     * per ~5 s - cheap against a fault that otherwise leaves no trace at all.
     *
     * The log call is OUTSIDE the mutex deliberately: ESP_LOGI can block on
     * the UART, and holding this lock through it would stall every reader,
     * including the UI timer and the web handler. */
    bool changed;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    changed = (strncmp(s_buf, tmp, STATUS_MAX) != 0);
    memcpy(s_buf, tmp, STATUS_MAX);
    if (s_lock) xSemaphoreGive(s_lock);
    if (changed) {
        const char *who = pcTaskGetName(NULL);
        ESP_LOGI(TAG, "status -> '%s' (set by %s)", tmp, who ? who : "?");
    }
}

void ft8_status_get(char *buf, size_t len)
{
    if (!buf || !len) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(buf, s_buf, len - 1);
    buf[len - 1] = '\0';
    if (s_lock) xSemaphoreGive(s_lock);
}
