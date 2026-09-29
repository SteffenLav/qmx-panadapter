<#
.SYNOPSIS
    Moves taskLVGL's own stack from internal RAM to PSRAM.

.DESCRIPTION
    dma_owners.c's per-task attribution (2026-09-20, rx-audio session) found
    taskLVGL is the single biggest individual stack still in internal RAM:
    17,408 B, out of a MALLOC_CAP_DMA pool that settles to ~8-12 KB free
    within 3 minutes of every boot and stays there for the rest of the
    session (confirmed flat across a 180/420/600 s trace - not a leak, a
    fixed ceiling). esp_lvgl_port's own task creation (src/lvgl9/esp_lvgl_port.c)
    calls plain xTaskCreate()/xTaskCreatePinnedToCore() with no PSRAM option.

    taskLVGL is NOT on CLAUDE.md's psram-stack exclusion list (audio_task,
    fft_task, render_task, cat's link/poll tasks, ws_push_task - all excluded
    for measured latency reasons). It touches USB/DMA buffers nowhere directly;
    LVGL's own memory POOL has been in PSRAM since v0.19.2 (lv_pool_shim.h) with
    no measured cost, because pool allocation is cold compared to a task's own
    stack, which is touched on every function call/return - this is a
    DIFFERENT, hotter access pattern and is NOT assumed safe by analogy alone.

    ⚠ UNVERIFIED AS SHIPPED. This project has twice measured an "obviously
    safe" taskLVGL change and had hardware disagree (moving it to core 1
    starved fft_task; shrinking the rotation working set made idle WORSE, not
    better). Build, flash, and compare core-0 idle% / fps / touch latency
    against a baseline BEFORE trusting this. Revert immediately if idle drops
    or the UI feels laggy - see the two-attempts-already-falsified section of
    CLAUDE.md ("No long interrupts-off critical sections" chapter) for the
    precedent.

    Same idiom as apply_esp_hosted_task_stacks.ps1's RPC-pair move:
    xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    falling back to a plain internal create if the PSRAM allocation fails, and
    vTaskDeleteWithCaps() at both self-delete sites so the PSRAM stack is
    actually freed rather than leaked on lvgl_port_deinit().

    Edits managed_components/ (git-ignored): wiped by `idf.py fullclean`, a
    dependency refresh, or the release process's `rm -r managed_components/`.
    Idempotent.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_lvgl_port_task_psram.ps1
#>

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repo "managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c"

if (-not (Test-Path $target)) {
    Write-Host "esp_lvgl_port.c not found - run a build first, then re-run this." -ForegroundColor Red
    exit 1
}

$content = Get-Content -Raw -Path $target
$contentLf = $content -replace "`r`n", "`n"

if ($contentLf -match "QMX_LVGL_PORT_TASK_PSRAM") {
    Write-Host "Already patched (LVGL port task PSRAM stack) - nothing to do." -ForegroundColor Green
    exit 0
}

$edits = @(
    @{
        name = "includes"
        old  = "#include `"freertos/event_groups.h`"`n"
        new  = @'
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"      /* QMX_LVGL_PORT_TASK_PSRAM */
#include "esp_memory_utils.h"   /* esp_ptr_external_ram() */

'@
    },
    @{
        name = "create"
        old  = @'
    BaseType_t res;
    if (cfg->task_affinity < 0) {
        res = xTaskCreate(lvgl_port_task, "taskLVGL", cfg->task_stack, xTaskGetCurrentTaskHandle(), cfg->task_priority, &lvgl_port_ctx.lvgl_task);
    } else {
        res = xTaskCreatePinnedToCore(lvgl_port_task, "taskLVGL", cfg->task_stack, xTaskGetCurrentTaskHandle(), cfg->task_priority, &lvgl_port_ctx.lvgl_task, cfg->task_affinity);
    }
'@
        new  = @'
    /* QMX_LVGL_PORT_TASK_PSRAM - see tools/patches/apply_lvgl_port_task_psram.ps1.
     * taskLVGL's own stack (17,408 B measured) is the single biggest stack
     * still in internal RAM. Falls back to the stock internal create if the
     * PSRAM allocation fails, so this can never make boot worse than before. */
    BaseType_t res;
    BaseType_t qmx_core = (cfg->task_affinity < 0) ? tskNO_AFFINITY : cfg->task_affinity;
    res = xTaskCreatePinnedToCoreWithCaps(lvgl_port_task, "taskLVGL", cfg->task_stack,
            xTaskGetCurrentTaskHandle(), cfg->task_priority, &lvgl_port_ctx.lvgl_task,
            qmx_core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (res != pdPASS) {
        if (cfg->task_affinity < 0) {
            res = xTaskCreate(lvgl_port_task, "taskLVGL", cfg->task_stack, xTaskGetCurrentTaskHandle(), cfg->task_priority, &lvgl_port_ctx.lvgl_task);
        } else {
            res = xTaskCreatePinnedToCore(lvgl_port_task, "taskLVGL", cfg->task_stack, xTaskGetCurrentTaskHandle(), cfg->task_priority, &lvgl_port_ctx.lvgl_task, cfg->task_affinity);
        }
    }
'@
    },
    @{
        name = "self-delete (both sites)"
        old  = "vTaskDelete( NULL );"
        new  = @'
/* QMX_LVGL_PORT_TASK_PSRAM: a PSRAM-stack task made WithCaps must be
             * deleted WithCaps, or its stack and TCB are never freed. */
            esp_ptr_external_ram(pxTaskGetStackStart(NULL)) ? vTaskDeleteWithCaps(NULL) : vTaskDelete(NULL);
'@
        count_min = 2
    }
)

foreach ($e in $edits) {
    $old = $e.old -replace "`r`n", "`n"
    $new = $e.new -replace "`r`n", "`n"
    $count = ([regex]::Matches($contentLf, [regex]::Escape($old))).Count
    $needMin = if ($e.count_min) { $e.count_min } else { 1 }
    if ($count -lt $needMin) {
        Write-Host "Expected at least $needMin '$($e.name)' site(s) in esp_lvgl_port.c, found $count - the component may have changed." -ForegroundColor Red
        Write-Host "Patch by hand and update this script."
        exit 1
    }
    $contentLf = $contentLf.Replace($old, $new)
}

[System.IO.File]::WriteAllText($target, $contentLf, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Patched esp_lvgl_port.c: taskLVGL stack now tries PSRAM first." -ForegroundColor Green
Write-Host "  $target"
