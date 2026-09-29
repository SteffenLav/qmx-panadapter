# Standing patch - esp-mqtt's mqtt_client.c, esp_mqtt_client_init().
#
# WHY
# ---
# esp_mqtt_client_init() creates the client's own private event loop with
# `esp_event_loop_create(&no_task_loop, &client->config->event_loop_handle);`
# and NEVER CHECKS THE RETURN VALUE. If it fails - e.g. esp_event_loop_create()
# cannot get memory for the loop/queue, which is exactly the boot-time
# MALLOC_CAP_DMA/internal-heap trough this board already documents extensively
# - the function does NOT take the `_mqtt_init_failed` cleanup path. It
# returns a non-NULL client handle with `event_loop_handle` left NULL
# (the struct was zero-allocated).
#
# Every caller in this project (pskr_self.c's client_create()) correctly
# checks `esp_mqtt_client_init()` for NULL before proceeding - that check
# passes, because the handle genuinely is non-NULL. The very next call,
# `esp_mqtt_client_register_event()`, passes that NULL event_loop_handle
# into `esp_event_handler_register_with()`, which hits:
#
#   assert failed: esp_event_handler_register_with_internal esp_event.c:787
#   (event_loop)
#
# Captured on hardware 2026-09-20, task pskr_self, 22.569 s of uptime, during
# exactly the boot-time DMA-pool starvation this session was already chasing
# for the SD/httpd wedges - and because the memory trough recurred on the next
# boot too, this became a crash-reboot-crash LOOP, not a one-off.
#
# WHAT THIS DOES
# --------------
# Checks esp_event_loop_create()'s return value and takes the SAME
# `_mqtt_init_failed` path every other failure in this function already uses -
# esp_mqtt_client_destroy(client), return NULL. Callers that already check for
# NULL (this project's pskr_self.c does) then correctly retry with backoff
# instead of registering a handler on a broken client.
#
# Edits the pinned IDF tree (components/mqtt/esp-mqtt/mqtt_client.c), so an
# IDF reinstall wipes it - per-build-machine, like the other IDF-tree patches.
# Idempotent, marker-guarded.

$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) { Write-Error "IDF_PATH not set - run this from an activated ESP-IDF environment."; exit 1 }

$target = Join-Path $env:IDF_PATH "components/mqtt/esp-mqtt/mqtt_client.c"
if (-not (Test-Path $target)) { Write-Error "not found: $target"; exit 1 }

$src = (Get-Content -Raw -Encoding UTF8 $target) -replace "`r`n", "`n"
$marker = "QMX_MQTT_EVENT_LOOP_CREATE_CHECKED"
if ($src -match $marker) {
    Write-Host "mqtt event-loop-create check: already patched." -ForegroundColor Green
    exit 0
}

$old = @'
    esp_event_loop_create(&no_task_loop, &client->config->event_loop_handle);
'@ -replace "`r`n", "`n"

$new = @'
    /* QMX_MQTT_EVENT_LOOP_CREATE_CHECKED - see tools/patches/. Stock never
     * checked this return value: on failure (e.g. no memory for the loop's
     * queue) the client is returned non-NULL with event_loop_handle left
     * NULL, and the first esp_mqtt_client_register_event() call then asserts
     * inside esp_event_handler_register_with_internal(). Take the same
     * cleanup path every other init failure in this function already uses. */
    if (esp_event_loop_create(&no_task_loop, &client->config->event_loop_handle) != ESP_OK) {
        goto _mqtt_init_failed;
    }
'@ -replace "`r`n", "`n"

if ($src -notmatch [regex]::Escape($old)) { Write-Error "anchor not found - upstream changed, re-check by hand"; exit 1 }
$src = $src.Replace($old, $new)

[System.IO.File]::WriteAllText($target, $src, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Patched mqtt_client.c: esp_event_loop_create() return value checked." -ForegroundColor Green
Write-Host "  $target"
