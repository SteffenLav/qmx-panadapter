<#
.SYNOPSIS
    Gives esp_hosted's six WiFi-link tasks stacks sized to what they use, and
    moves the two RPC control-path stacks to PSRAM - returning ~15 KB to the
    internal MALLOC_CAP_DMA pool.

.DESCRIPTION
    host/port/include/os_wrapper.h hardcodes DFLT_TASK_STACK_SIZE and
    RPC_TASK_STACK_SIZE to (5*1024) and IGNORES sdkconfig's
    CONFIG_ESP_HOSTED_DFLT_TASK_STACK (CLAUDE.md, #284). So six tasks take
    5,120 B each of internal, DMA-capable RAM - the pool whose exhaustion fails
    SD mounts, USB endpoint allocation, TLS, and (2026-09-11) esp_hosted's own
    RPC semaphores. That day a boot started with the pool at 99 bytes; the RPC
    path then leaked on every failure until sdio_write_task asserted at 24 min.

    Measured high-water (used of 5,120 B) on the dev bench, 470 s of a normal
    WSPR/WiFi/BLE/feeds session, 2026-09-11:

        rpc_rx          2,808      sdio_write        856
        sdio_read       2,748      rpc_tx            844
        sdio_process_rx 2,080      sdio_rx_buf       464

    Sized from that WITH HEADROOM FOR THE PATHS THAT DID NOT RUN - every log
    line costs ~1 KB here through the diag-log vprintf hook, and error/drain
    paths are exactly the ones a normal session never exercises (CLAUDE.md:
    tab5_kb was cut to its measured peak and crashed 18 minutes later):

        sdio_rx_buf, sdio_write     5,120 -> 3,072   (>= 2.2 KB headroom)
        sdio_process_rx             5,120 -> 4,096   (~2 KB headroom)
        sdio_read                   5,120 unchanged  (deep: RX drain paths)
        rpc_rx, rpc_tx              5,120, in PSRAM  (1 Hz control path, not
                                                      latency-critical)

    The RPC tasks are made with xTaskCreateWithCaps(); hosted_thread_cancel()
    deletes a PSRAM-stack task with vTaskDeleteWithCaps(), or its stack leaks.
    If the PSRAM create fails it falls back to the stock internal create.
    Sizes only ever SHRINK (a future esp_hosted that asks for less wins).

    Layered on top of apply_esp_hosted_assert_tolerant.ps1, which REPLACES
    os_wrapper.c wholesale - so this refuses to run until that one has, and
    tools/check_patches.py fails the build if either is missing.

    Edits managed_components/ (git-ignored): wiped by `idf.py fullclean`, a
    dependency refresh, or the release process's `rm -r managed_components/`.
    Idempotent.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_assert_tolerant.ps1
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_task_stacks.ps1
#>

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repo "managed_components/espressif__esp_hosted/host/port/src/os_wrapper.c"

if (-not (Test-Path $target)) {
    Write-Host "os_wrapper.c not found - run a build first, then re-run this." -ForegroundColor Red
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "QMX_HOSTED_TASK_STACKS") {
    Write-Host "Already patched (esp_hosted task stacks) - nothing to do." -ForegroundColor Green
    exit 0
}
if ($content -notmatch "QMX_PANADAPTER_ASSERT_TOLERANT_PATCH_MARKER") {
    Write-Host "Run apply_esp_hosted_assert_tolerant.ps1 FIRST - it replaces os_wrapper.c" -ForegroundColor Red
    Write-Host "wholesale and would discard this patch if it ran afterwards."
    exit 1
}

$contentLf = $content -replace "`r`n", "`n"

$edits = @(
    @{
        name = "includes"
        old  = "#include `"string.h`"`n"
        new  = @'
#include "string.h"
#include "esp_heap_caps.h"      /* QMX_HOSTED_TASK_STACKS: PSRAM stacks for the RPC tasks */
#include "esp_memory_utils.h"   /* esp_ptr_external_ram() - which delete to use */

'@
    },
    @{
        name = "create"
        old  = "`ttask_created = xTaskCreate((void (*)(void *))start_routine, tname, tstack_size, sr_arg, tprio, thread_handle);`n"
        new  = @'
	/* QMX_HOSTED_TASK_STACKS - see tools/patches/apply_esp_hosted_task_stacks.ps1.
	 * os_wrapper.h gives every task 5 KB of internal, DMA-capable RAM; these
	 * sizes come from measured high-water plus headroom for the unexercised
	 * error/log paths, and the RPC control tasks go to PSRAM. */
	uint32_t qmx_stack = tstack_size;
	bool     qmx_psram = false;
	if (tname) {
		if (!strcmp(tname, "sdio_rx_buf") || !strcmp(tname, "sdio_write"))
			qmx_stack = 3072;
		else if (!strcmp(tname, "sdio_process_rx"))
			qmx_stack = 4096;
		else if (!strcmp(tname, "rpc_rx") || !strcmp(tname, "rpc_tx"))
			qmx_psram = true;
	}
	if (qmx_stack > tstack_size) qmx_stack = tstack_size;   /* only ever shrink */
	task_created = pdFAIL;
	if (qmx_psram)
		task_created = xTaskCreateWithCaps((void (*)(void *))start_routine, tname,
				qmx_stack, sr_arg, tprio, thread_handle,
				MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (task_created != pdTRUE)
		task_created = xTaskCreate((void (*)(void *))start_routine, tname, qmx_stack, sr_arg, tprio, thread_handle);

'@
    },
    @{
        name = "cancel"
        old  = "`tvTaskDelete(*thread_hdl);`n"
        new  = @'
	/* QMX_HOSTED_TASK_STACKS: a PSRAM-stack task was made WithCaps and must be
	 * deleted WithCaps, or its stack and TCB are never freed. */
	if (esp_ptr_external_ram(pxTaskGetStackStart(*thread_hdl)))
		vTaskDeleteWithCaps(*thread_hdl);
	else
		vTaskDelete(*thread_hdl);

'@
    }
)

foreach ($e in $edits) {
    $old = $e.old -replace "`r`n", "`n"
    $new = $e.new -replace "`r`n", "`n"
    $count = ([regex]::Matches($contentLf, [regex]::Escape($old))).Count
    if ($count -ne 1) {
        Write-Host "Expected exactly one '$($e.name)' site in os_wrapper.c, found $count - esp_hosted may have changed." -ForegroundColor Red
        Write-Host "Patch by hand and update this script."
        exit 1
    }
    $contentLf = $contentLf.Replace($old, $new)
}

[System.IO.File]::WriteAllText($target, $contentLf, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Patched os_wrapper.c: esp_hosted task stacks sized, RPC stacks in PSRAM." -ForegroundColor Green
Write-Host "  $target"
