# Standing patch - newlib's port, components/newlib/locks.c, lock_init_generic().
#
# WHY
# ---
# Every task's FIRST use of a given C-library facility that needs a lock
# (malloc's own arena lock, stdio, etc.) lazily creates the mutex here, and
# on failure the stock code aborts unconditionally: `if (!new_sem) abort();`
# - "No more semaphores available or OOM". CLAUDE.md already documents one
# occurrence of this exact class (#317, root-caused to leftover R&D code
# that called fopen() 140x/slot) but its own general diagnosis stands:
# "ANY task's first [lock-needing call] while internal heap is near zero
# can abort the device - there is nothing special about [any one caller]."
#
# Reproduced twice more on hardware 2026-09-20, DIFFERENT tasks each time -
# `diag_persist` and `spots` - both aborting here during the same boot-time
# internal-RAM trough this board has extensive history with. Neither task
# does anything unusual; they just happened to be the ones taking their
# first lock-needing call at the wrong moment.
#
# WHAT THIS DOES
# --------------
# Restructures the single attempt into a bounded retry loop: on failure,
# leave the critical section (a mutex allocation can take tens of ms even
# when it succeeds, and this board's own measurements put the boot trough
# at tens of ms to a few seconds - never hold a spinlock across a wait),
# pause briefly, and try again - re-checking *lock first each time, in case
# a racing thread initialised it while this one was waiting. Still aborts
# after 5 attempts (~100 ms), preserving the exact original behaviour for
# genuine, sustained exhaustion - this narrows the transient boot-time
# window that caused both captured crashes, it does not claim to make
# exhaustion impossible.
#
# Edits the pinned IDF tree, so an IDF reinstall wipes it - per-build-machine,
# like the other IDF-tree patches. Idempotent, marker-guarded.

$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) { Write-Error "IDF_PATH not set - run this from an activated ESP-IDF environment."; exit 1 }

$target = Join-Path $env:IDF_PATH "components/newlib/locks.c"
if (-not (Test-Path $target)) { Write-Error "not found: $target"; exit 1 }

$src = (Get-Content -Raw -Encoding UTF8 $target) -replace "`r`n", "`n"
$marker = "QMX_NEWLIB_LOCK_INIT_RETRY"
if ($src -match $marker) {
    Write-Host "newlib lock-init retry: already patched." -ForegroundColor Green
    exit 0
}

$old = @'
static void IRAM_ATTR lock_init_generic(_lock_t *lock, uint8_t mutex_type)
{
    portENTER_CRITICAL(&lock_init_spinlock);
    if (*lock) {
        /* Lock already initialised (either we didn't check earlier,
         or it got initialised while we were waiting for the
         spinlock.) */
    } else {
        /* Create a new semaphore

           this is a bit of an API violation, as we're calling the
           private function xQueueCreateMutex(x) directly instead of
           the xSemaphoreCreateMutex / xSemaphoreCreateRecursiveMutex
           wrapper functions...

           The better alternative would be to pass pointers to one of
           the two xSemaphoreCreate___Mutex functions, but as FreeRTOS
           implements these as macros instead of inline functions
           (*party like it's 1998!*) it's not possible to do this
           without writing wrappers. Doing it this way seems much less
           spaghetti-like.
        */
        SemaphoreHandle_t new_sem = xQueueCreateMutex(mutex_type);
        if (!new_sem) {
            abort(); /* No more semaphores available or OOM */
        }
        *lock = (_lock_t)new_sem;
    }
    portEXIT_CRITICAL(&lock_init_spinlock);
}
'@ -replace "`r`n", "`n"

$new = @'
static void IRAM_ATTR lock_init_generic(_lock_t *lock, uint8_t mutex_type)
{
    /* QMX_NEWLIB_LOCK_INIT_RETRY - see tools/patches/. Stock made exactly one
     * attempt and abort()ed on failure - reproduced twice on hardware in a
     * single boot-time memory trough, two different tasks, neither doing
     * anything unusual. Never sleep while holding the spinlock: leave it,
     * pause, re-enter and re-check *lock (a racing thread may have won)
     * before trying again. */
    for (int qmx_try = 0; ; qmx_try++) {
        portENTER_CRITICAL(&lock_init_spinlock);
        if (*lock) {
            /* Lock already initialised (either we didn't check earlier,
             or it got initialised while we were waiting for the
             spinlock.) */
            portEXIT_CRITICAL(&lock_init_spinlock);
            return;
        }
        /* Create a new semaphore

           this is a bit of an API violation, as we're calling the
           private function xQueueCreateMutex(x) directly instead of
           the xSemaphoreCreateMutex / xSemaphoreCreateRecursiveMutex
           wrapper functions...

           The better alternative would be to pass pointers to one of
           the two xSemaphoreCreate___Mutex functions, but as FreeRTOS
           implements these as macros instead of inline functions
           (*party like it's 1998!*) it's not possible to do this
           without writing wrappers. Doing it this way seems much less
           spaghetti-like.
        */
        SemaphoreHandle_t new_sem = xQueueCreateMutex(mutex_type);
        if (new_sem) {
            *lock = (_lock_t)new_sem;
            portEXIT_CRITICAL(&lock_init_spinlock);
            return;
        }
        portEXIT_CRITICAL(&lock_init_spinlock);
        if (qmx_try >= 4) {
            abort(); /* No more semaphores available or OOM, after retrying */
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
'@ -replace "`r`n", "`n"

if ($src -notmatch [regex]::Escape($old)) { Write-Error "anchor not found - upstream changed, re-check by hand"; exit 1 }
$src = $src.Replace($old, $new)

[System.IO.File]::WriteAllText($target, $src, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Patched locks.c: lock_init_generic() retries before giving up." -ForegroundColor Green
Write-Host "  $target"
