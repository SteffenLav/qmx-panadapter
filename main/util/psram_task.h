#pragma once

// ⛔ NEVER for a task that writes FLASH (OTA, SPIFFS, NVS commit).
// Writing flash disables the cache, and a task whose stack is in PSRAM cannot
// run with the cache off - IDF asserts in
// spi_flash_disable_interrupts_caches_and_other_cpu (cache_utils.c:152).
// Confirmed on hardware 2026-08-20: the #218 OTA task with a PSRAM stack
// panicked at 96% of the download. Such tasks need an ordinary internal stack.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// Spawn a task whose stack lives in PSRAM instead of internal RAM. Only the
// small StaticTask_t TCB (~100 bytes) stays internal, which FreeRTOS
// requires. Falls back to a normal internal-stack task if either PSRAM
// allocation fails (logs a warning either way).
//
// ONLY use this for background/non-realtime tasks: low-frequency polling,
// debounced flushes, infrequent housekeeping. Do NOT use it for tasks that
// touch USB/DMA buffers on their own stack or that are latency-critical
// (audio_task, fft_task, render_task, cat's link/poll tasks, ws_push_task) —
// PSRAM access is slower than internal SRAM and those tasks' margins are
// already tight.
//
// core_id: a specific core number, or tskNO_AFFINITY for no pinning (this is
// what plain xTaskCreate() uses internally, so passing it here is the direct
// equivalent of xTaskCreate with a PSRAM stack).
TaskHandle_t psram_task_create(TaskFunction_t func, const char *name,
                                uint32_t stack_bytes, void *arg,
                                UBaseType_t priority, BaseType_t core_id);

#ifdef __cplusplus
}
#endif
