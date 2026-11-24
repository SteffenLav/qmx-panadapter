<#
.SYNOPSIS
    Makes the ESP-IDF USB hub driver tolerate ESP_ERR_NOT_FOUND when the
    root port reports a device gone that is no longer in the device tree
    (T182), instead of abort()-rebooting.

.DESCRIPTION
    ESP-IDF v5.4.4's hub.c, root_port_handle_events(), on a disconnect:

        if (port_has_device) {
            ESP_ERROR_CHECK(dev_tree_node_dev_gone(NULL, 0));
        }

    The node can already be gone through the recycle path
    (USBH_EVENT_DEV_FREE -> hub_node_recycle) when the device was freed
    before the disconnect event reached the hub driver. The call then
    returns ESP_ERR_NOT_FOUND and the check aborts the Tab5 (cyan/blue
    screen, task usb_lib).

    Seen twice: 2026-11-10 (T182, full message hub.c:435, QMX power-cycled
    during a usb_replug retry) and 2026-11-24 on a QDX test build (a wedged
    QDX plugged in, unplugged, power-cycled, plugged in again). The second
    record holds only _esp_error_check_failed in usb_lib, so it is
    consistent with hub.c:435, not proven to be it.

    The fix is upstream's own: espressif/esp-usb host/usb/src/hub.c treats
    ESP_ERR_NOT_FOUND as benign at this call and keeps the check for every
    other error. Copied here, not invented.

    Edits the pinned IDF install (NOT the project tree), same maintenance
    model as apply_hub_recover_tolerant.ps1. Idempotent.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_hub_root_gone_tolerant.ps1
#>

$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    Write-Host "IDF_PATH not set - run this from an activated ESP-IDF environment." -ForegroundColor Red
    exit 1
}

$target = Join-Path $env:IDF_PATH "components/usb/hub.c"
if (-not (Test-Path $target)) {
    Write-Host "hub.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "PATCHED \(qmx-panadapter, T182\)") {
    Write-Host "Already patched (root port dev_gone NOT_FOUND tolerance) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
        if (port_has_device) {
            ESP_ERROR_CHECK(dev_tree_node_dev_gone(NULL, 0));
        }
'@

$patched = @'
        if (port_has_device) {
            // PATCHED (qmx-panadapter, T182): upstream esp-usb's own fix. The
            // dev tree node may already have been removed via the recycle path
            // (USBH_EVENT_DEV_FREE -> hub_node_recycle) if the device was freed
            // before this disconnect event was processed. Nothing more to
            // report then, so ESP_ERR_NOT_FOUND is benign; any other error
            // still aborts.
            const esp_err_t gone_ret = dev_tree_node_dev_gone(NULL, 0);
            if (gone_ret != ESP_OK && gone_ret != ESP_ERR_NOT_FOUND) {
                ESP_ERROR_CHECK(gone_ret);
            }
        }
'@

$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock dev_tree_node_dev_gone(NULL, 0) block - hub.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched hub.c: root port dev_gone NOT_FOUND tolerance (T182)." -ForegroundColor Green
Write-Host "  $target"
