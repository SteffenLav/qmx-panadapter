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
#define KEY_FLAT_MODE   "flat_md"
#define KEY_WIFI_SSID   "wifi_ssid"
#define KEY_WIFI_PASS   "wifi_pass"
#define KEY_LAST_VFO   "last_vfo"

// Defaults — must match the runtime defaults used elsewhere.
#define DEF_DB_MIN      (-130.0f)
#define DEF_DB_MAX      (-30.0f)
#define DEF_EMA_ALPHA   (0.4f)
#define DEF_IQ_ENABLED  (true)
#define DEF_FLAT_MODE   (true)

// Debounce: how long we wait after the last change before flushing.
#define DEBOUNCE_MS     500

// Dirty bits — which fields have been changed since last flush.
#define DIRTY_DB_MIN     (1u << 0)
#define DIRTY_DB_MAX     (1u << 1)
#define DIRTY_EMA_ALPHA  (1u << 2)
#define DIRTY_IQ_ENABLED (1u << 3)
#define DIRTY_FLAT_MODE  (1u << 7)
#define DIRTY_WIFI_SSID  (1u << 4)
#define DIRTY_WIFI_PASS  (1u << 5)
#define DIRTY_LAST_VFO  (1u << 6)

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
        if (dirty_local & DIRTY_FLAT_MODE)  nvs_set_u8(s_nvs, KEY_FLAT_MODE,  snap.flat_mode    ? 1 : 0);
        if (dirty_local & DIRTY_WIFI_SSID)  nvs_set_str(s_nvs, KEY_WIFI_SSID, snap.wifi_ssid);
        if (dirty_local & DIRTY_WIFI_PASS)  nvs_set_str(s_nvs, KEY_WIFI_PASS, snap.wifi_pass);
        if (dirty_local & DIRTY_LAST_VFO)  nvs_set_u32(s_nvs, KEY_LAST_VFO, snap.last_vfo_hz);

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

    esp_err_t err = nvs_flash_init_partition("user_nvs");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("user_nvs"));
        err = nvs_flash_init_partition("user_nvs");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "user_nvs init failed: 0x%x", err);
        return;
    }
    err = nvs_open_from_partition("user_nvs", NVS_NS, NVS_READWRITE, &s_nvs);
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
    out->flat_mode  = DEF_FLAT_MODE;
    out->last_vfo_hz = 0;

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
    if (nvs_get_u8(s_nvs, KEY_FLAT_MODE,  &u8v) == ESP_OK) out->flat_mode  = (u8v != 0);
    nvs_get_u32(s_nvs, KEY_LAST_VFO, &out->last_vfo_hz);

    // Strings: zero buffers first, then read length-bounded.
    out->wifi_ssid[0] = '\0';
    out->wifi_pass[0] = '\0';
    size_t sz = sizeof(out->wifi_ssid);
    nvs_get_str(s_nvs, KEY_WIFI_SSID, out->wifi_ssid, &sz);
    sz = sizeof(out->wifi_pass);
    nvs_get_str(s_nvs, KEY_WIFI_PASS, out->wifi_pass, &sz);

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

void settings_set_flat_mode(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.flat_mode = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FLAT_MODE);
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
void settings_set_wifi_ssid(const char *ssid)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ssid) {
        strncpy(s_pending.wifi_ssid, ssid, sizeof(s_pending.wifi_ssid) - 1);
        s_pending.wifi_ssid[sizeof(s_pending.wifi_ssid) - 1] = '\0';
    } else {
        s_pending.wifi_ssid[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_WIFI_SSID);
}

void settings_set_wifi_pass(const char *pass)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (pass) {
        strncpy(s_pending.wifi_pass, pass, sizeof(s_pending.wifi_pass) - 1);
        s_pending.wifi_pass[sizeof(s_pending.wifi_pass) - 1] = '\0';
    } else {
        s_pending.wifi_pass[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_WIFI_PASS);
}

void settings_set_last_vfo(uint32_t hz)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.last_vfo_hz == hz) {
        xSemaphoreGive(s_mutex);
        return;  // unchanged, skip the dirty/flush cycle
    }
    s_pending.last_vfo_hz = hz;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_LAST_VFO);
}
