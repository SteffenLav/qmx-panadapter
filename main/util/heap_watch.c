/* heap_watch.c - find out WHAT eats internal RAM, without a soak.
 *
 * WHY THIS EXISTS (2026-09-26)
 * ----------------------------
 * A 20.5 h idle capture settled the leak question: internal free trends
 * +0.072 KB/h - flat. There is no leak. What the same capture DID show is 84
 * separate events where `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` read
 * zero, against a stable ~22 KB baseline, and the dip rate does not grow with
 * uptime (3.1% of samples in hour 0, 3.6% in hour 20, 0.0% in hours 11-14).
 *
 * That is a recurring transient, not a leak, and it is where
 * esp_dma_capable_malloc() refuses - the measured root cause of SD "goes
 * yellow", and of BLE silently failing to start.
 *
 * ⛔ THE 10 s HEAP LINE IN audio.c CANNOT SEE IT. Every dip but one was a
 * single 10 s sample, so the event is somewhere under 10 s and its real
 * duration is unknown. Correlating the dips against every other log tag found
 * nothing: 5 `cat` lines, 4 `rx_audio` resample warnings and 3 SDIO drains
 * against 89 dip samples. The consumer does not log. So instrument it.
 *
 * WHAT THIS DOES
 * --------------
 * 1. Samples internal free every HEAP_WATCH_PERIOD_MS with the O(1) counter
 *    query only, and reports a dip as ONE line when it ENDS: how deep, how
 *    long, how many samples. Duration is the thing worth knowing first - it
 *    decides whether a dump can ever catch the hog (long dip) or whether only
 *    heap tracing will (short dip).
 *
 * 2. Names the victim of any allocation that actually FAILS, via
 *    heap_caps_register_failed_alloc_callback(). The callback runs in the
 *    failing task's own context, so the task name is the caller.
 *
 * ⛔ THE CALLBACK DOES NOT LOG. It may be entered from an ISR or with the heap
 * lock held, and ESP_LOGx from there can deadlock or corrupt the line. It
 * records into plain statics and the sampler task prints on change - the same
 * discipline sock_probe uses.
 *
 * ⛔ heap_caps_get_largest_free_block() WALKS THE HEAP WITH INTERRUPTS OFF and
 * must never go on a periodic path - that is what caused the FT4 cyan flash
 * (see audio.c's own note). It is called here ONCE per dip entry, which is 84
 * times in 20 hours, not 50 times a second.
 */

#include "util/heap_watch.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps */
#ifdef CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
#endif

static const char *TAG = "heapwatch";

/* 50 Hz. The counter query is O(1), so this costs nothing measurable, and it
 * resolves a dip to 20 ms where the old instrument resolved it to 10 s. */
#define HEAP_WATCH_PERIOD_MS   20

/* Baseline measured over 20.5 h is ~22 KB with a median of 22 KB and p5 of
 * 9 KB. 12 KB is below anything healthy and well above the 0 KB floor, so it
 * catches the approach rather than only the bottom. */
#define HEAP_WATCH_DIP_BYTES   (12 * 1024)

/* Written by the failed-alloc callback, read by the task. Not a queue: the
 * callback must not block, and only the most recent failure plus a count is
 * worth having. */
static volatile uint32_t s_fail_count;
static volatile uint32_t s_fail_size;
static volatile uint32_t s_fail_caps;
static char              s_fail_task[configMAX_TASK_NAME_LEN];

static void heap_watch_failed_alloc(size_t size, uint32_t caps, const char *fn)
{
    (void)fn;
    s_fail_size = (uint32_t)size;
    s_fail_caps = caps;
    /* pcTaskGetName on the CURRENT handle is safe from a task; from an ISR it
     * would not be, so guard it. A missing name is better than a crash while
     * reporting a failure. */
    if (!xPortInIsrContext()) {
        const char *n = pcTaskGetName(NULL);
        if (n) { strncpy(s_fail_task, n, sizeof(s_fail_task) - 1);
                 s_fail_task[sizeof(s_fail_task) - 1] = '\0'; }
    } else {
        strcpy(s_fail_task, "ISR");
    }
    s_fail_count++;          /* last, so a reader never sees a torn record */
}

#ifdef CONFIG_HEAP_TASK_TRACKING
/* ⛔ THE WALK TAKES INTERRUPTS OFF and is the documented cause of the
 * full-screen cyan flash (see dma_owners.c and audio.c). It must NEVER go on a
 * periodic path. That is why the baseline is taken ONCE, not refreshed, and
 * why the dip snapshots are capped at HEAP_WATCH_MAX_DUMPS: nine walks for the
 * whole run, not one every two seconds.
 *
 * ⚠ AND IT PERTURBS WHAT IT MEASURES - an owning-task handle in every block
 * header, so absolute free figures on this build read LOW. The ATTRIBUTION is
 * the answer here, never the totals. */
#define HEAP_WATCH_MAX_TASKS   48
#define HEAP_WATCH_MAX_DUMPS   8

static heap_task_totals_t *s_base;      /* one healthy snapshot */
static size_t              s_base_n;
static heap_task_totals_t *s_dip;       /* refilled at each dip entry */
static size_t              s_dip_n;
static int                 s_dumps;
static bool                s_have_base;
static uintptr_t           s_dip_seen[5];   /* tasks already printed this dump */

static void snap(heap_task_totals_t *into, size_t *n)
{
    heap_task_info_params_t p = { 0 };
    /* Slot 0 is the pool under investigation. Slot 1 is everything else
     * internal, so slot 0 is read against something rather than alone. */
    p.caps[0] = MALLOC_CAP_INTERNAL; p.mask[0] = MALLOC_CAP_INTERNAL;
    p.caps[1] = MALLOC_CAP_DMA;      p.mask[1] = MALLOC_CAP_DMA;
    p.totals = into; p.num_totals = n; p.max_totals = HEAP_WATCH_MAX_TASKS;
    /* ⛔ heap_caps_get_per_task_info ACCUMULATES into the totals array. Resetting
     * only the count made every dump after the first read as the SUM of all
     * previous dumps - 85328 -> 127992 -> 170656 B, rising by a fixed step,
     * which is arithmetic, not a measurement. Clear the array itself. */
    memset(into, 0, HEAP_WATCH_MAX_TASKS * sizeof(heap_task_totals_t));
    *n = 0;
    heap_caps_get_per_task_info(&p);
}

/* Print the tasks whose internal holding GREW most between the healthy
 * baseline and the dip. That difference is the hog, by name. */
static void report_deltas(void)
{
    for (int rank = 0; rank < 5; rank++) {
        long  best_d = 0;
        int   best_i = -1;
        for (size_t i = 0; i < s_dip_n; i++) {
            long b = 0;
            for (size_t j = 0; j < s_base_n; j++)
                if (s_base[j].task == s_dip[i].task) { b = (long)s_base[j].size[0]; break; }
            long d = (long)s_dip[i].size[0] - b;
            /* rank-th largest: skip anything already printed */
            if (d <= 0) continue;
            bool taken = false;
            for (int k = 0; k < rank; k++) if (s_dip[i].task == (TaskHandle_t)(uintptr_t)s_dip_seen[k]) taken = true;
            if (taken) continue;
            if (d > best_d) { best_d = d; best_i = (int)i; }
        }
        if (best_i < 0) break;
        s_dip_seen[rank] = (uintptr_t)s_dip[best_i].task;
        /* ⛔ The handle may belong to a task that has since been DELETED - that
         * is where the '???O' garbage name came from, and reading a freed TCB
         * is a crash waiting to happen. Only name a task the scheduler still
         * knows about. */
        TaskHandle_t th = s_dip[best_i].task;
        const char *nm = "(gone or pre-scheduler)";
        if (th && eTaskGetState(th) != eDeleted) {
            const char *n2 = pcTaskGetName(th);
            if (n2) nm = n2;
        }
        ESP_LOGW(TAG, "   +%ld B internal  <- task '%s' (holds %u B in %u blocks)",
                 best_d, nm ? nm : "?", (unsigned)s_dip[best_i].size[0],
                 (unsigned)s_dip[best_i].count[0]);
    }
}
#endif /* CONFIG_HEAP_TASK_TRACKING */

static void heap_watch_task(void *arg)
{
    (void)arg;
    bool     in_dip     = false;
    uint32_t dip_start  = 0;
    uint32_t dip_min    = UINT32_MAX;
    uint32_t dip_samples = 0;
    uint32_t dip_lblk   = 0;
    uint32_t seen_fails = 0;
    uint32_t peak_free  = 0;
    const char *dip_core0 = "?";

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEAP_WATCH_PERIOD_MS));

        uint32_t now  = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

        /* ⛔ PEAK MUST NOT INCLUDE THE BOOT HIGH. First attempt tracked the
         * peak from uptime 0, latched ~190 KB from early boot, and the
         * "free is near the peak" gate could then never fire again - the
         * instrument was silently dead for a whole flash cycle. Second time
         * I have made this exact mistake in one session: a gate whose failure
         * mode is silence must have a guaranteed fallback, below. */
        if (now > 60000 && free > peak_free) peak_free = free;

#ifdef CONFIG_HEAP_TASK_TRACKING
        /* One healthy baseline, once, after the boot storm has settled.
         *
         * ⛔ THE GATE MUST BE RELATIVE. It was `free > 20 KB` and never fired:
         * CONFIG_HEAP_TASK_TRACKING puts an owning-task handle in every block
         * header, so this build's ceiling is ~16 KB where the shipping build
         * sits at ~28 KB. A fixed threshold copied from the un-instrumented
         * build silently disabled the whole instrument. Track the peak this
         * build actually reaches and baseline near it. */
        if (!s_have_base && s_base && now > 90000 &&
            free > HEAP_WATCH_DIP_BYTES &&
            (free + 1024 >= peak_free || now > 180000)) {   /* fallback: take SOMETHING by 3 min */
            snap(s_base, &s_base_n);
            s_have_base = true;
            ESP_LOGW(TAG, "baseline taken at %u ms (free %u B, peak %u B): %u tasks",
                     (unsigned)now, (unsigned)free, (unsigned)peak_free,
                     (unsigned)s_base_n);
        }
#endif

        if (free < HEAP_WATCH_DIP_BYTES) {
            if (!in_dip) {
                in_dip      = true;
                dip_start   = now;
                dip_min     = free;
                dip_samples = 0;
                /* ONE walk, at the edge - see the header note. */
                dip_lblk = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                /* Which task is on the OTHER core at the instant the dip
                 * starts. O(1), no scheduler lock - independent of the
                 * per-task attribution below, and it still works on a build
                 * without CONFIG_HEAP_TASK_TRACKING. Core 1 would only ever
                 * name this sampler. */
                {
                    TaskHandle_t h = xTaskGetCurrentTaskHandleForCore(0);
                    const char *n = h ? pcTaskGetName(h) : NULL;
                    dip_core0 = n ? n : "?";
                }
#ifdef CONFIG_HEAP_TASK_TRACKING
                /* Snapshot INSIDE the dip - that is the whole point. Capped,
                 * because each one is a heap walk with interrupts off. */
                if (s_have_base && s_dip && s_dumps < HEAP_WATCH_MAX_DUMPS) {
                    snap(s_dip, &s_dip_n);
                    s_dumps++;
                }
#endif
            }
            if (free < dip_min) dip_min = free;
            dip_samples++;
        } else if (in_dip) {
            in_dip = false;
            ESP_LOGW(TAG, "DIP: internal fell to %u B (lblk at entry %u B) for %u ms "
                          "across %u samples - started at %u ms",
                     (unsigned)dip_min, (unsigned)dip_lblk,
                     (unsigned)(now - dip_start), (unsigned)dip_samples,
                     (unsigned)dip_start);
            ESP_LOGW(TAG, "   core 0 was running '%s' when it started", dip_core0);
#ifdef CONFIG_HEAP_TASK_TRACKING
            /* Printed AFTER the dip, so the dip path stays short. */
            if (s_dip_n) { report_deltas(); s_dip_n = 0; }
#endif
        }

        /* Report allocation failures from the task, never from the callback. */
        uint32_t fails = s_fail_count;
        if (fails != seen_fails) {
            ESP_LOGE(TAG, "ALLOC FAILED: %u B caps=0x%08x in task '%s' "
                          "(%u failure(s) so far, internal free now %u B)",
                     (unsigned)s_fail_size, (unsigned)s_fail_caps, s_fail_task,
                     (unsigned)fails, (unsigned)free);
            seen_fails = fails;
        }
    }
}

void heap_watch_start(void)
{
#ifdef CONFIG_HEAP_TASK_TRACKING
    /* Both arrays in PSRAM and allocated ONCE, up front: asking the question
     * must not consume the resource under investigation, and nothing may be
     * allocated on the dip path. */
    s_base = heap_caps_calloc(HEAP_WATCH_MAX_TASKS, sizeof(heap_task_totals_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dip  = heap_caps_calloc(HEAP_WATCH_MAX_TASKS, sizeof(heap_task_totals_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_base || !s_dip)
        ESP_LOGE(TAG, "no PSRAM for the per-task arrays - attribution is OFF");
    else
        ESP_LOGW(TAG, "per-task attribution ON (CONFIG_HEAP_TASK_TRACKING) - "
                      "absolute free figures on this build read LOW, use the deltas");
#endif

    esp_err_t err = heap_caps_register_failed_alloc_callback(heap_watch_failed_alloc);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "could not register the failed-alloc callback: %s",
                 esp_err_to_name(err));

    /* Core 1: the 20.5 h capture shows idle1 at 94-98% while core 0 is owned by
     * taskLVGL (85.9%, #284/#285). A sampler that must not miss a 20 ms window
     * cannot live on the starved core. PSRAM stack - this task never runs from
     * an ISR and holds nothing that needs internal RAM, and taking internal RAM
     * to measure internal RAM would be its own answer. */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        heap_watch_task, "heapwatch", 3072, NULL, 10, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS)
        ESP_LOGW(TAG, "heap watch task did not start");
    else
        ESP_LOGI(TAG, "watching internal RAM every %d ms, dip threshold %d B",
                 HEAP_WATCH_PERIOD_MS, HEAP_WATCH_DIP_BYTES);
}
