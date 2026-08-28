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
        enum { MAX_BLOCKS = 900, TOP_N = 30 };
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
            // TOP_N into place. A full sort of 900 entries is not worth the
            // stack or the time when only the head is read.
            size_t m = 0;
            for (size_t i = 0; i < n; i++) {
                if (esp_ptr_dma_capable(blocks[i].address)) blocks[m++] = blocks[i];
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
            size_t head = 0;
            for (size_t i = 0; i < show; i++) {
                head += blocks[i].size;
                const char *nm = "(dead/main)";
                for (UBaseType_t k = 0; k < n_live; k++)
                    if (live[k].xHandle == blocks[i].task) { nm = live[k].pcTaskName; break; }
                ESP_LOGW(TAG, "  %8u B  @%p  %s",
                         (unsigned)blocks[i].size, blocks[i].address, nm);
            }
            ESP_LOGW(TAG, "  ... top %u account for %u B of %u B (%u%%)",
                     (unsigned)show, (unsigned)head, (unsigned)all,
                     all ? (unsigned)((head * 100) / all) : 0);
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
