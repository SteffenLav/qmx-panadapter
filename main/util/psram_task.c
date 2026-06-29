#include "psram_task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "psram_task";

TaskHandle_t psram_task_create(TaskFunction_t func, const char *name,
                                uint32_t stack_bytes, void *arg,
                                UBaseType_t priority, BaseType_t core_id)
{
    StackType_t *stack = heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *tcb = stack ? heap_caps_malloc(sizeof(StaticTask_t),
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                               : NULL;
    if (!stack || !tcb) {
        ESP_LOGW(TAG, "%s: PSRAM stack/TCB alloc failed, falling back to internal RAM", name);
        if (stack) heap_caps_free(stack);
        TaskHandle_t h = NULL;
        xTaskCreatePinnedToCore(func, name, stack_bytes, arg, priority, &h, core_id);
        return h;
    }
    return xTaskCreateStaticPinnedToCore(func, name, stack_bytes, arg, priority,
                                          stack, tcb, core_id);
}
