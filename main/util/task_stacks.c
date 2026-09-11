#include "task_stacks.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "stacks";

// Below this much unused stack, a task is one deep call from smashing whatever
// sits beneath it. Not a FreeRTOS limit - a judgement, and deliberately
// generous: on this board a "small" local is not small (CLAUDE.md), and the
// failures this exists to catch corrupt memory silently rather than tripping
// the canary.
#define STACK_WARN_BYTES 512

void task_stacks_report(void)
{
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;
    // PSRAM, not the stack and not the internal heap: this runs while the
    // internal heap is the thing under investigation, and a diagnostic that
    // needs the scarce resource cannot report on it.
    TaskStatus_t *t = heap_caps_calloc(cap, sizeof(TaskStatus_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t) {
        ESP_LOGE(TAG, "stacks: out of PSRAM for the snapshot");
        return;
    }

    UBaseType_t n = uxTaskGetSystemState(t, cap, NULL);

    ESP_LOGW(TAG, "=== task stack headroom (unused bytes, LOWEST EVER for each "
                  "task - only covers paths that actually ran) ===");
    ESP_LOGW(TAG, "%-18s %5s %4s %9s", "task", "core", "pri", "free");

    // Selection sort, tightest first - the answer is always at the top.
    for (UBaseType_t i = 0; i < n; i++) {
        UBaseType_t best = i;
        for (UBaseType_t k = i + 1; k < n; k++)
            if (t[k].usStackHighWaterMark < t[best].usStackHighWaterMark) best = k;
        if (best != i) { TaskStatus_t tmp = t[i]; t[i] = t[best]; t[best] = tmp; }

        unsigned freeb = (unsigned)t[i].usStackHighWaterMark * sizeof(StackType_t);
        BaseType_t core = t[i].xCoreID;
        ESP_LOGW(TAG, "%-18s %5s %4u %7u B%s",
                 t[i].pcTaskName,
                 (core == tskNO_AFFINITY) ? "any" : (core == 0 ? "0" : "1"),
                 (unsigned)t[i].uxCurrentPriority,
                 freeb,
                 (freeb < STACK_WARN_BYTES) ? "   <-- LOW" : "");
    }

    heap_caps_free(t);
}
