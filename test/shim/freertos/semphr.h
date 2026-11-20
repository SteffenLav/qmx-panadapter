/* Host shim: the harness is single-threaded, so the mutex only has to exist
   and count. shim_lock_depth catches a take/give imbalance, which is a real
   defect class in this file. */
#ifndef SHIM_SEMPHR_H
#define SHIM_SEMPHR_H
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
extern int shim_lock_depth;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t h);
/* Recursive mutex, for main/util/spiffs_guard.c. Its OWN depth counter, so a
 * guard take does not read as an imbalance of adif_log.c's mutex above. */
extern int shim_rlock_depth;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t h, TickType_t t);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t h);
#endif
