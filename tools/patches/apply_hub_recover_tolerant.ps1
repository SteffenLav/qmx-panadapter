<#
.SYNOPSIS
    Makes the ESP-IDF USB hub driver tolerate a root-port recover/power-on
    failing with ESP_ERR_INVALID_STATE instead of abort()-rebooting.

.DESCRIPTION
    ESP-IDF v5.4.4's hub.c has, in root_port_req():

        ESP_ERROR_CHECK(hcd_port_recover(...));
        ...
        ESP_ERROR_CHECK(hcd_port_command(..., HCD_PORT_CMD_POWER_ON));

    Field-observed on the Tab5 (ESP32-P4, 2026-08-03, serial-captured):
    usb_host_lib_set_root_port_power(false/true) - used by usb_replug() to
    free a zombie device left by a failed UAC teardown (QMX powered off
    mid-stream) - can race the hub FSM: a queued PORT_REQ_RECOVER executes
    after the port has already left the RECOVERY state, hcd_port_recover()
    returns ESP_ERR_INVALID_STATE, and the ESP_ERROR_CHECK abort()s the
    whole device ("abort() was called... hub.c line 462, root_port_req").

    The patch downgrades both calls to log-and-continue, mirroring the
    driver's own precedent two lines above ("We allow this to fail" on the
    PORT_REQ_DISABLE command). If recover fails the power-on block is
    skipped - the port is somewhere else in its lifecycle and the pending
    power request that raced us handles power.

    Because this edits the pinned IDF install (NOT the project tree), it is
    wiped if the IDF is reinstalled and must be re-applied per build machine
    - same maintenance model as the other four apply_*.ps1 patches.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_hub_recover_tolerant.ps1
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

if ($content -match "PATCHED \(qmx-panadapter, 2026-08-03\)") {
    Write-Host "Already patched (root_port_req recover tolerance) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
    if (port_reqs & PORT_REQ_RECOVER) {
        ESP_LOGD(HUB_DRIVER_TAG, "Recovering root port");
        ESP_ERROR_CHECK(hcd_port_recover(p_hub_driver_obj->constant.root_port_hdl));

        // In case the port's power was turned off with usb_host_lib_set_root_port_power(false)
        // we will not turn on the power during port recovery
        HUB_DRIVER_ENTER_CRITICAL();
        const root_port_state_t root_state = p_hub_driver_obj->dynamic.root_port_state;
        HUB_DRIVER_EXIT_CRITICAL();

        if (root_state != ROOT_PORT_STATE_NOT_POWERED) {
            ESP_ERROR_CHECK(hcd_port_command(p_hub_driver_obj->constant.root_port_hdl, HCD_PORT_CMD_POWER_ON));
            HUB_DRIVER_ENTER_CRITICAL();
            p_hub_driver_obj->dynamic.root_port_state = ROOT_PORT_STATE_POWERED;
            HUB_DRIVER_EXIT_CRITICAL();
        }
    }
'@

$patched = @'
    if (port_reqs & PORT_REQ_RECOVER) {
        ESP_LOGD(HUB_DRIVER_TAG, "Recovering root port");
        // PATCHED (qmx-panadapter, 2026-08-03): a root-port power cycle via
        // usb_host_lib_set_root_port_power() can race a queued
        // PORT_REQ_RECOVER so that the port has already left the RECOVERY
        // state by the time this runs; hcd_port_recover() then returns
        // ESP_ERR_INVALID_STATE, which stock code feeds into
        // ESP_ERROR_CHECK and abort()s the whole device. Allow it to fail
        // like the PORT_REQ_DISABLE command above - the racing power
        // request handles the port from here.
        esp_err_t recover_err = hcd_port_recover(p_hub_driver_obj->constant.root_port_hdl);
        if (recover_err != ESP_OK) {
            ESP_LOGW(HUB_DRIVER_TAG, "Root port recover skipped: %s", esp_err_to_name(recover_err));
        } else {

        // In case the port's power was turned off with usb_host_lib_set_root_port_power(false)
        // we will not turn on the power during port recovery
        HUB_DRIVER_ENTER_CRITICAL();
        const root_port_state_t root_state = p_hub_driver_obj->dynamic.root_port_state;
        HUB_DRIVER_EXIT_CRITICAL();

        if (root_state != ROOT_PORT_STATE_NOT_POWERED) {
            // PATCHED (qmx-panadapter, 2026-08-03): same tolerance for the
            // power-on - it can race a concurrent power request the same way.
            esp_err_t pwr_err = hcd_port_command(p_hub_driver_obj->constant.root_port_hdl, HCD_PORT_CMD_POWER_ON);
            if (pwr_err != ESP_OK) {
                ESP_LOGW(HUB_DRIVER_TAG, "Root port power-on skipped: %s", esp_err_to_name(pwr_err));
            } else {
            HUB_DRIVER_ENTER_CRITICAL();
            p_hub_driver_obj->dynamic.root_port_state = ROOT_PORT_STATE_POWERED;
            HUB_DRIVER_EXIT_CRITICAL();
            }
        }
        }
    }
'@

# Normalize line endings for the match (the IDF tree uses LF)
$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock root_port_req block - hub.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched hub.c: root_port_req recover/power-on tolerance (no more abort-reboot)." -ForegroundColor Green
Write-Host "  $target"
