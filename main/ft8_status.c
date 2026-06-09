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

void ft8_status_set(const char *fmt, ...)
{
    char tmp[STATUS_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_buf, tmp, STATUS_MAX);
    xSemaphoreGive(s_lock);
}

void ft8_status_get(char *buf, size_t len)
{
    if (!buf || !len) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(buf, s_buf, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(s_lock);
}
