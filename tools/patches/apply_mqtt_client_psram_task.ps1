# Contributed by Uwe DL8UG, who found the problem this patch fixes while
# building the spot map and wrote the fix.
<#
.SYNOPSIS
    Gives esp-mqtt's client task a PSRAM-backed stack instead of the
    internal-RAM-only dynamic xTaskCreate() it uses stock.

.DESCRIPTION
    ESP-IDF v5.4.4's components/mqtt/esp-mqtt/mqtt_client.c creates
    esp_mqtt_task via a bare xTaskCreate()/xTaskCreatePinnedToCore() in
    esp_mqtt_client_start() - always INTERNAL RAM for the task stack, with
    no PSRAM option, unlike every background task the qmx-panadapter
    project starts itself (see main/util/psram_task.h).

    Field-observed on the Tab5 (ESP32-P4, 2026-09-10, serial-captured) while
    bringing up net/pskr_self.c's live PSK Reporter self-spotting over MQTT:
    internal RAM on this board is chronically tight after WiFi bring-up
    (measured as low as 0 KB free, 2-4 KB largest contiguous block), so the
    stock allocation failed outright ("E mqtt_client: Error create mqtt
    task") and kept failing across repeated retries with backoff - there is
    no amount of waiting that helps when the pool itself is fragmented,
    only freeing internal RAM does. That silently killed the MQTT client
    for the whole session.

    The fix mirrors util/psram_task.h exactly: the task stack is allocated
    from PSRAM via heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    on first start and kept for the client's lifetime (esp_mqtt_task ends
    with vTaskDelete(NULL) on a full stop/disconnect, which does not free a
    statically-provided stack - exactly like every psram_task_create()
    caller in that project, and safe to reuse if the client is ever
    restarted). The TCB stays a plain static (FreeRTOS requires a TCB to be
    reachable for the life of the task; a small fixed struct is fine in
    internal RAM here, same as psram_task.c's approach).

    Because this edits the pinned IDF install (NOT the project tree), it is
    wiped if the IDF is reinstalled and must be re-applied per build
    machine - same maintenance model as the other apply_*.ps1 patches.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_mqtt_client_psram_task.ps1
#>

$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    Write-Host "IDF_PATH not set - run this from an activated ESP-IDF environment." -ForegroundColor Red
    exit 1
}

$target = Join-Path $env:IDF_PATH "components/mqtt/esp-mqtt/mqtt_client.c"
if (-not (Test-Path $target)) {
    Write-Host "mqtt_client.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "PATCHED \(qmx-panadapter, 2026-09-10\)") {
    Write-Host "Already patched (mqtt_task PSRAM stack) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{
    if (!client) {
        ESP_LOGE(TAG, "Client was not initialized");
        return ESP_ERR_INVALID_ARG;
    }
    MQTT_API_LOCK(client);
    if (client->state != MQTT_STATE_INIT && client->state != MQTT_STATE_DISCONNECTED) {
        ESP_LOGE(TAG, "Client has started");
        MQTT_API_UNLOCK(client);
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
#if MQTT_CORE_SELECTION_ENABLED
    ESP_LOGD(TAG, "Core selection enabled on %u", MQTT_TASK_CORE);
    if (xTaskCreatePinnedToCore(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, client->config->task_prio, &client->task_handle, MQTT_TASK_CORE) != pdTRUE) {
        ESP_LOGE(TAG, "Error create mqtt task");
        err = ESP_FAIL;
    }
#else
    ESP_LOGD(TAG, "Core selection disabled");
    if (xTaskCreate(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, client->config->task_prio, &client->task_handle) != pdTRUE) {
        ESP_LOGE(TAG, "Error create mqtt task");
        err = ESP_FAIL;
    }
#endif
    MQTT_API_UNLOCK(client);
    return err;
}
'@

$patched = @'
// PATCHED (qmx-panadapter, 2026-09-10): esp_mqtt_task's stack was always
// requested from INTERNAL RAM via the dynamic xTaskCreate()/
// xTaskCreatePinnedToCore() below, with no PSRAM option - unlike every
// background task the qmx-panadapter project starts itself (see its
// util/psram_task.h). Field-observed on the Tab5 (ESP32-P4): internal RAM on
// that board is chronically tight after WiFi bring-up (measured as low as
// 0 KB free, 2-4 KB largest block), so this allocation failed outright
// ("Error create mqtt task") and stayed failed across repeated retries -
// silently killing the MQTT client for the whole session, with the caller
// having no way to recover short of freeing unrelated internal RAM elsewhere.
// The fix mirrors psram_task.h exactly: a PSRAM-backed stack buffer (the
// TCB itself must stay in internal RAM - FreeRTOS requires it) via
// xTaskCreateStaticPinnedToCore(), allocated lazily on first start and kept
// for the client's lifetime (esp_mqtt_task ends with vTaskDelete(NULL) on a
// full stop/disconnect, which does not free a statically-provided stack -
// exactly like every psram_task_create() caller in that project, and safe to
// reuse if the client is ever restarted).
static StackType_t  *s_qmx_mqtt_task_stack;
static StaticTask_t  s_qmx_mqtt_task_tcb;

static TaskHandle_t qmx_mqtt_create_task_psram_stack(esp_mqtt_client_handle_t client, BaseType_t core)
{
    if (!s_qmx_mqtt_task_stack) {
        s_qmx_mqtt_task_stack = heap_caps_malloc(client->config->task_stack, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_qmx_mqtt_task_stack) {
            ESP_LOGE(TAG, "PSRAM stack alloc failed for mqtt_task");
            return NULL;
        }
    }
    return xTaskCreateStaticPinnedToCore(esp_mqtt_task, "mqtt_task", client->config->task_stack, client,
                                          client->config->task_prio, s_qmx_mqtt_task_stack, &s_qmx_mqtt_task_tcb, core);
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{
    if (!client) {
        ESP_LOGE(TAG, "Client was not initialized");
        return ESP_ERR_INVALID_ARG;
    }
    MQTT_API_LOCK(client);
    if (client->state != MQTT_STATE_INIT && client->state != MQTT_STATE_DISCONNECTED) {
        ESP_LOGE(TAG, "Client has started");
        MQTT_API_UNLOCK(client);
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
#if MQTT_CORE_SELECTION_ENABLED
    ESP_LOGD(TAG, "Core selection enabled on %u", MQTT_TASK_CORE);
    client->task_handle = qmx_mqtt_create_task_psram_stack(client, MQTT_TASK_CORE);
    if (client->task_handle == NULL) {
        ESP_LOGE(TAG, "Error create mqtt task");
        err = ESP_FAIL;
    }
#else
    ESP_LOGD(TAG, "Core selection disabled");
    client->task_handle = qmx_mqtt_create_task_psram_stack(client, tskNO_AFFINITY);
    if (client->task_handle == NULL) {
        ESP_LOGE(TAG, "Error create mqtt task");
        err = ESP_FAIL;
    }
#endif
    MQTT_API_UNLOCK(client);
    return err;
}
'@

# Normalize line endings for the match (the IDF tree uses LF)
$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock esp_mqtt_client_start block - mqtt_client.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched mqtt_client.c: esp_mqtt_task now gets a PSRAM-backed stack." -ForegroundColor Green
Write-Host "  $target"
