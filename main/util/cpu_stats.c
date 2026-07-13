#include "cpu_stats.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

#include "psram_task.h"

static const char *TAG = "cpu";

// 10 s window: fine-grained enough to line up with individual FT8 slots in
// the diag log (slot lines carry their own timestamps), coarse enough that
// two log lines per window are noise in the 5 MB ring.
#define CPU_STATS_PERIOD_MS  10000

// Generous ceiling; the firmware runs ~25-30 tasks. If it's ever exceeded the
// snapshot silently covers the first MAX_TASKS only (uxTaskGetSystemState
// returns 0 on overflow, which we treat as "skip this window").
#define CPU_STATS_MAX_TASKS  48

// Don't clutter the line with tasks that consumed less than this.
#define CPU_STATS_MIN_PCT    5   // tenths of a percent (0.5%)

typedef struct {
    TaskHandle_t handle;
    uint32_t     counter;
} prev_entry_t;

static TaskStatus_t  *s_snap = NULL;      // PSRAM, one-shot alloc
static prev_entry_t  *s_prev = NULL;      // PSRAM, one-shot alloc
static int            s_prev_n = 0;
static configRUN_TIME_COUNTER_TYPE s_prev_total = 0;
static bool           s_prev_valid = false;

// Find last window's counter for a task; 0 delta for tasks born this window.
static bool prev_lookup(TaskHandle_t h, uint32_t *out)
{
    for (int i = 0; i < s_prev_n; i++) {
        if (s_prev[i].handle == h) {
            *out = s_prev[i].counter;
            return true;
        }
    }
    return false;
}

// Append " name pct.d" to line (tenths-of-percent fixed point, e.g. "lvgl 38.2").
static void append_entry(char *line, size_t cap, const char *name, uint32_t pct10)
{
    size_t len = strlen(line);
    if (len >= cap - 1) return;
    snprintf(line + len, cap - len, " %s %lu.%lu",
             name, (unsigned long)(pct10 / 10), (unsigned long)(pct10 % 10));
}

static void cpu_stats_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CPU_STATS_PERIOD_MS));

        configRUN_TIME_COUNTER_TYPE total = 0;
        UBaseType_t n = uxTaskGetSystemState(s_snap, CPU_STATS_MAX_TASKS, &total);
        if (n == 0) {
            // More live tasks than CPU_STATS_MAX_TASKS; resample next window.
            s_prev_valid = false;
            continue;
        }

        // Total run time is a single esp_timer-based wall clock (µs, 32-bit
        // by default, wraps ~71 min). Truncating unsigned subtraction handles
        // the wrap regardless of counter width.
        uint32_t dt = (uint32_t)(total - s_prev_total);

        if (s_prev_valid && dt > 0) {
            // One line per core; unpinned tasks (tskNO_AFFINITY) go to "any".
            // Each entry: task delta as tenths of a percent of wall time, so
            // a core's entries + its IDLE task sum to ~100.
            char line0[384] = {0};
            char line1[384] = {0};
            char lineA[192] = {0};

            // Selection sort by delta, descending — n is ~25-30, cost is nil,
            // and busiest-first makes the log line readable at a glance.
            uint32_t delta[CPU_STATS_MAX_TASKS];
            int      order[CPU_STATS_MAX_TASKS];
            for (UBaseType_t i = 0; i < n; i++) {
                uint32_t prevc = 0;
                prev_lookup(s_snap[i].xHandle, &prevc);
                delta[i] = s_snap[i].ulRunTimeCounter - prevc;
                order[i] = i;
            }
            for (UBaseType_t i = 0; i + 1 < n; i++) {
                for (UBaseType_t j = i + 1; j < n; j++) {
                    if (delta[order[j]] > delta[order[i]]) {
                        int t = order[i]; order[i] = order[j]; order[j] = t;
                    }
                }
            }

            for (UBaseType_t k = 0; k < n; k++) {
                const TaskStatus_t *ts = &s_snap[order[k]];
                uint32_t d = delta[order[k]];
                // 64-bit intermediate: d and dt are µs, up to ~10^7 each.
                uint32_t pct10 = (uint32_t)(((uint64_t)d * 1000) / dt);
                if (pct10 < CPU_STATS_MIN_PCT) continue;
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
                BaseType_t core = ts->xCoreID;
#else
                BaseType_t core = tskNO_AFFINITY;
#endif
                if (core == 0)      append_entry(line0, sizeof(line0), ts->pcTaskName, pct10);
                else if (core == 1) append_entry(line1, sizeof(line1), ts->pcTaskName, pct10);
                else                append_entry(lineA, sizeof(lineA), ts->pcTaskName, pct10);
            }

            ESP_LOGI(TAG, "core0:%s%s%s", line0,
                     lineA[0] ? " | any:" : "", lineA);
            ESP_LOGI(TAG, "core1:%s", line1);
        }

        // Current snapshot becomes the baseline for the next window.
        for (UBaseType_t i = 0; i < n; i++) {
            s_prev[i].handle  = s_snap[i].xHandle;
            s_prev[i].counter = s_snap[i].ulRunTimeCounter;
        }
        s_prev_n     = (int)n;
        s_prev_total = total;
        s_prev_valid = true;
    }
}

esp_err_t cpu_stats_init(void)
{
    s_snap = heap_caps_malloc(sizeof(TaskStatus_t) * CPU_STATS_MAX_TASKS,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_prev = heap_caps_malloc(sizeof(prev_entry_t) * CPU_STATS_MAX_TASKS,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_snap || !s_prev) {
        ESP_LOGE(TAG, "alloc failed; CPU stats disabled");
        free(s_snap); s_snap = NULL;
        free(s_prev); s_prev = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Background housekeeping — PSRAM stack per project convention, lowest
    // priority. PINNED TO CORE 1, not unpinned (2026-07-13, FT4 cyan-flash
    // fix): uxTaskGetSystemState() holds the kernel lock (interrupts off on
    // the calling core) while it byte-walks every task's stack for the
    // watermark — several stacks are 64 KB and in PSRAM, so this is a
    // multi-ms ints-off window every 10 s. When it landed on core 0 it
    // delayed the MIPI-DSI frame-restart ISR (core 0) past the blanking
    // window and the panel blanked for one frame (full-screen cyan flash,
    // seen in FT4 where PSRAM contention makes the walk slowest). On core 1
    // the window can't touch the display ISR; fft/decode there have seconds
    // of slack against a rare ~ms stall.
    TaskHandle_t h = psram_task_create(cpu_stats_task, "cpu_stats", 4096,
                                       NULL, 1, 1);
    if (!h) {
        ESP_LOGE(TAG, "task create failed; CPU stats disabled");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "per-task CPU stats every %d s (in diag log)",
             CPU_STATS_PERIOD_MS / 1000);
    return ESP_OK;
}
