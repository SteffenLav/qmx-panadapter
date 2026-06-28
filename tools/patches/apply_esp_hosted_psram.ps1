<#
.SYNOPSIS
    Re-applies the QMX-Panadapter esp_hosted "transport buffers -> PSRAM" patch.

.DESCRIPTION
    managed_components/ is git-ignored and is wiped by `idf.py fullclean` or a
    dependency refresh (the release process runs `rm -r managed_components/`).
    This script re-applies the one-line patch that routes the esp_hosted WiFi
    transport DMA pool into PSRAM instead of the scarce internal DRAM.

    Without it, the per-packet TX/RX pool grows under WiFi bursts, exhausts
    internal DMA RAM under QMX+FT8 load, and panics the SDIO TX path
    (transport_drv_sta_tx -> assert(copy_buff)) -> reboot.

    Safe on ESP32-P4: SOC_SDMMC_PSRAM_DMA_CAPABLE == 1, and the 1536-byte
    transport block is 64-byte (cache-line) aligned.

    Idempotent: running it twice is a no-op. Run after every clean checkout of
    managed components, before building.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_psram.ps1
#>

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repoRoot "managed_components/espressif__esp_hosted/host/port/include/os_wrapper.h"

if (-not (Test-Path $target)) {
    Write-Host "esp_hosted not present at:" -ForegroundColor Yellow
    Write-Host "  $target"
    Write-Host "Run a build first so the IDF component manager fetches it, then re-run this."
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "extra_heap_caps = MALLOC_CAP_SPIRAM") {
    Write-Host "Already patched - nothing to do." -ForegroundColor Green
    exit 0
}

if ($content -notmatch "\.extra_heap_caps = 0,") {
    Write-Host "Could not find the expected '.extra_heap_caps = 0,' line." -ForegroundColor Red
    Write-Host "The esp_hosted version may have changed - patch by hand and update this script."
    exit 1
}

$patched = $content -replace "\.extra_heap_caps = 0,", ".extra_heap_caps = MALLOC_CAP_SPIRAM,"
# Write without a BOM so the C compiler is happy.
[System.IO.File]::WriteAllText($target, $patched, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched esp_hosted MEM_ALLOC -> PSRAM." -ForegroundColor Green
Write-Host "  $target"
