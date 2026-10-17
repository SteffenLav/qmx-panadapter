/* Host shim - see test/shim/README.md. Enough of esp_log.h to compile and run
   a real firmware .c on the PC. The log text is kept, because a test whose
   subject is "it must say so instead of failing in silence" has to be able to
   SEE the line. */
#ifndef SHIM_ESP_LOG_H
#define SHIM_ESP_LOG_H
#include <stdio.h>
extern int shim_log_errors;     /* ESP_LOGE count, for assertions */
extern int shim_log_quiet;      /* 1 = count but do not print */
#define SHIM_LOG(lvl, tag, fmt, ...) do { \
    if (!shim_log_quiet) printf("[%s] %s: " fmt "\n", lvl, tag, ##__VA_ARGS__); \
} while (0)
#define ESP_LOGE(tag, fmt, ...) do { shim_log_errors++; SHIM_LOG("E", tag, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) SHIM_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) SHIM_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#endif
