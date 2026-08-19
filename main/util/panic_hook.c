// See panic_hook.h for why this exists and why it does NOT touch flash.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/panic_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"

#include "panic_hook.h"

static const char *TAG = "panic_hook";

#define PANIC_REC_MAGIC   0x514D5843u   // 'QMXC'
#define PANIC_REC_VERSION 1

// RTC no-init: kept across a warm reset, and NOT zeroed by the startup code.
// Every panic reset is warm, which is the whole basis of this mechanism.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t uptime_ms;      // how far into the run it died - often the tell
    int32_t  core;
    uint32_t mepc;           // faulting PC: feed to addr2line
    uint32_t mtval;          // faulting address for a load/store fault
    uint32_t mcause;
    uint32_t ra;             // return address: one frame of caller context
    uint32_t sp;
    char     reason[40];     // e.g. "Load access fault", "abort()"
    char     desc[48];
    char     details[96];    // assert text / abort details, the most useful field
    char     task[20];
} panic_rec_t;

static RTC_NOINIT_ATTR panic_rec_t s_rec;

// Consumed into these at boot so the record can be cleared immediately - a
// second crash during start-up must not be able to overwrite the report we are
// about to print.
static panic_rec_t s_prev;
static bool        s_prev_valid;

// ---------------------------------------------------------------------------
// Panic context. Interrupts are off, the other core is halted, the cache may be
// disabled. Nothing here may allocate, take a lock, log, or touch flash.
// ---------------------------------------------------------------------------

// Bounded copy that tolerates a NULL or unmapped-looking source. A panic
// handler chasing a corrupt pointer would fault again and lose the report
// entirely, so anything outside the plausible address ranges is skipped.
static void IRAM_ATTR safe_copy(char *dst, size_t cap, const char *src)
{
    dst[0] = '\0';
    if (!src) return;
    uintptr_t a = (uintptr_t)src;
    // DROM/IROM/PSRAM/SRAM on the P4. A wild pointer is far likelier to be
    // small or 0xFFFF'FFFF-ish than to land inside one of these.
    if (a < 0x40000000u || a > 0x60000000u) {
        if (a < 0x4FF00000u || a > 0x50100000u) return;
    }
    size_t i = 0;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    // Record first, then hand straight over. If anything below were to fault we
    // would lose the panic entirely, so the whole body is defensive and short.
    memset(&s_rec, 0, sizeof(s_rec));
    s_rec.magic     = PANIC_REC_MAGIC;
    s_rec.version   = PANIC_REC_VERSION;
    s_rec.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (info) {
        s_rec.core = info->core;
        safe_copy(s_rec.reason, sizeof(s_rec.reason), info->reason);
        safe_copy(s_rec.desc,   sizeof(s_rec.desc),   info->description);

        if (info->frame) {
            const RvExcFrame *f = (const RvExcFrame *)info->frame;
            s_rec.mepc   = (uint32_t)f->mepc;
            s_rec.mtval  = (uint32_t)f->mtval;
            s_rec.mcause = (uint32_t)f->mcause;
            s_rec.ra     = (uint32_t)f->ra;
            s_rec.sp     = (uint32_t)f->sp;
        }
        if (!s_rec.mepc && info->addr) {
            s_rec.mepc = (uint32_t)(uintptr_t)info->addr;
        }
    }

    // The assert string. This is the single most useful field - "assert failed:
    // xQueueSemaphoreTake queue.c:1709" names the bug outright.
    if (g_panic_abort && g_panic_abort_details) {
        safe_copy(s_rec.details, sizeof(s_rec.details), g_panic_abort_details);
    }

    // Which task died. pcTaskGetName() walks no lists and takes no lock for the
    // current TCB, so it is safe here; it is also exactly what identifies a
    // stack-protection fault.
    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    if (t) safe_copy(s_rec.task, sizeof(s_rec.task), pcTaskGetName(t));

    __real_esp_panic_handler(info);
}

// ---------------------------------------------------------------------------
// Next boot.
// ---------------------------------------------------------------------------

bool panic_hook_previous_was_crash(void)
{
    return s_prev_valid;
}

void panic_hook_report_previous(void)
{
    if (s_rec.magic == PANIC_REC_MAGIC && s_rec.version == PANIC_REC_VERSION) {
        s_prev = s_rec;
        s_prev_valid = true;
    }
    // Clear before printing, not after: a crash while reporting must not leave
    // a record that reports itself forever.
    s_rec.magic = 0;

    if (!s_prev_valid) return;

    const panic_rec_t *r = &s_prev;
    ESP_LOGE(TAG, "=== THE PREVIOUS BOOT CRASHED - recovered from RTC memory ===");
    ESP_LOGE(TAG, "  reason : %s%s%s", r->reason[0] ? r->reason : "(unknown)",
             r->desc[0] ? " - " : "", r->desc);
    if (r->details[0]) {
        ESP_LOGE(TAG, "  details: %s", r->details);
    }
    ESP_LOGE(TAG, "  task   : %s   core %d   after %u.%03u s of uptime",
             r->task[0] ? r->task : "(unknown)", (int)r->core,
             (unsigned)(r->uptime_ms / 1000), (unsigned)(r->uptime_ms % 1000));
    ESP_LOGE(TAG, "  MEPC=0x%08x MTVAL=0x%08x MCAUSE=0x%08x RA=0x%08x SP=0x%08x",
             (unsigned)r->mepc, (unsigned)r->mtval, (unsigned)r->mcause,
             (unsigned)r->ra, (unsigned)r->sp);
    ESP_LOGE(TAG, "  decode : riscv32-esp-elf-addr2line -e build/qmx_panadapter.elf 0x%08x 0x%08x",
             (unsigned)r->mepc, (unsigned)r->ra);
    ESP_LOGE(TAG, "=== end of crash record ===");
}
