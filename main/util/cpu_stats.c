#include "cpu_stats.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "psram_task.h"

static const char *TAG = "cpu";

// v2 (2026-07-13): idle-only O(1) sampler. v1 used uxTaskGetSystemState()
// for a full per-task table — but that byte-walks EVERY task's stack for the
// watermark inside a kernel-lock critical section (several stacks are 64 KB
// and in PSRAM), a multi-ms interrupts-off window every 10 s. That window
// delayed the core-0 MIPI-DSI frame-restart ISR and blanked the panel for a
// frame (the FT4 "full-screen cyan flash") — and core-pinning does NOT
// contain it (hardware-verified): a core-0 task touching the same lock spins
// with its own interrupts off, propagating the stall. So: no walks, ever.
// Per-core idle% (read from the idle tasks' run-time counters, O(1)) is the
// headline number anyway; the full per-task breakdown remains available
// on-demand via the dev-only resmon (POST /api/cmd {resmon}), where a rare
// one-frame blink is an accepted cost of invoking it.
#define CPU_STATS_PERIOD_MS  10000

static void cpu_stats_task(void *arg)
{
    (void)arg;
    uint32_t prev_idle0 = 0, prev_idle1 = 0;
    int64_t  prev_us    = 0;
    bool     prev_valid = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CPU_STATS_PERIOD_MS));
        // A single TCB field read per core (short critical section, no
        // walks). Run-time counters are esp_timer µs.
        uint32_t i0 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0);
        uint32_t i1 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1);
        int64_t  now = esp_timer_get_time();

        if (prev_valid) {
            uint32_t dt = (uint32_t)(now - prev_us);
            if (dt > 0) {
                // Tenths of a percent; idle counter can't exceed wall time.
                uint32_t p0 = (uint32_t)(((uint64_t)(i0 - prev_idle0) * 1000) / dt);
                uint32_t p1 = (uint32_t)(((uint64_t)(i1 - prev_idle1) * 1000) / dt);
                if (p0 > 1000) p0 = 1000;
                if (p1 > 1000) p1 = 1000;
                ESP_LOGI(TAG, "idle0 %lu.%lu%% idle1 %lu.%lu%%",
                         (unsigned long)(p0 / 10), (unsigned long)(p0 % 10),
                         (unsigned long)(p1 / 10), (unsigned long)(p1 % 10));
            }
        }
        prev_idle0 = i0;
        prev_idle1 = i1;
        prev_us    = now;
        prev_valid = true;
    }
}

esp_err_t cpu_stats_init(void)
{
    TaskHandle_t h = psram_task_create(cpu_stats_task, "cpu_stats", 3072,
                                       NULL, 1, tskNO_AFFINITY);
    if (!h) {
        ESP_LOGE(TAG, "task create failed; CPU stats disabled");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "per-core idle%% every %d s (in diag log)",
             CPU_STATS_PERIOD_MS / 1000);
    return ESP_OK;
}
