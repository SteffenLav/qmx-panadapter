#ifndef SHIM_FREERTOS_H
#define SHIM_FREERTOS_H
#include <stdint.h>
#define portMAX_DELAY 0xFFFFFFFFu
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE  1
#define pdFALSE 0
#endif
