# Re-apply the lv_event chain guard (#329) to managed_components/.
#
# managed_components/ is git-ignored and is wiped by `idf.py fullclean`, a
# dependency refresh, and the release process's `rm -r managed_components/` -
# so this restores the patched file the same store/restore way as
# apply_esp_hosted_sdio_recovery.ps1. Idempotent, marker-guarded.
#
# WHAT IT DOES: lv_event_mark_deleted() walks the in-flight event chain on
# every object destruction. Twice on 2026-09-06 that walk read a link that was
# not a pointer (0x6a9d5caa, then 0x0000ffff) and the device took a Load access
# fault on taskLVGL. The guard stops walking and counts instead of dying.
# Diagnostic, not a cure - it comes out when the cause is understood.
$ErrorActionPreference = 'Stop'

$target = 'C:/dev/qmx-panadapter/managed_components/lvgl__lvgl/src/misc/lv_event.c'
$stored = Join-Path $PSScriptRoot 'lv_event.c.patched'
$marker = 'qmx_lv_event_chain_bad'

if (-not (Test-Path $target)) {
    Write-Host "lv_event.c not found - run a build first so the component is fetched." -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path $stored)) {
    Write-Host "stored copy missing: $stored" -ForegroundColor Red
    exit 1
}
if ((Get-Content $target -Raw) -match $marker) {
    Write-Host "lv_event chain guard: already patched." -ForegroundColor Green
    exit 0
}
Copy-Item $stored $target -Force
Write-Host "lv_event chain guard: applied." -ForegroundColor Green
