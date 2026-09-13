<#
.SYNOPSIS
    Stops esp_lvgl_port's 2 ms tick callback from taking a mutex, which let
    taskLVGL inherit priority 22 and starve every USB/audio task on core 0.

.DESCRIPTION
    Stock esp_lvgl_port (v2.5.0, src/lvgl9/esp_lvgl_port.c):

        static void lvgl_port_tick_increment(void *arg)
        {
            xSemaphoreTake(lvgl_port_ctx.timer_mux, portMAX_DELAY);
            lv_tick_inc(lvgl_port_ctx.timer_period_ms);
            xSemaphoreGive(lvgl_port_ctx.timer_mux);
        }

    That callback runs on the esp_timer task - PRIORITY 22 - every 2 ms. The
    LVGL task takes the same timer_mux around lv_indev_read() while it ALREADY
    holds lvgl_mux. When a tick lands during a touch read, esp_timer blocks on
    timer_mux and taskLVGL inherits 22. FreeRTOS only disinherits a priority
    when the holder has released EVERY mutex it holds, so taskLVGL keeps 22
    after giving timer_mux back and runs the entire lv_timer_handler() at 22 -
    above USB-CDC (10), audio_task (6) and the UAC driver (5) on core 0.

    Measured on the dev bench 2026-09-13 with a task snapshot taken the moment a
    CAT send failed: taskLVGL RUNNING at 4->22 on core 0, esp_timer READY at 22,
    USB-CDC / audio_task / USB UAC Host all READY and not running. That is the
    "opening the manual costs 7-16 CAT timeouts and 200-300 ms of lost audio"
    fault, and the timeouts seen through WSPR cycles, and the priority-22
    reading from earlier that day. Ruled out first by direct test: CPU load,
    PSRAM and internal-RAM copies, and full-screen repaints at priority 4.

    The mutex is unnecessary. lv_tick_inc() is written to be called from an
    interrupt: it clears sys_irq_flag and adds to sys_time, and lv_tick_get()
    retries until it reads without an intervening tick (lv_tick.c). So the tick
    no longer locks; the task-side take around indev reads stays and is now
    uncontended.

    ⚠ This edits managed_components/, which is git-ignored and is WIPED by
    `idf.py fullclean`, a dependency refresh, and the release process's
    `rm -r managed_components/`. Re-run it after any of those.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_lvgl_port_tick_no_mutex.ps1
#>

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repo "managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c"

if (-not (Test-Path $target)) {
    Write-Host "esp_lvgl_port.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    Write-Host "Build once so the component is fetched, then re-run."
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "PATCHED \(qmx-panadapter, 2026-09-13\) tick without mutex") {
    Write-Host "Already patched (LVGL tick without mutex) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
static void lvgl_port_tick_increment(void *arg)
{
    xSemaphoreTake(lvgl_port_ctx.timer_mux, portMAX_DELAY);
    /* Tell LVGL how many milliseconds have elapsed */
    lv_tick_inc(lvgl_port_ctx.timer_period_ms);
    xSemaphoreGive(lvgl_port_ctx.timer_mux);
}
'@

$patched = @'
static void lvgl_port_tick_increment(void *arg)
{
    /* PATCHED (qmx-panadapter, 2026-09-13) tick without mutex.
     * This runs on the esp_timer task (priority 22) every 2 ms. Taking
     * timer_mux here, while the LVGL task holds it around lv_indev_read()
     * AND holds lvgl_mux, made taskLVGL inherit 22 and keep it for the whole
     * lv_timer_handler() (FreeRTOS disinherits only when ALL held mutexes are
     * released) - starving USB-CDC, audio and the UAC driver on core 0.
     * lv_tick_inc() is interrupt-safe by design (lv_tick.c sys_irq_flag), so
     * no lock is needed. See tools/patches/apply_lvgl_port_tick_no_mutex.ps1. */
    lv_tick_inc(lvgl_port_ctx.timer_period_ms);
}
'@

$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock lvgl_port_tick_increment() - the component may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched esp_lvgl_port.c: the LVGL tick no longer takes a mutex." -ForegroundColor Green
Write-Host "  $target"
