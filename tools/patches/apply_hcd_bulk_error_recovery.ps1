<#
.SYNOPSIS
    Makes the ESP-IDF USB host driver tolerate a bulk transfer completing
    with an error status instead of assert-panicking the whole device.

.DESCRIPTION
    ESP-IDF v5.4.4's hcd_dwc.c has, in _buffer_parse_bulk():

        assert(desc_status == USB_DWC_HAL_XFER_DESC_STS_SUCCESS);

    Field-observed on the Tab5 (ESP32-P4 v1.3, 2026-07-16, serial-captured):
    a bulk transfer on the CDC-ACM (QMX CAT) pipe completed with a
    non-SUCCESS descriptor status while UAC audio streamed on the same host
    - a transient bus/transaction error - and the assert rebooted the device
    mid-FT8-session ("assert failed: _buffer_parse_bulk hcd_dwc.c:2406").

    The patch replaces the assert with a graceful failure: the URB is
    reported as USB_TRANSFER_STATUS_ERROR with 0 bytes (identical semantics
    to the driver's own _buffer_parse_error() path) and the class driver /
    application retries - cat.c's poll task already tolerates ~20
    consecutive transient failures.

    Because this edits the pinned IDF install (NOT the project tree), it is
    wiped if the IDF is reinstalled and must be re-applied per build machine
    - same maintenance model as apply_fatfs_exfat.ps1 /
    apply_esp_hosted_psram.ps1 / apply_esp_hosted_sdio_recovery.ps1.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_hcd_bulk_error_recovery.ps1
#>

$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    Write-Host "IDF_PATH not set - run this from an activated ESP-IDF environment." -ForegroundColor Red
    exit 1
}

$target = Join-Path $env:IDF_PATH "components/usb/hcd_dwc.c"
if (-not (Test-Path $target)) {
    Write-Host "hcd_dwc.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "PATCHED \(qmx-panadapter, 2026-07-16\)") {
    Write-Host "Already patched (_buffer_parse_bulk error recovery) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
    usb_dwc_hal_xfer_desc_parse(buffer->xfer_desc_list, 0, &rem_len, &desc_status);
    assert(desc_status == USB_DWC_HAL_XFER_DESC_STS_SUCCESS);
    assert(rem_len <= transfer->num_bytes);
    transfer->actual_num_bytes = transfer->num_bytes - rem_len;
'@

$patched = @'
    usb_dwc_hal_xfer_desc_parse(buffer->xfer_desc_list, 0, &rem_len, &desc_status);
    // PATCHED (qmx-panadapter, 2026-07-16): the pipe event said URB_DONE but
    // the descriptor carries a non-SUCCESS status (field-observed on ESP32-P4
    // v1.3: a transient bulk transaction error on a CDC-ACM pipe while the
    // host ran UAC + CDC concurrently). Stock code assert()s here, panicking
    // the whole device. Report a failed transfer instead - same semantics as
    // _buffer_parse_error() - and let the class driver/application retry.
    // No logging here: this can run inside the HCD's critical section.
    if (desc_status != USB_DWC_HAL_XFER_DESC_STS_SUCCESS ||
        rem_len > transfer->num_bytes) {
        transfer->actual_num_bytes = 0;
        transfer->status = USB_TRANSFER_STATUS_ERROR;
        memset(buffer->xfer_desc_list, 0, XFER_LIST_LEN_BULK * sizeof(usb_dwc_ll_dma_qtd_t));
        return;
    }
    transfer->actual_num_bytes = transfer->num_bytes - rem_len;
'@

# Normalize line endings for the match (the IDF tree uses LF)
$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock _buffer_parse_bulk block - hcd_dwc.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched hcd_dwc.c: _buffer_parse_bulk bulk-error recovery (no more assert-reboot)." -ForegroundColor Green
Write-Host "  $target"
