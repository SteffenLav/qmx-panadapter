#include "cpu_stats.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "display.h"
#include "ui.h"          // ui_bandplan_call_counts
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
                // fps alongside idle, because they answer different questions and
                // only one of them is what the operator actually perceives. Both
                // are O(1) counter reads - no heap or stack walk - so this stays
                // safe on a periodic path (see the cyan-flash rule in CLAUDE.md).
                unsigned f10  = display_fps_x10();
                unsigned kpx  = display_inval_kpx_per_s();
                /* Frame SPACING on the same line as the rate, because the two
                 * disagree and only the spacing matches what the operator sees:
                 * the Tab5 renders FASTER than the web waterfall (12.2 vs 10.0
                 * fps, measured 2026-10-05) and looks worse. A mean near the
                 * nominal with a large max is jitter; a mean that is simply
                 * long is a slow renderer. Same O(1) counter read as the rest. */
                display_frame_spacing_t fs;
                display_frame_spacing(&fs);
                ESP_LOGI(TAG, "idle0 %lu.%lu%% idle1 %lu.%lu%% fps %u.%u inval %ukpx/s "
                              "| frame ms min %u mean %u max %u late %u/%u "
                              "| draw ms mean %u max %u",
                         (unsigned long)(p0 / 10), (unsigned long)(p0 % 10),
                         (unsigned long)(p1 / 10), (unsigned long)(p1 % 10),
                         f10 / 10, f10 % 10, kpx,
                         (unsigned)(fs.min_us / 1000), (unsigned)(fs.mean_us / 1000),
                         (unsigned)(fs.max_us / 1000),
                         (unsigned)fs.late, (unsigned)fs.n,
                         (unsigned)(fs.draw_mean_us / 1000),
                         (unsigned)(fs.draw_max_us / 1000));

                /* Second line, same window: WHERE those pixels were asked for.
                 * The total on the line above swings 6-20 Mpx/s on an
                 * unchanged screen, and the two big canvases cannot account
                 * for the high end - so this names the panel. */
                display_inval_bands_t ib;
                display_inval_bands(&ib);
                ESP_LOGI(TAG, "inval kpx/s: top %u spec %u lbl %u wf %u bp %u bot %u"
                              " | reqs %u max %ukpx",
                         ib.kpx_per_s[0], ib.kpx_per_s[1], ib.kpx_per_s[2],
                         ib.kpx_per_s[3], ib.kpx_per_s[4], ib.kpx_per_s[5],
                         ib.events, ib.max_px / 1000);
                if (ib.fullscreen) {
                    ESP_LOGW(TAG, "inval: %u FULL-SCREEN requests this window "
                                  "(%u kpx each = ~2 waterfall canvases of work)",
                             ib.fullscreen, (unsigned)(DISPLAY_H_RES * DISPLAY_V_RES / 1000));
                }

                /* The band plan's share, resolved into a call rate. bp_calls is
                 * every path into update_bandplan_strip, bp_poll only the
                 * unconditional defensive refresh from cat's poll_task. */
                unsigned bp_calls = 0, bp_poll = 0, bp_skip = 0;
                ui_bandplan_call_counts(&bp_calls, &bp_poll, &bp_skip);
                ESP_LOGI(TAG, "bandplan: %u repaints (%u from CAT poll, %u gated) | "
                              "bp reqs %u = %ukpx/s",
                         bp_calls, bp_poll, bp_skip, ib.reqs[4], ib.kpx_per_s[4]);
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
