#include "ft8_status.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define STATUS_MAX 96

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
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_buf, tmp, STATUS_MAX);
    if (s_lock) xSemaphoreGive(s_lock);
}

void ft8_status_get(char *buf, size_t len)
{
    if (!buf || !len) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(buf, s_buf, len - 1);
    buf[len - 1] = '\0';
    if (s_lock) xSemaphoreGive(s_lock);
}
