#include "psram_task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "psram_task";

/* ---------------------------------------------------------------------------
 * Ending a PSRAM-stack task (#279)
 *
 * The stack and TCB below are OURS - xTaskCreateStatic* means FreeRTOS never
 * owns or frees them. So a task created here that ends with vTaskDelete()
 * leaks its whole 32-64 KB stack, every time, permanently and silently.
 * Measured on the dev bench: wspr_rx creates two of these on every WSPR entry
 * and deletes them on every exit, and PSRAM fell a clean 64 KB - exactly their
 * two stacks - on each Panadapter/WSPR round trip.
 *
 * A task cannot free the stack it is standing on, so it PARKS and an owner
 * reaps it. The suspend-then-spin-then-delete order is IDF's own, copied from
 * prvTaskDeleteWithCaps, and every step is load-bearing on SMP: the target may
 * be running on the other core, and deleting a running task is the
 * use-after-free this exists to avoid. Deleting a task OTHER than yourself
 * completes synchronously - the idle task is only involved in self-deletion -
 * so the buffers are safe to free the moment vTaskDelete() returns.
 * ------------------------------------------------------------------------- */
/* Only tasks created REAPABLE take a slot. The 28 psram_task_create() call
 * sites are nearly all permanent tasks that never end, and when they were
 * registered too they filled the table on the way up - so the handful of tasks
 * that actually park found no slot, silently fell back to vTaskDelete(), and
 * went on leaking exactly as before. Opt-in is what makes the size knowable. */
#define PSRAM_TASK_MAX 8

typedef struct {
    TaskHandle_t   h;
    StackType_t   *stack;
    StaticTask_t  *tcb;
    volatile bool  finished;
} psram_task_rec_t;

static psram_task_rec_t s_recs[PSRAM_TASK_MAX];
static portMUX_TYPE     s_recs_lock = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t psram_task_create_common(TaskFunction_t func, const char *name,
                                             uint32_t stack_bytes, void *arg,
                                             UBaseType_t priority, BaseType_t core_id,
                                             bool reapable)
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
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(func, name, stack_bytes, arg,
                                                   priority, stack, tcb, core_id);
    if (!h) { heap_caps_free(stack); heap_caps_free(tcb); return NULL; }

    if (!reapable) return h;   /* permanent task: no slot, nothing to reap */

    int slot = -1;
    portENTER_CRITICAL(&s_recs_lock);
    for (int i = 0; i < PSRAM_TASK_MAX; i++) {
        if (!s_recs[i].h) { slot = i; break; }
    }
    if (slot >= 0) {
        s_recs[slot].h        = h;
        s_recs[slot].stack    = stack;
        s_recs[slot].tcb      = tcb;
        s_recs[slot].finished = false;
    }
    portEXIT_CRITICAL(&s_recs_lock);
    /* An ERROR, not a warning: this task will park and its stack will then be
     * unreachable for ever. Silence here is what made the first version of
     * this fix measure as no fix at all. */
    if (slot < 0) ESP_LOGE(TAG, "%s: no reap slot free - its stack WILL leak", name);
    return h;
}

TaskHandle_t psram_task_create(TaskFunction_t func, const char *name,
                               uint32_t stack_bytes, void *arg,
                               UBaseType_t priority, BaseType_t core_id)
{
    return psram_task_create_common(func, name, stack_bytes, arg, priority,
                                    core_id, false);
}

TaskHandle_t psram_task_create_reapable(TaskFunction_t func, const char *name,
                                        uint32_t stack_bytes, void *arg,
                                        UBaseType_t priority, BaseType_t core_id)
{
    return psram_task_create_common(func, name, stack_bytes, arg, priority,
                                    core_id, true);
}

void psram_task_park(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool registered = false;

    portENTER_CRITICAL(&s_recs_lock);
    for (int i = 0; i < PSRAM_TASK_MAX; i++) {
        if (s_recs[i].h == me) { s_recs[i].finished = true; registered = true; break; }
    }
    portEXIT_CRITICAL(&s_recs_lock);

    /* Either the internal-RAM fallback above (FreeRTOS owns that stack, so the
     * ordinary delete is right and is not a leak), or - the bug worth shouting
     * about - a task that parks but was created with the plain, non-reapable
     * constructor. Its PSRAM stack becomes unreachable here. */
    if (!registered) {
        ESP_LOGW(TAG, "park on a task not created reapable - if its stack was "
                      "PSRAM it is now leaked; use psram_task_create_reapable()");
        vTaskDelete(NULL);
    }

    vTaskSuspend(NULL);
    for (;;) vTaskDelay(portMAX_DELAY);   /* never return, even if resumed */
}

int psram_task_reap(void)
{
    int reaped = 0;
    for (;;) {
        TaskHandle_t  h = NULL;
        StackType_t  *stack = NULL;
        StaticTask_t *tcb = NULL;

        portENTER_CRITICAL(&s_recs_lock);
        for (int i = 0; i < PSRAM_TASK_MAX; i++) {
            if (s_recs[i].h && s_recs[i].finished) {
                h = s_recs[i].h; stack = s_recs[i].stack; tcb = s_recs[i].tcb;
                s_recs[i].h = NULL; s_recs[i].stack = NULL;
                s_recs[i].tcb = NULL; s_recs[i].finished = false;
                break;
            }
        }
        portEXIT_CRITICAL(&s_recs_lock);
        if (!h) break;

        vTaskSuspend(h);
        while (eTaskGetState(h) == eRunning) taskYIELD();
        vTaskDelete(h);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        reaped++;
    }
    return reaped;
}
