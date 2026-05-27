#include "settings.h"

#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "settings";

// NVS namespace and keys. Keys must be <=15 chars per NVS spec.
#define NVS_NS          "qmx"
#define KEY_DB_MIN      "db_min"
#define KEY_DB_MAX      "db_max"
#define KEY_EMA_ALPHA   "ema_alpha"
#define KEY_IQ_ENABLED  "iq_en"

// Defaults — must match the runtime defaults used elsewhere.
#define DEF_DB_MIN      (-130.0f)
#define DEF_DB_MAX      (-30.0f)
#define DEF_EMA_ALPHA   (0.4f)
#define DEF_IQ_ENABLED  (true)

// Debounce: how long we wait after the last change before flushing.
#define DEBOUNCE_MS     500

// Dirty bits — which fields have been changed since last flush.
#define DIRTY_DB_MIN     (1u << 0)
#define DIRTY_DB_MAX     (1u << 1)
#define DIRTY_EMA_ALPHA  (1u << 2)
#define DIRTY_IQ_ENABLED (1u << 3)

// ---- Module state ------------------------------------------------------
static bool             s_ready          = false;
static nvs_handle_t     s_nvs            = 0;
static SemaphoreHandle_t s_mutex         = NULL;
static uint32_t         s_dirty          = 0;
static qmx_settings_t   s_pending;       // staged values awaiting flush
static TickType_t       s_last_change_tick = 0;
static TaskHandle_t     s_flush_task     = NULL;

// ---- Internal helpers --------------------------------------------------
static float u32_to_float(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static uint32_t float_to_u32(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static bool nvs_get_float(const char *key, float *out)
{
    uint32_t raw;
    esp_err_t err = nvs_get_u32(s_nvs, key, &raw);
    if (err != ESP_OK) return false;
    *out = u32_to_float(raw);
    return true;
}

static void nvs_set_float(const char *key, float v)
{
    nvs_set_u32(s_nvs, key, float_to_u32(v));
}

// ---- Flush task --------------------------------------------------------
// Runs forever, wakes every 100 ms, writes to NVS once the dirty set is
// older than DEBOUNCE_MS. Cheap to leave running; only allocates a
// 1.5kB stack.
static void flush_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_ready) continue;

        uint32_t dirty_local = 0;
        qmx_settings_t snap;
        bool do_flush = false;

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_dirty != 0) {
                TickType_t age = xTaskGetTickCount() - s_last_change_tick;
                if (age >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
                    dirty_local = s_dirty;
                    snap = s_pending;
                    s_dirty = 0;
                    do_flush = true;
                }
            }
            xSemaphoreGive(s_mutex);
        }

        if (!do_flush) continue;

        // We hold no mutex now — NVS writes can be slow.
        if (dirty_local & DIRTY_DB_MIN)     nvs_set_float(KEY_DB_MIN,    snap.db_min);
        if (dirty_local & DIRTY_DB_MAX)     nvs_set_float(KEY_DB_MAX,    snap.db_max);
        if (dirty_local & DIRTY_EMA_ALPHA)  nvs_set_float(KEY_EMA_ALPHA, snap.ema_alpha);
        if (dirty_local & DIRTY_IQ_ENABLED) nvs_set_u8(s_nvs, KEY_IQ_ENABLED, snap.iq_enabled ? 1 : 0);

        esp_err_t err = nvs_commit(s_nvs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_commit failed: 0x%x", err);
        } else {
            ESP_LOGI(TAG, "flushed dirty=0x%lx", (unsigned long)dirty_local);
        }
    }
}

// ---- Public API --------------------------------------------------------
void settings_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex create failed");
        return;
    }

    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: 0x%x — settings will not persist", err);
        return;
    }

    s_ready = true;

    // Spawn the debounced flush task. Low priority — IO, not real-time.
    xTaskCreate(flush_task, "settings_flush", 2048, NULL, 3, &s_flush_task);
    ESP_LOGI(TAG, "ready");
}

void settings_load_all(qmx_settings_t *out)
{
    if (!out) return;

    // Start from defaults; overlay whatever NVS has.
    out->db_min     = DEF_DB_MIN;
    out->db_max     = DEF_DB_MAX;
    out->ema_alpha  = DEF_EMA_ALPHA;
    out->iq_enabled = DEF_IQ_ENABLED;

    if (!s_ready) {
        ESP_LOGW(TAG, "load_all: NVS not ready, using defaults");
        return;
    }

    float fv;
    uint8_t u8v;
    if (nvs_get_float(KEY_DB_MIN,    &fv)) out->db_min    = fv;
    if (nvs_get_float(KEY_DB_MAX,    &fv)) out->db_max    = fv;
    if (nvs_get_float(KEY_EMA_ALPHA, &fv)) out->ema_alpha = fv;
    if (nvs_get_u8(s_nvs, KEY_IQ_ENABLED, &u8v) == ESP_OK) out->iq_enabled = (u8v != 0);

    ESP_LOGI(TAG, "loaded: db=[%.1f..%.1f] ema=%.2f iq=%d",
             out->db_min, out->db_max, out->ema_alpha, out->iq_enabled);
}

static void mark_dirty(uint32_t bit)
{
    if (!s_ready) return;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_dirty |= bit;
        s_last_change_tick = xTaskGetTickCount();
        xSemaphoreGive(s_mutex);
    }
}

void settings_set_db_min(float v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.db_min = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_DB_MIN);
}

void settings_set_db_max(float v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.db_max = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_DB_MAX);
}

void settings_set_ema_alpha(float v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.ema_alpha = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_EMA_ALPHA);
}

void settings_set_iq_enabled(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.iq_enabled = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_IQ_ENABLED);
}

void settings_flush(void)
{
    if (!s_ready) return;
    // Force the debounce timer to expire on next tick.
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_last_change_tick = 0;
        xSemaphoreGive(s_mutex);
    }
    // Give the flush task a chance to run. Not deterministic, but
    // usually enough.
    vTaskDelay(pdMS_TO_TICKS(200));
}