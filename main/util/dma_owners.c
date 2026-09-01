// dma_owners.c - who actually owns MALLOC_CAP_DMA?
//
// === TEMP INSTRUMENT (#283). DELETE WITH THE DIAGNOSIS - see the checklist in
// === TODO #283. This file is deliberately self-contained so removing it is one
// === `rm` plus three call-site lines.
//
// WHY THIS EXISTS
//   The DMA-capable pool is the scarce resource on this board, and CLAUDE.md
//   records it at 40-41 KB with WiFi fully up after the TODO #65 .bss fix
//   (2026-08-05). On 2026-08-28 it measures ~8 KB with Bluetooth off and ~4.7 KB
//   with it on - and at 4.7 KB FatFs cannot get a sector buffer, so fopen()
//   returns EIO, the SD archive's slow mirror fails three times and unmounts a
//   perfectly good card (the operator watched the dot vanish).
//
//   Three candidates were eliminated by measurement rather than argument: .bss
//   creep (154,290 B in libmain, matching the 153,906 already recorded, so no
//   drift), the NimBLE tuning going missing (present in sdkconfig AND
//   sdkconfig.defaults), and Bluetooth itself (worth ~3.4 KB - the straw, not
//   the load). Eliminating one subsystem per reboot from there is guesswork
//   with a QMX power cycle attached to each guess.
//
//   heap_caps_get_per_task_info() partitions per-task allocation totals BY
//   CAPABILITY, so it answers the question directly: which task is holding the
//   DMA-capable bytes. That is a name, not another elimination.
//
// ⛔ ON DEMAND ONLY - NEVER ON A PERIODIC PATH.
//   This walks the heap with interrupts off, which is the documented cause of
//   the full-screen cyan flash on this panel (see the note in audio.c, and #281
//   where exactly this call sat on the FT8 per-slot path at 7.5 s intervals).
//   One invocation may cost a one-frame blink; CLAUDE.md permits that for
//   on-demand diagnostics and for nothing else.
//
// ⚠ THE INSTRUMENT PERTURBS WHAT IT MEASURES. CONFIG_HEAP_TASK_TRACKING stores
//   an owning-task handle in every block header, so absolute free figures on
//   this build are LOWER than on a shipping build. The ATTRIBUTION is what this
//   is for - who holds the bytes, in what proportion - not the totals. Do not
//   quote a dma-free number measured on a tracking build as if it were normal.

#include "dma_owners.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"   // esp_ptr_dma_capable

#ifdef CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "dmaown";

#define MAX_TASKS 48

void dma_owners_report(void)
{
    // The totals array is big; put it in PSRAM so asking the question does not
    // itself consume the resource under investigation.
    heap_task_totals_t *totals =
        heap_caps_calloc(MAX_TASKS, sizeof(heap_task_totals_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!totals) {
        ESP_LOGE(TAG, "could not allocate the totals array");
        return;
    }
    size_t num_totals = 0;

    // Partition on the capability under investigation. Slot 0 collects blocks
    // in DMA-capable regions; slot 1 collects everything else internal, so the
    // two can be compared rather than one being read in isolation.
    heap_task_info_params_t p = { 0 };
    p.caps[0] = MALLOC_CAP_DMA;      p.mask[0] = MALLOC_CAP_DMA;
    p.caps[1] = MALLOC_CAP_INTERNAL; p.mask[1] = MALLOC_CAP_INTERNAL;
    p.tasks       = NULL;            // every task
    p.num_tasks   = 0;
    p.totals      = totals;
    p.num_totals  = &num_totals;
    p.max_totals  = MAX_TASKS;
    p.blocks      = NULL;            // totals only - block detail is far larger
    p.max_blocks  = 0;

    heap_caps_get_per_task_info(&p);

    // Snapshot the LIVE task handles once, so each row can be marked LIVE or
    // DEAD. The first run of this instrument attributed 146,120 B in 184 blocks
    // to a task whose name came back blank - which is what a DELETED task looks
    // like, its allocations still held. That is the difference between "WiFi is
    // simply big" and "something leaks", and guessing between those two is what
    // this exists to avoid.
    //
    // ⛔ uxTaskGetSystemState() byte-walks EVERY task's stack inside a kernel
    // critical section - the same documented cyan-flash cause as the heap walk
    // above (see CLAUDE.md, and #281). Acceptable here for the same reason and
    // only that reason: this is on demand, never periodic.
    TaskStatus_t *live = NULL;
    UBaseType_t n_live = 0;
    {
        UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;
        live = heap_caps_calloc(cap, sizeof(TaskStatus_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (live) n_live = uxTaskGetSystemState(live, cap, NULL);
    }

    ESP_LOGW(TAG, "=== MALLOC_CAP_DMA owners (free now: %u B, %u live tasks) ===",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), (unsigned)n_live);
    ESP_LOGW(TAG, "%-18s %10s %7s %10s  %-10s %s",
             "task", "DMA bytes", "blocks", "int bytes", "handle", "state");

    size_t dma_sum = 0, dead_sum = 0;
    unsigned dead_blocks = 0;
    for (size_t i = 0; i < num_totals; i++) {
        if (totals[i].size[0] == 0 && totals[i].size[1] == 0) continue;

        const char *state = "pre-sched";
        if (totals[i].task) {
            state = "DEAD";                       // until found in the live list
            for (UBaseType_t k = 0; k < n_live; k++) {
                if (live[k].xHandle == totals[i].task) { state = "live"; break; }
            }
        }
        // Do NOT call pcTaskGetName() on a handle that is not in the live list:
        // a deleted task's TCB has been freed, and reading its name array is a
        // use-after-free. Print the handle instead and let the state say why
        // there is no name.
        const char *name = "-";
        if (totals[i].task == NULL)            name = "pre-scheduler";
        else if (state[0] == 'l')              name = pcTaskGetName(totals[i].task);

        ESP_LOGW(TAG, "%-18s %10u %7u %10u  %-10p %s",
                 name ? name : "?",
                 (unsigned)totals[i].size[0], (unsigned)totals[i].count[0],
                 (unsigned)totals[i].size[1],
                 (void *)totals[i].task, state);

        dma_sum += totals[i].size[0];
        if (state[0] == 'D') { dead_sum += totals[i].size[0]; dead_blocks += totals[i].count[0]; }
    }
    // ⛔ "DEAD" IS NOT "LEAKED", and the first version of this line said it was.
    //
    // The `main` task runs app_main() - display, LVGL, USB host, WiFi bring-up -
    // and FreeRTOS deletes it when app_main returns. Every long-lived structure
    // allocated there is therefore attributed to a handle that no longer
    // resolves, while the memory is perfectly live and in use. `bt_start` is the
    // same pattern in miniature: it self-deletes once NimBLE is up.
    //
    // So this line reports a FACT (whose handle is gone) and refuses to draw the
    // conclusion. A leak would need the blocks to be unreachable, which this
    // cannot see. Measured 2026-08-28: 146,136 B on one early handle - almost
    // certainly app_main's - plus 1,092 B on bt_start.
    if (dead_sum) {
        ESP_LOGW(TAG, "%u B in %u blocks are owned by handles that are no longer "
                      "live tasks. NOT necessarily leaked: app_main's task and "
                      "bt_start both self-delete after allocating long-lived "
                      "structures. Check the handle against boot order first.",
                 (unsigned)dead_sum, dead_blocks);
    } else {
        ESP_LOGW(TAG, "every allocation belongs to a live task");
    }
    ESP_LOGW(TAG, "--- total attributed DMA-capable: %u B over %u tasks ---",
             (unsigned)dma_sum, (unsigned)num_totals);

    // ---------------------------------------------------------------------
    // PER-BLOCK BREAKDOWN. The totals say WHO; this says WHAT, which is the
    // only view that shows what could be moved.
    //
    // The operator's point, and it is the real one: if the DMA-capable region
    // is fully committed then no new feature can land - the CW page included -
    // and "all resources are eaten up" becomes the honest thing to tell users.
    // #65 recovered 52 KB by moving named buffers to PSRAM, so the way out is
    // to name them again rather than to shave features.
    //
    // Only DMA-capable blocks are printed: the caps of the heap a block lives
    // in are not in heap_task_block_t, so each address is asked directly.
    {
        enum { MAX_BLOCKS = 2400, TOP_N = 34 };
        heap_task_block_t *blocks =
            heap_caps_calloc(MAX_BLOCKS, sizeof(heap_task_block_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (blocks) {
            size_t num_totals2 = 0;
            heap_task_info_params_t bp = { 0 };
            bp.caps[0] = MALLOC_CAP_DMA; bp.mask[0] = MALLOC_CAP_DMA;
            bp.tasks      = NULL;         // every task
            bp.num_tasks  = 0;
            bp.totals     = NULL;
            bp.num_totals = &num_totals2;
            bp.max_totals = 0;
            bp.blocks     = blocks;
            bp.max_blocks = MAX_BLOCKS;
            size_t n = heap_caps_get_per_task_info(&bp);

            // Keep only the DMA-capable ones, then selection-sort the largest
            // TOP_N into place. A full sort of 2400 entries is not worth the
            // stack or the time when only the head is read.
            size_t m = 0;
            for (size_t i = 0; i < n; i++) {
                if (esp_ptr_dma_capable(blocks[i].address)) blocks[m++] = blocks[i];
            }

            // -------------------------------------------------------------
            // SIZE HISTOGRAM. The top-N list showed 17 blocks of 4,352 B and
            // 7 of 5,376 B and that repetition is the whole lead: identical
            // sizes allocated many times are a POOL, and a pool has a count
            // somebody chose. But a top-30 list can only ever show the pools
            // whose members are among the 30 biggest blocks - a pool of 40 x
            // 1,536 B is 61 KB and completely invisible there. Group by size
            // instead, so every pool is visible whatever its element size.
            enum { MAX_SIZES = 96, HIST_ROWS = 18 };
            struct { unsigned size, count; } *hist =
                heap_caps_calloc(MAX_SIZES, sizeof(*hist),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (hist) {
                size_t nh = 0, overflow = 0;
                for (size_t i = 0; i < m; i++) {
                    size_t k = 0;
                    while (k < nh && hist[k].size != blocks[i].size) k++;
                    if (k < nh)            hist[k].count++;
                    else if (nh < MAX_SIZES) { hist[nh].size = blocks[i].size;
                                               hist[nh].count = 1; nh++; }
                    else                   overflow++;
                }
                size_t hshow = (nh < HIST_ROWS) ? nh : HIST_ROWS;
                for (size_t i = 0; i < hshow; i++) {   // by total bytes, desc
                    size_t best = i;
                    for (size_t k = i + 1; k < nh; k++)
                        if ((size_t)hist[k].size * hist[k].count >
                            (size_t)hist[best].size * hist[best].count) best = k;
                    typeof(*hist) t = hist[i]; hist[i] = hist[best]; hist[best] = t;
                }
                ESP_LOGW(TAG, "=== DMA blocks GROUPED BY SIZE (%u distinct sizes%s) ===",
                         (unsigned)nh, overflow ? ", some not counted" : "");
                for (size_t i = 0; i < hshow; i++)
                    ESP_LOGW(TAG, "  %4u x %6u B = %7u B%s",
                             hist[i].count, hist[i].size,
                             hist[i].size * hist[i].count,
                             hist[i].count >= 4 ? "   <-- repeated: a pool?" : "");
                heap_caps_free(hist);
            }

            size_t show = (m < TOP_N) ? m : TOP_N;
            for (size_t i = 0; i < show; i++) {
                size_t best = i;
                for (size_t k = i + 1; k < m; k++)
                    if (blocks[k].size > blocks[best].size) best = k;
                heap_task_block_t t = blocks[i]; blocks[i] = blocks[best]; blocks[best] = t;
            }

            size_t all = 0;
            for (size_t i = 0; i < m; i++) all += blocks[i].size;
            ESP_LOGW(TAG, "=== biggest DMA-capable BLOCKS (%u seen%s, %u B total) ===",
                     (unsigned)m, (n >= MAX_BLOCKS) ? ", TRUNCATED - raise MAX_BLOCKS" : "",
                     (unsigned)all);
            size_t head = 0, stack_bytes = 0;
            unsigned stack_blocks = 0;
            for (size_t i = 0; i < show; i++) {
                head += blocks[i].size;
                const char *nm = "(dead/main)";
                for (UBaseType_t k = 0; k < n_live; k++)
                    if (live[k].xHandle == blocks[i].task) { nm = live[k].pcTaskName; break; }

                // ⭐ WHAT the block IS, not just who allocated it.
                //
                // The allocating task is the wrong question for a task stack:
                // xTaskCreate() takes the stack from the CALLER's heap context,
                // so every stack created during app_main() is attributed to a
                // handle that no longer resolves, and every stack created by a
                // subsystem's own bring-up task is filed under that task. Six
                // 5,376 B blocks under `ipc0` say nothing about what they hold.
                //
                // TaskStatus_t.pxStackBase IS the pointer xTaskCreate() got
                // back from the allocator, and xHandle IS the TCB's address, so
                // both can be matched EXACTLY against a block address. No
                // heuristic, no size guessing: a hit names the task the memory
                // belongs to, which is the thing that can actually be moved to
                // PSRAM (psram_task_create) or shrunk.
                // ⚠ The match is CONTAINMENT, not equality, and the first
                // version of this got it wrong and reported nothing at all -
                // which read exactly like "none of these are task stacks" and
                // would have sent the whole reclamation pass down a blind
                // alley. CONFIG_HEAP_TASK_TRACKING stores the owning-task
                // handle in the FIRST word of the block and hands the caller
                // back address+4 (multi_heap.c's ADD_BLOCK_OWNER_OFFSET), while
                // heap_task_info reports the block's own address. So a stack
                // pointer can never equal a block address on a tracking build.
                // Containment is immune to that and to any other header the
                // allocator may grow later.
                const char *what = "";
                char lbl[56];
                for (UBaseType_t k = 0; k < n_live; k++) {
                    uintptr_t lo = (uintptr_t)blocks[i].address;
                    uintptr_t hi = lo + blocks[i].size;
                    uintptr_t sb = (uintptr_t)live[k].pxStackBase;
                    uintptr_t tcb = (uintptr_t)live[k].xHandle;
                    // ⚠ Bound the offset. Plain containment let the 32,772 B
                    // block claim to be the stack of every task whose stack
                    // happened to sit at a higher address, because it was
                    // scanned first and is enormous - it reported httpd at
                    // "+4380", which is the tell. A stack begins within the
                    // first few bytes of its own block (4 for the owner word),
                    // so anything past OFF_MAX is a different block entirely.
                    if (sb >= lo && sb < hi && (sb - lo) <= 256) {
                        snprintf(lbl, sizeof(lbl), "  <== STACK of %s (hwm %u, +%u)",
                                 live[k].pcTaskName,
                                 (unsigned)live[k].usStackHighWaterMark,
                                 (unsigned)(sb - lo));
                        what = lbl;
                        stack_bytes += blocks[i].size; stack_blocks++;
                        break;
                    }
                    if (tcb >= lo && tcb < hi) {
                        snprintf(lbl, sizeof(lbl), "  <== TCB of %s",
                                 live[k].pcTaskName);
                        what = lbl;
                        break;
                    }
                }
                ESP_LOGW(TAG, "  %8u B  @%p  alloc'd by %-14s%s",
                         (unsigned)blocks[i].size, blocks[i].address, nm, what);
            }
            if (stack_blocks)
                ESP_LOGW(TAG, "  of the top %u, %u blocks totalling %u B are TASK "
                              "STACKS still in internal RAM",
                         (unsigned)show, stack_blocks, (unsigned)stack_bytes);
            ESP_LOGW(TAG, "  ... top %u account for %u B of %u B (%u%%)",
                     (unsigned)show, (unsigned)head, (unsigned)all,
                     all ? (unsigned)((head * 100) / all) : 0);

            // -------------------------------------------------------------
            // TASK STACK CENSUS - the actionable total.
            //
            // CLAUDE.md's rule is already "use psram_task_create() instead of
            // xTaskCreate by default", and 25+ call sites in main/ obey it. But
            // that rule can only reach OUR tasks: every task an IDF or managed
            // component starts for itself (httpd, tiT, the esp_hosted transport
            // tasks, nimble_host, mdns, usb_lib...) takes its stack from
            // internal RAM, and each of those has a Kconfig size. Listing them
            // with their high-water marks says which are oversized, which is a
            // number rather than an opinion.
            //
            // A task whose stack is NOT in this list is either in PSRAM already
            // or statically allocated, and is therefore not part of the problem.
            {
                size_t int_stack = 0, ext_stack = 0;
                unsigned int_n = 0, ext_n = 0;
                ESP_LOGW(TAG, "=== TASK STACKS IN INTERNAL RAM (size, high-water) ===");
                for (UBaseType_t k = 0; k < n_live; k++) {
                    void *base = (void *)live[k].pxStackBase;
                    if (!base) continue;
                    if (esp_ptr_external_ram(base)) { ext_n++; continue; }
                    size_t sz = 0;
                    for (size_t i = 0; i < m; i++) {   // containment - see above
                        uintptr_t lo = (uintptr_t)blocks[i].address;
                        if ((uintptr_t)base >= lo &&
                            (uintptr_t)base <  lo + blocks[i].size &&
                            ((uintptr_t)base - lo) <= 256) {   /* see above */
                            sz = blocks[i].size; break;
                        }
                    }
                    int_n++; int_stack += sz;
                    if (sz)
                        ESP_LOGW(TAG, "  %-16s %6u B  hwm %5u B  core %d  pri %u",
                                 live[k].pcTaskName, (unsigned)sz,
                                 (unsigned)live[k].usStackHighWaterMark,
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
                                 (int)live[k].xCoreID,
#else
                                 0,
#endif
                                 (unsigned)live[k].uxCurrentPriority);
                    else
                        ESP_LOGW(TAG, "  %-16s      ? B  hwm %5u B  base %p  "
                                      "(static, or outside the DMA region)",
                                 live[k].pcTaskName,
                                 (unsigned)live[k].usStackHighWaterMark, base);
                }
                (void)ext_stack;
                ESP_LOGW(TAG, "  --- %u internal-stack tasks holding %u B; "
                              "%u tasks already on PSRAM stacks ---",
                         int_n, (unsigned)int_stack, ext_n);
            }
            heap_caps_free(blocks);
        } else {
            ESP_LOGW(TAG, "could not allocate the blocks array");
        }
    }

    if (live) heap_caps_free(live);
    heap_caps_free(totals);
}

#else  /* !CONFIG_HEAP_TASK_TRACKING */

void dma_owners_report(void)
{
    ESP_LOGW("dmaown", "per-task heap tracking is not compiled in "
                       "(CONFIG_HEAP_TASK_TRACKING) - nothing to report");
}

#endif

/* =========================================================================
 * WHO IS EATING CORE 0?  (#284, 2026-08-28)
 *
 * === TEMP INSTRUMENT. Delete with dma_owners.c - same checklist. ===
 *
 * WHY: the panadapter has sat at 0-7% core-0 idle for months and CLAUDE.md's
 * v1.8.3 note calls it "unexplained". The software 90-degree rotation was the
 * obvious suspect and was MEASURED OUT on this date: neutering rotate90_rgb565
 * so the screen showed garbage while everything else stayed byte-identical
 * moved idle0 from 6.9-7.6% to only 14.3-15.4%. So the rotation is worth ~7
 * points and something ELSE holds ~85% of the core.
 *
 * Guessing which task that is costs a QMX power cycle per guess, and today
 * already spent three of those on theories that hardware refused (a cache
 * working set, a smaller flush strip, moving LVGL to core 1). So: measure.
 * CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is already on, so every task's
 * ulRunTimeCounter is available; two snapshots a second apart give each task's
 * share directly, which is a name rather than another theory.
 *
 * ⛔ ON DEMAND ONLY. uxTaskGetSystemState() byte-walks every task's stack
 * inside a kernel critical section - the documented cyan-flash cause - and this
 * calls it TWICE. One invocation may cost a frame blink; that is the price
 * CLAUDE.md permits for an on-demand diagnostic and for nothing else.
 *
 * ⚠ The counter is summed over BOTH cores, so the percentages total ~200%, not
 * 100%. Read a task's share against the core its `core` column names; an
 * unpinned task (core -1) may have run on either.
 * ========================================================================= */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

/* Own tag: this function lives OUTSIDE the CONFIG_HEAP_TASK_TRACKING guard
 * above (it needs no per-block owner, only the run-time counters), so it cannot
 * borrow that block's static TAG. */
static const char *TAG_CPU = "cpuown";

void cpu_owners_report(void)
{
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;
    TaskStatus_t *a = heap_caps_calloc(cap, sizeof(TaskStatus_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TaskStatus_t *b = heap_caps_calloc(cap, sizeof(TaskStatus_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a || !b) {
        ESP_LOGE(TAG_CPU, "cpu_owners: out of PSRAM for the snapshots");
        if (a) heap_caps_free(a);
        if (b) heap_caps_free(b);
        return;
    }

    uint32_t t0 = 0, t1 = 0;
    UBaseType_t na = uxTaskGetSystemState(a, cap, &t0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    UBaseType_t nb = uxTaskGetSystemState(b, cap, &t1);

    uint32_t total = t1 - t0;
    if (total == 0) total = 1;      /* never divide by zero on a quiet counter */

    ESP_LOGW(TAG_CPU, "=== CPU over 1 s (share of the 2-core total; ~200%% = both "
                  "cores busy) ===");
    ESP_LOGW(TAG_CPU, "%-18s %7s %6s %5s  %s", "task", "cpu%", "core", "pri", "state");

    /* Selection-sort the busiest to the front. ~50 tasks, printed head only. */
    for (UBaseType_t i = 0; i < nb; i++) {
        UBaseType_t best = i;
        uint32_t best_d = 0;
        for (UBaseType_t k = i; k < nb; k++) {
            uint32_t d = 0;
            for (UBaseType_t j = 0; j < na; j++)
                if (a[j].xHandle == b[k].xHandle) {
                    d = b[k].ulRunTimeCounter - a[j].ulRunTimeCounter;
                    break;
                }
            if (d >= best_d) { best_d = d; best = k; }
        }
        TaskStatus_t t = b[i]; b[i] = b[best]; b[best] = t;
    }

    for (UBaseType_t i = 0; i < nb && i < 16; i++) {
        uint32_t d = 0;
        bool seen = false;
        for (UBaseType_t j = 0; j < na; j++)
            if (a[j].xHandle == b[i].xHandle) {
                d = b[i].ulRunTimeCounter - a[j].ulRunTimeCounter; seen = true; break;
            }
        /* Tenths of a percent without floating point in a log line. */
        unsigned pct10 = (unsigned)(((uint64_t)d * 1000) / total);
        ESP_LOGW(TAG_CPU, "%-18s %4u.%u%% %6d %5u  %s",
                 b[i].pcTaskName, pct10 / 10, pct10 % 10,
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
                 (b[i].xCoreID == tskNO_AFFINITY) ? -1 : (int)b[i].xCoreID,
#else
                 0,
#endif
                 (unsigned)b[i].uxCurrentPriority,
                 seen ? "" : "(new since the first snapshot)");
    }

    heap_caps_free(b);
    heap_caps_free(a);
}
