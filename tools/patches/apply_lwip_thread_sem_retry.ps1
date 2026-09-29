# Standing patch - LWIP's port/freertos/sys_arch.c, sys_thread_sem_init().
#
# WHY
# ---
# Every task's FIRST blocking netconn/socket call lazily allocates a
# per-thread semaphore via sys_thread_sem_init(). On failure it correctly
# returns NULL (mem_malloc or xSemaphoreCreateBinary came back empty) and
# logs "thread_sem_init: out of memory" - but LWIP's own netconn API
# (api_lib.c, reached through the LWIP_NETCONN_THREAD_SEM_GET() macro from
# many call sites) does not check that return value before dereferencing
# it. The result is a Load access fault, not a clean error path.
#
# Captured on hardware 2026-09-20: task `tiT`, 15.9 s of uptime -
# `E lwip_arch: thread_sem_init: out of memory` immediately followed by
# `Guru Meditation Error: Core 0 panic'ed (Load access fault)`, A0=0x0 (a
# NULL first argument). Squarely inside the same boot-time internal-RAM
# trough this board already has extensive history with (BLE's assert,
# OTA's silently-failed task, esp-mqtt's unchecked event-loop-create).
#
# WHAT THIS DOES
# --------------
# Retries both allocations a few times with a short pause before giving up -
# NOT to guarantee success (every caller upstream of this would still need
# auditing for that), but because the failure observed is a TRANSIENT boot
# trough, not a sustained one: this board's own measurements elsewhere
# tonight show internal free recovering within tens of milliseconds to a
# few seconds during this exact window. A short bounded retry costs nothing
# once the trough has passed (both allocations succeed first try) and turns
# most of the transient-failure window into a non-event instead of a crash.
# Still returns 0/logs on genuine, sustained exhaustion - the graceful path
# this function already had is unchanged, just given a fairer chance first.
#
# Edits the pinned IDF tree, so an IDF reinstall wipes it - per-build-machine,
# like the other IDF-tree patches. Idempotent, marker-guarded.

$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) { Write-Error "IDF_PATH not set - run this from an activated ESP-IDF environment."; exit 1 }

$target = Join-Path $env:IDF_PATH "components/lwip/port/freertos/sys_arch.c"
if (-not (Test-Path $target)) { Write-Error "not found: $target"; exit 1 }

$src = (Get-Content -Raw -Encoding UTF8 $target) -replace "`r`n", "`n"
$marker = "QMX_LWIP_THREAD_SEM_RETRY"
if ($src -match $marker) {
    Write-Host "lwip thread-sem retry: already patched." -ForegroundColor Green
    exit 0
}

$old = @'
sys_sem_t*
sys_thread_sem_init(void)
{
  sys_sem_t *sem = (sys_sem_t*)mem_malloc(sizeof(sys_sem_t*));

  if (!sem){
    ESP_LOGE(TAG, "thread_sem_init: out of memory");
    return 0;
  }

  *sem = xSemaphoreCreateBinary();
  if (!(*sem)){
    free(sem);
    ESP_LOGE(TAG, "thread_sem_init: out of memory");
    return 0;
  }

  pthread_setspecific(sys_thread_sem_key, sem);
  return sem;
}
'@ -replace "`r`n", "`n"

$new = @'
sys_sem_t*
sys_thread_sem_init(void)
{
  /* QMX_LWIP_THREAD_SEM_RETRY - see tools/patches/. Both allocations below
   * used to give up after one try, and callers in lwIP's own netconn API
   * do not check this function's NULL return before dereferencing it - a
   * transient boot-time allocation failure here became a Load access
   * fault, not a clean error. 5 attempts x 20 ms is cheap: once the
   * trough passes (this board's own measurements put it at tens of ms to
   * a few seconds), both allocations succeed on the very next try. */
  sys_sem_t *sem = NULL;
  BaseType_t sem_created = pdFALSE;
  for (int qmx_try = 0; qmx_try < 5; qmx_try++) {
    if (qmx_try) vTaskDelay(pdMS_TO_TICKS(20));
    sem = (sys_sem_t*)mem_malloc(sizeof(sys_sem_t*));
    if (!sem) continue;
    *sem = xSemaphoreCreateBinary();
    if (*sem) { sem_created = pdTRUE; break; }
    free(sem);
    sem = NULL;
  }

  if (!sem){
    ESP_LOGE(TAG, "thread_sem_init: out of memory");
    return 0;
  }
  (void)sem_created;

  pthread_setspecific(sys_thread_sem_key, sem);
  return sem;
}
'@ -replace "`r`n", "`n"

if ($src -notmatch [regex]::Escape($old)) { Write-Error "anchor not found - upstream changed, re-check by hand"; exit 1 }
$src = $src.Replace($old, $new)

[System.IO.File]::WriteAllText($target, $src, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Patched sys_arch.c: sys_thread_sem_init() retries before giving up." -ForegroundColor Green
Write-Host "  $target"
