#pragma once
#include <stddef.h>

// Shared one-line status string — written by ft8_task, read by the LVGL UI
// 1 Hz timer. Thread-safe via a mutex.

void ft8_status_init(void);
void ft8_status_set(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ft8_status_get(char *buf, size_t len);
