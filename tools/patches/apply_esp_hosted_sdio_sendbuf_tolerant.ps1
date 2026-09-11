<#
.SYNOPSIS
    Makes esp_hosted's SDIO TX task drop a frame when its send buffer cannot be
    allocated, instead of assert()-rebooting the device.

.DESCRIPTION
    host/drivers/transport/sdio/sdio_drv.c's sdio_write_task() has:

        sendbuf = sdio_buffer_alloc(MEMSET_REQUIRED);
        assert(sendbuf);
        ...
        if (!sendbuf) {
            ESP_LOGE(TAG, "sdio buff malloc failed");
            free_func = NULL;
            goto done;
        }

    i.e. a working drop path sits directly under the assert - the same shape
    apply_esp_hosted_assert_tolerant.ps1 and Uwe DL8UG's
    apply_esp_hosted_sdio_rxbuf_tolerant.ps1 (the RX-side twin of this one)
    already fixed. `done:` frees the caller's payload, so the drop leaks nothing.

    Field-observed on the dev bench 2026-09-11, via main/util/panic_hook.c's
    crash record:

        assert failed: sdio_write_task sdio_drv.c:433 (sendbuf)
        task: sdio_write core 1 after 1429.722 s of uptime

    That boot started with the MALLOC_CAP_DMA pool at 99 bytes; esp_hosted then
    failed to create an RPC semaphore, which orphaned a response in the 3-deep
    rpc_rx_q and deadlocked the RPC receive thread against itself (see
    apply_esp_hosted_rpc_orphan_resp.ps1 - that is the root cause, and an
    earlier version of this paragraph wrongly called it a per-failure leak).
    The un-read backlog behind that blocked thread then filled PSRAM, and this
    assert ended it.
    With the drop the device degrades instead of rebooting - and a reboot with
    the radio attached is a warm reset, i.e. the documented #74 QMX wedge.

    Edits managed_components/ (git-ignored): wiped by `idf.py fullclean`, a
    dependency refresh, or the release process's `rm -r managed_components/`.
    Re-run alongside the other esp_hosted scripts; tools/check_patches.py fails
    the build if it is missing. Idempotent.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_sdio_sendbuf_tolerant.ps1
#>

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repo "managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c"

if (-not (Test-Path $target)) {
    Write-Host "sdio_drv.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    Write-Host "(managed_components/ not fetched yet - run a build first, then re-run this.)"
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "QMX_SDIO_SENDBUF_TOLERANT") {
    Write-Host "Already patched (sdio_write_task sendbuf tolerance) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
			sendbuf = sdio_buffer_alloc(MEMSET_REQUIRED);
			assert(sendbuf);
			free_func = sdio_buffer_free;
'@

$patched = @'
			sendbuf = sdio_buffer_alloc(MEMSET_REQUIRED);
			/* QMX_SDIO_SENDBUF_TOLERANT - see tools/patches/. Stock asserted
			 * here with the drop path ("sdio buff malloc failed") right below;
			 * an exhausted pool now loses one frame instead of the device. */
			free_func = sdio_buffer_free;
'@

$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock sendbuf-assert block - sdio_drv.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched sdio_drv.c: sdio_write_task now drops a frame on a failed send-buffer allocation." -ForegroundColor Green
Write-Host "  $target"
