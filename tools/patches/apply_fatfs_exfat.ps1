<#
.SYNOPSIS
    Enables exFAT support in the ESP-IDF FatFs component.

.DESCRIPTION
    ESP-IDF v5.4.4 ships FatFs with exFAT compiled OFF (`#define FF_FS_EXFAT 0`
    in components/fatfs/src/ffconf.h) and exposes NO Kconfig option to flip it.
    Large microSD cards (>32 GB) are exFAT by default and won't mount without
    it, so the SD auto-archive (storage/sd_archive.c) can't write to them.

    This script flips FF_FS_EXFAT 0 -> 1 in the *IDF install tree* so FatFs
    auto-detects and mounts exFAT (as well as FAT/FAT32) cards. exFAT also
    requires long filenames, which are enabled in this project's sdkconfig
    (CONFIG_FATFS_LFN_HEAP=y) - the two go together.

    Because this edits the pinned IDF install (NOT the project tree), it is
    wiped if the IDF is reinstalled and must be re-applied per build machine -
    same maintenance model as tools/patches/apply_esp_hosted_psram.ps1. There
    is no compile guard tying exFAT to LFN, so the change is harmless to other
    projects built against this IDF (it only adds exFAT code; FAT still works).

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_fatfs_exfat.ps1
#>

$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    Write-Host "IDF_PATH not set - run this from an activated ESP-IDF environment." -ForegroundColor Red
    exit 1
}

$target = Join-Path $env:IDF_PATH "components/fatfs/src/ffconf.h"
if (-not (Test-Path $target)) {
    Write-Host "fatfs ffconf.h not found at:" -ForegroundColor Red
    Write-Host "  $target"
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "(?m)^\s*#define\s+FF_FS_EXFAT\s+1\b") {
    Write-Host "Already patched (FF_FS_EXFAT = 1) - nothing to do." -ForegroundColor Green
    exit 0
}

if ($content -notmatch "(?m)^\s*#define\s+FF_FS_EXFAT\s+0\b") {
    Write-Host "Could not find '#define FF_FS_EXFAT 0' - FatFs may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patched = $content -replace "(?m)^(\s*#define\s+FF_FS_EXFAT\s+)0\b", '${1}1'
[System.IO.File]::WriteAllText($target, $patched, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched FatFs: FF_FS_EXFAT -> 1 (exFAT enabled)." -ForegroundColor Green
Write-Host "  $target"
