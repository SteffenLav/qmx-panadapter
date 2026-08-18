<#
.SYNOPSIS
    Stops the ESP-IDF USB HAL asserting when a channel error interrupt arrives
    without the channel-halted bit, which reboots the whole device.

.DESCRIPTION
    ESP-IDF v5.4.4's usb_dwc_hal.c has, in usb_dwc_hal_chan_decode_intr():

        if (chan_intrs & CHAN_INTRS_ERROR_MSK) {
            HAL_ASSERT(chan_intrs & USB_DWC_LL_INTR_CHAN_CHHLTD);  //An error should have halted the channel

    That comment is an ASSUMPTION, and this hardware violates it. Field-observed
    on the Tab5 (ESP32-P4 v1.3, 2026-08-18, serial-captured during an overnight
    soak of v1.8.5): at 1 h 57 m of an otherwise perfectly healthy FT8 session -
    radio streaming 46,806 pairs/s, FT8 mid-capture, 55 KB internal free - a
    channel error interrupt arrived WITHOUT CHHLTD and the assert aborted the
    device:

        assert failed: usb_dwc_hal_chan_decode_intr usb_dwc_hal.c:504
                       (chan_intrs & USB_DWC_LL_INTR_CHAN_CHHLTD)

    The consequence was worse than one reboot. The abort is a warm reset with the
    QMX still attached, which is the documented TODO #74 trigger, so the radio
    then failed to re-enumerate and stayed dead for the remaining 5 h 26 m of the
    night (4 ENUM failures, then "RX 0 pairs/s" until morning). One assert cost
    the whole session.

    THE FIX IS ALREADY WRITTEN, immediately below the assert: the same block
    classifies the error (STALL / BBLEER / BNA / XCS_XACT), sets
    chan_obj->flags.active = 0 and returns USB_DWC_HAL_CHAN_EVENT_ERROR, which
    the HCD handles - and standing patch #4 (hcd_dwc.c) already made the layer
    above tolerant of exactly that. So dropping the assert does not invent a
    recovery path; it lets the existing one run.

    Deliberately NO logging added: this executes in the USB interrupt path, and
    a long or blocking call there risks the MIPI-DSI frame-restart stall that
    CLAUDE.md records as the "cyan flash".

    This is the THIRD assert of the same family in IDF's USB stack - see also
    apply_hcd_bulk_error_recovery.ps1 (#4) and apply_hub_recover_tolerant.ps1
    (#5). The pattern: IDF asserts on hardware states it treats as impossible,
    and on this board they happen.

    Because this edits the pinned IDF install (NOT the project tree), it is
    wiped if the IDF is reinstalled and must be re-applied per build machine.
    tools/check_patches.py fails the build if it is missing.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_usb_dwc_hal_chan_error_tolerant.ps1
#>

$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    Write-Host "IDF_PATH not set - run this from an activated ESP-IDF environment." -ForegroundColor Red
    exit 1
}

$target = Join-Path $env:IDF_PATH "components/hal/usb_dwc_hal.c"
if (-not (Test-Path $target)) {
    Write-Host "usb_dwc_hal.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "PATCHED \(qmx-panadapter, 2026-08-18\)") {
    Write-Host "Already patched (chan_decode_intr error tolerance) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
    if (chan_intrs & CHAN_INTRS_ERROR_MSK) {    //Note: Errors are uncommon, so we check against the entire interrupt mask to reduce frequency of entering this call path
        HAL_ASSERT(chan_intrs & USB_DWC_LL_INTR_CHAN_CHHLTD);  //An error should have halted the channel
'@

$patched = @'
    if (chan_intrs & CHAN_INTRS_ERROR_MSK) {    //Note: Errors are uncommon, so we check against the entire interrupt mask to reduce frequency of entering this call path
        // PATCHED (qmx-panadapter, 2026-08-18): stock code has
        //     HAL_ASSERT(chan_intrs & USB_DWC_LL_INTR_CHAN_CHHLTD);
        // on the premise that "an error should have halted the channel". That is
        // an assumption, and ESP32-P4 v1.3 violates it: field-observed at 1h57m
        // of a healthy FT8 session, an error interrupt arrived with no CHHLTD and
        // the assert rebooted the device - which, being a warm reset with the QMX
        // attached, then left the radio unable to re-enumerate for 5+ hours
        // (TODO #74/#182). Reporting the error is strictly better than aborting:
        // the code below already classifies it, clears flags.active and returns
        // CHAN_EVENT_ERROR, and the HCD above handles that (see standing patch
        // #4). No logging here - this runs in the USB interrupt path.
'@

# Normalize line endings for the match (the IDF tree uses LF)
$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock chan_decode_intr error block - usb_dwc_hal.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched usb_dwc_hal.c: a channel error without CHHLTD is reported, not asserted." -ForegroundColor Green
Write-Host "  $target"
