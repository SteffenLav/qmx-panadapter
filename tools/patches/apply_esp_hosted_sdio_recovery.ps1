<#
.SYNOPSIS
    Re-applies the QMX-Panadapter esp_hosted SDIO "oversize-length recovery" patch.

.DESCRIPTION
    managed_components/ is git-ignored and is wiped by `idf.py fullclean` or a
    dependency refresh (the release process runs `rm -r managed_components/`).
    This script re-applies the patch to esp_hosted's SDIO host driver that stops
    a permanent WiFi wedge.

    THE BUG: in RX_NONE mode (CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_NONE=y) the
    host RX task computes a rolling "pending bytes" delta from the slave (C6). If
    that delta ever exceeds ESP_RX_BUFFER_SIZE (1536) -- which happens
    intermittently under WiFi load, and on long idle uptimes -- the stock driver
    logged "H_SDIO_DRV: Len from slave[N] exceeds max [1536]", returned ESP_FAIL,
    and the RX task skipped the interrupt WITHOUT reading the bytes or advancing
    its byte counter. The identical oversized delta then recurred on every
    subsequent interrupt forever -> the SDIO link livelocked -> every RPC to the
    C6 (e.g. the 1 Hz WifiStaGetApInfo 0x126 poll) timed out permanently. WiFi
    "died" with FT8/CAT unaffected; only a reboot recovered it.

    THE FIX (in sdio_drv.c): remove the return-ESP_FAIL guard from
    sdio_get_len_from_slave() (it now always returns the raw delta), and have the
    RX task, on an oversized delta, DRAIN+DISCARD the pending bytes in
    <=ESP_RX_BUFFER_SIZE block-padded chunks (identical CMD53 addressing to the
    normal read) and advance sdio_rx_byte_count so the delta re-aligns. One frame
    is lost (TCP/UDP retransmits) but the link survives. A diagnostic
    "SDIO RX oversize: len=.. host_cnt=.. slave_reg=.." line is logged each time.
    Verified on hardware: the recovery fired and WiFi stayed fully up (no 0x126).

    This ships as a byte-exact copy of the patched file (the patch is multi-hunk;
    a store/restore is far more reliable than reconstructing string edits). A
    version guard refuses to clobber anything that isn't the known-pristine
    esp_hosted 1.4.0 sdio_drv.c.

    Idempotent: running it twice is a no-op. Run after every clean fetch of the
    managed components, before building.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_sdio_recovery.ps1
#>

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repoRoot "managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c"
$stored = Join-Path $PSScriptRoot "esp_hosted_sdio_drv.c.patched"

if (-not (Test-Path $target)) {
    Write-Host "esp_hosted not present at:" -ForegroundColor Yellow
    Write-Host "  $target"
    Write-Host "Run a build first so the IDF component manager fetches it, then re-run this."
    exit 1
}
if (-not (Test-Path $stored)) {
    Write-Host "Stored patched copy missing:" -ForegroundColor Red
    Write-Host "  $stored"
    exit 1
}

$content = Get-Content -Raw -Path $target

# Already patched? (marker is our unique recovery log string.)
if ($content -match "SDIO RX oversize") {
    Write-Host "Already patched - nothing to do." -ForegroundColor Green
    exit 0
}

# Version guard: only restore over the known-pristine esp_hosted 1.4.0 file,
# identified by the exact "exceeds max" log string the fix removes. If it's
# absent, the esp_hosted version has changed - do NOT clobber; re-port by hand.
if ($content -notmatch "Len from slave\[%ld\] exceeds max") {
    Write-Host "Target sdio_drv.c is not the expected pristine version." -ForegroundColor Red
    Write-Host "esp_hosted may have been updated - re-port the SDIO oversize-recovery"
    Write-Host "patch by hand and refresh tools/patches/esp_hosted_sdio_drv.c.patched."
    exit 1
}

Copy-Item -Path $stored -Destination $target -Force

Write-Host "Patched esp_hosted SDIO driver (oversize-length recovery)." -ForegroundColor Green
Write-Host "  $target"
