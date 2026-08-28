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

// ⛔ A task created above MUST end with psram_task_park(), never vTaskDelete().
// Its stack and TCB belong to US, not to FreeRTOS - xTaskCreateStatic* means
// nothing frees them on delete - so vTaskDelete() leaks the whole stack, every
// time, silently. Measured: 64 KB per Panadapter/WSPR round trip from the two
// wspr_rx tasks alone (#269).
//
// psram_task_park() marks the caller finished and stops it for good; it never
// returns. An owner then calls psram_task_reap() from an ordinary task context
// to delete the parked task and free its buffers, and gets back how many it
// freed. Reaping at the next START is the easy place: whatever ended last time
// is provably parked by then.
//
// A task that runs for the life of the boot needs neither call, and must be
// created with the plain psram_task_create() above - permanent tasks taking
// reap slots is what broke the first version of this: they filled the table on
// the way up, the tasks that actually park found no slot, and the leak
// continued while the code looked fixed.
//
// So a task that ENDS must be created with psram_task_create_reapable() and
// must end with psram_task_park(). The two go together; either one alone
// leaks, loudly in the log now rather than silently.
TaskHandle_t psram_task_create_reapable(TaskFunction_t func, const char *name,
                                        uint32_t stack_bytes, void *arg,
                                        UBaseType_t priority, BaseType_t core_id);
void psram_task_park(void);
int  psram_task_reap(void);

#ifdef __cplusplus
}
#endif
