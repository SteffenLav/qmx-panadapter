# Make esp_hosted honour its own task-stack Kconfig values.
#
# WHY: upstream hardcodes 5 KB in os_wrapper.h and ignores
# CONFIG_ESP_HOSTED_RPC_TASK_STACK / CONFIG_ESP_HOSTED_DFLT_TASK_STACK. Six
# tasks are created with those sizes and their stacks come out of
# MALLOC_CAP_DMA - the scarce pool on this board.
#
# The rx_audio track's I2S channel needs ~15.4 KB of that same pool. Without
# this, the pool floor went from 5223 B to 59 B and esp_hosted's SDIO transport
# could not get RX buffers: that is the 2026-09-04 crash, which was
# misattributed to the platform for two days.
#
# 4096, not the Kconfig default of 3072: measured high-water marks put sdio_read
# at ~2716 B used and rpc_rx at ~2728 B.
#
# ⚠ managed_components/ is git-ignored and is wiped by `idf.py fullclean`, a
# dependency refresh and the release process - re-run this after any of those,
# alongside the other esp_hosted patches. Idempotent, marker-guarded.
$ErrorActionPreference = 'Stop'

$target = Join-Path $PSScriptRoot '..\..\managed_components\espressif__esp_hosted\host\port\include\os_wrapper.h'
$marker = 'QMX_HOSTED_TASK_STACKS_FROM_KCONFIG'

if (-not (Test-Path $target)) {
    Write-Host "os_wrapper.h not found - run a build first so the component is fetched." -ForegroundColor Yellow
    exit 1
}
if ((Get-Content $target -Raw) -match $marker) {
    Write-Host "esp_hosted task stacks: already patched." -ForegroundColor Green
    exit 0
}
# ⚠ The PSRAM patch edits this same file. Restoring a stored copy would discard
# it, so apply this one as an EDIT and let its own script run independently.
$txt = Get-Content $target -Raw
$txt = $txt -replace '(?m)^#define RPC_TASK_STACK_SIZE\s+\(5\*1024\)$', @'
#ifdef CONFIG_ESP_HOSTED_RPC_TASK_STACK   /* QMX_HOSTED_TASK_STACKS_FROM_KCONFIG */
#define RPC_TASK_STACK_SIZE                          CONFIG_ESP_HOSTED_RPC_TASK_STACK
#else
#define RPC_TASK_STACK_SIZE                          (5*1024)
#endif
'@
$txt = $txt -replace '(?m)^#define DFLT_TASK_STACK_SIZE\s+\(5\*1024\)$', @'
#ifdef CONFIG_ESP_HOSTED_DFLT_TASK_STACK
#define DFLT_TASK_STACK_SIZE                         CONFIG_ESP_HOSTED_DFLT_TASK_STACK
#else
#define DFLT_TASK_STACK_SIZE                         (5*1024)
#endif
'@
if ($txt -notmatch $marker) { Write-Host "pattern not found - upstream changed?" -ForegroundColor Red; exit 1 }
Set-Content -Path $target -Value $txt -NoNewline -Encoding utf8
Write-Host "esp_hosted task stacks: applied (RPC/DFLT now follow sdkconfig)." -ForegroundColor Green
