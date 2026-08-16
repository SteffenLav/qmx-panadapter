<#
.SYNOPSIS
    Stops cdc_acm_host_close() abort()-rebooting the device when an endpoint
    still has a URB in flight. Retries the interface release first, then logs
    and continues instead of asserting.

.DESCRIPTION
    Stock cdc_acm_host.c (v2.x) ends cdc_acm_host_close() with:

        ESP_ERROR_CHECK(usb_host_interface_release(..., data.intf_desc->bInterfaceNumber));
        ...
        ESP_ERROR_CHECK(usb_host_interface_release(..., notif.intf_desc->bInterfaceNumber));

    usb_host_interface_release() returns ESP_ERR_INVALID_STATE if ANY endpoint
    of the interface still has num_urb_inflight != 0 or is on the pending list.
    It gives the client task exactly vTaskDelay(10) to reap flushed URBs - and
    at this project's CONFIG_FREERTOS_HZ=1000 that is 10 MILLISECONDS, not the
    100 ms the vendor comment reads like. So a busy port turns a transient into
    abort(), i.e. a full device reboot.

    Serial-captured on the Tab5 while adding the QMX terminal (#147,
    2026-08-16): closing a session on the radio's second CDC interface aborted
    at cdc_acm_host.c:717 with 0x103 ESP_ERR_INVALID_STATE. Timing tweaks on our
    side (a non-blocking RX callback, waiting for the port to go quiet) made it
    rarer - three clean closes then an abort on the fourth - which is exactly
    the shape of a race rather than a bug that timing can fix.

    Worse, the reboot is a Tab5 warm reset with the QMX attached, which is the
    documented #74 trigger, so every abort also wedged the radio until a power
    cycle.

    This patch does two things, in order of preference:
      1. RETRIES the release (16 x 20 ms) - this addresses the actual cause,
         which is that IDF's 10 ms allowance is too short when the client task
         is busy. Nearly always succeeds on a later attempt.
      2. Only if it still fails, logs a warning and carries on, so the device
         object is always freed and the firmware never dies. The interface claim
         is leaked in that case - a leak is strictly better than a reboot, and
         it mirrors the precedent already set by this project's UAC fork
         ("forced teardown on a dead device", see CLAUDE.md).

    The same treatment is applied to the two endpoint resets above it, which
    fail for the same reason on a device that has already gone away.

    ⚠ This edits managed_components/, which is git-ignored and is WIPED by
    `idf.py fullclean`, a dependency refresh, and the release process's
    `rm -r managed_components/`. Re-run it after any of those, like the other
    apply_*.ps1 patches.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_cdc_acm_close_tolerant.ps1
#>

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repo "managed_components/espressif__usb_host_cdc_acm/cdc_acm_host.c"

if (-not (Test-Path $target)) {
    Write-Host "cdc_acm_host.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    Write-Host "Build once so the component is fetched, then re-run."
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "PATCHED \(qmx-panadapter, 2026-08-16\)") {
    Write-Host "Already patched (cdc_acm_host_close tolerance) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
    // Cancel polling of BULK IN and INTERRUPT IN
    if (cdc_dev->data.in_xfer) {
        ESP_ERROR_CHECK(cdc_acm_reset_transfer_endpoint(cdc_dev->dev_hdl, cdc_dev->data.in_xfer));
    }
    if (cdc_dev->notif.xfer != NULL) {
        ESP_ERROR_CHECK(cdc_acm_reset_transfer_endpoint(cdc_dev->dev_hdl, cdc_dev->notif.xfer));
    }

    // Release all interfaces
    ESP_ERROR_CHECK(usb_host_interface_release(p_cdc_acm_obj->cdc_acm_client_hdl, cdc_dev->dev_hdl, cdc_dev->data.intf_desc->bInterfaceNumber));
    if ((cdc_dev->notif.intf_desc != NULL) && (cdc_dev->notif.intf_desc != cdc_dev->data.intf_desc)) {
        ESP_ERROR_CHECK(usb_host_interface_release(p_cdc_acm_obj->cdc_acm_client_hdl, cdc_dev->dev_hdl, cdc_dev->notif.intf_desc->bInterfaceNumber));
    }
'@

$patched = @'
    // PATCHED (qmx-panadapter, 2026-08-16): none of these may abort().
    // usb_host_interface_release() returns ESP_ERR_INVALID_STATE while any
    // endpoint still has a URB in flight, and it allows the client task only
    // vTaskDelay(10) - 10 ms at FREERTOS_HZ=1000 - to reap them. Feeding that
    // into ESP_ERROR_CHECK turns a transient into a device reboot. Retry first
    // (that is the real fix - the window is simply too short), then log and
    // carry on so the device object is always freed.
    if (cdc_dev->data.in_xfer) {
        esp_err_t e = cdc_acm_reset_transfer_endpoint(cdc_dev->dev_hdl, cdc_dev->data.in_xfer);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "close: IN endpoint reset failed (%s) - continuing", esp_err_to_name(e));
        }
    }
    if (cdc_dev->notif.xfer != NULL) {
        esp_err_t e = cdc_acm_reset_transfer_endpoint(cdc_dev->dev_hdl, cdc_dev->notif.xfer);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "close: notif endpoint reset failed (%s) - continuing", esp_err_to_name(e));
        }
    }

    // Release all interfaces
    for (int attempt = 0; attempt < 16; attempt++) {
        esp_err_t e = usb_host_interface_release(p_cdc_acm_obj->cdc_acm_client_hdl, cdc_dev->dev_hdl, cdc_dev->data.intf_desc->bInterfaceNumber);
        if (e == ESP_OK) {
            break;
        }
        if (attempt == 15) {
            ESP_LOGW(TAG, "close: data interface %d would not release (%s) - leaking the claim rather than aborting",
                     cdc_dev->data.intf_desc->bInterfaceNumber, esp_err_to_name(e));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if ((cdc_dev->notif.intf_desc != NULL) && (cdc_dev->notif.intf_desc != cdc_dev->data.intf_desc)) {
        for (int attempt = 0; attempt < 16; attempt++) {
            esp_err_t e = usb_host_interface_release(p_cdc_acm_obj->cdc_acm_client_hdl, cdc_dev->dev_hdl, cdc_dev->notif.intf_desc->bInterfaceNumber);
            if (e == ESP_OK) {
                break;
            }
            if (attempt == 15) {
                ESP_LOGW(TAG, "close: notif interface %d would not release (%s) - leaking the claim rather than aborting",
                         cdc_dev->notif.intf_desc->bInterfaceNumber, esp_err_to_name(e));
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
'@

# Normalize line endings for the match (the component tree uses LF)
$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock cdc_acm_host_close() block - the component may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched cdc_acm_host.c: close() retries the interface release and never aborts." -ForegroundColor Green
Write-Host "  $target"
