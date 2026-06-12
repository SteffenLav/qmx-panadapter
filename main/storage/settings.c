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
#define KEY_MY_CALL     "my_call"
#define KEY_MY_GRID     "my_grid"
#define KEY_WIFI_PASS   "wifi_pass"
#define KEY_LAST_VFO   "last_vfo"
#define KEY_CW_PITCH   "cw_pitch"
#define KEY_COLORMAP   "colormap"
#define KEY_CW_CAL     "cw_cal"
#define KEY_ZOOM       "zoom"
#define KEY_BRIGHTNESS "brightness"
#define KEY_LAST_MODE  "last_mode"
#define KEY_LAST_TIME  "last_time"

// Defaults — must match the runtime defaults used elsewhere.
#define DEF_DB_MIN      (-130.0f)
#define DEF_DB_MAX      (-30.0f)
#define DEF_EMA_ALPHA   (0.4f)
#define DEF_IQ_ENABLED  (true)
#define DEF_FLAT_MODE   (true)
#define DEF_CW_PITCH    (700)
#define DEF_CW_CAL      (-60)
#define DEF_ZOOM        (1.0f)
#define DEF_COLORMAP    (0)  // Thermal
#define DEF_BRIGHTNESS  (100)
#define DEF_LAST_MODE   (0)

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
#define DIRTY_CW_PITCH  (1u << 8)
#define DIRTY_COLORMAP  (1u << 9)
#define DIRTY_MY_CALL   (1u << 10)
#define DIRTY_MY_GRID   (1u << 11)
#define DIRTY_CW_CAL    (1u << 12)
#define DIRTY_ZOOM      (1u << 13)
#define DIRTY_BRIGHTNESS (1u << 14)
#define DIRTY_LAST_MODE  (1u << 15)
#define DIRTY_LAST_TIME  (1u << 16)

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
        if (dirty_local & DIRTY_MY_CALL)    nvs_set_str(s_nvs, KEY_MY_CALL,   snap.my_callsign);
        if (dirty_local & DIRTY_MY_GRID)    nvs_set_str(s_nvs, KEY_MY_GRID,   snap.my_grid);
        if (dirty_local & DIRTY_WIFI_PASS)  nvs_set_str(s_nvs, KEY_WIFI_PASS, snap.wifi_pass);
        if (dirty_local & DIRTY_LAST_VFO)  nvs_set_u32(s_nvs, KEY_LAST_VFO, snap.last_vfo_hz);
        if (dirty_local & DIRTY_CW_PITCH)  nvs_set_u16(s_nvs, KEY_CW_PITCH, snap.cw_pitch_hz);
        if (dirty_local & DIRTY_CW_CAL)    nvs_set_i16(s_nvs, KEY_CW_CAL,   snap.cw_cal_hz);
        if (dirty_local & DIRTY_ZOOM) {
            uint32_t bits; memcpy(&bits, &snap.zoom_factor, 4);
            nvs_set_u32(s_nvs, KEY_ZOOM, bits);
        }
        if (dirty_local & DIRTY_COLORMAP)  nvs_set_u8(s_nvs, KEY_COLORMAP, snap.colormap_idx);
        if (dirty_local & DIRTY_BRIGHTNESS) nvs_set_u8(s_nvs, KEY_BRIGHTNESS, snap.brightness_pct);
        if (dirty_local & DIRTY_LAST_MODE)  nvs_set_u8(s_nvs, KEY_LAST_MODE,  snap.last_ui_mode);
        if (dirty_local & DIRTY_LAST_TIME)  nvs_set_u32(s_nvs, KEY_LAST_TIME, snap.last_unix_time);

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

    // Seed s_pending from NVS so the setters' "unchanged, skip write" checks
    // compare against the persisted value, not a zero-initialized struct.
    // Without this, the first call to a setter in a session that happens to
    // match the zero/default value (e.g. settings_set_last_ui_mode(0) when
    // NVS already holds 1) is wrongly treated as a no-op and never written.
    settings_load_all(&s_pending);

    // Spawn the debounced flush task. Low priority — IO, not real-time.
    xTaskCreate(flush_task, "settings_flush", 3072, NULL, 3, &s_flush_task);
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
    out->cw_pitch_hz = DEF_CW_PITCH;
    out->cw_cal_hz   = DEF_CW_CAL;
    out->zoom_factor = DEF_ZOOM;
    out->colormap_idx = DEF_COLORMAP;
    out->brightness_pct = DEF_BRIGHTNESS;
    out->last_ui_mode = DEF_LAST_MODE;
    out->last_unix_time = 0;

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
    nvs_get_u16(s_nvs, KEY_CW_PITCH, &out->cw_pitch_hz);
    nvs_get_i16(s_nvs, KEY_CW_CAL,   &out->cw_cal_hz);
    { uint32_t bits = 0; if (nvs_get_u32(s_nvs, KEY_ZOOM, &bits) == ESP_OK) memcpy(&out->zoom_factor, &bits, 4); }
    nvs_get_u8(s_nvs, KEY_COLORMAP, &out->colormap_idx);
    nvs_get_u8(s_nvs, KEY_BRIGHTNESS, &out->brightness_pct);
    nvs_get_u8(s_nvs, KEY_LAST_MODE, &out->last_ui_mode);
    nvs_get_u32(s_nvs, KEY_LAST_TIME, &out->last_unix_time);

    // Strings: zero buffers first, then read length-bounded.
    out->wifi_ssid[0] = '\0';
    out->wifi_pass[0] = '\0';
    size_t sz = sizeof(out->wifi_ssid);
    nvs_get_str(s_nvs, KEY_WIFI_SSID, out->wifi_ssid, &sz);
    sz = sizeof(out->wifi_pass);
    nvs_get_str(s_nvs, KEY_WIFI_PASS, out->wifi_pass, &sz);

    // FT8 operator identity
    out->my_callsign[0] = '\0';
    sz = sizeof(out->my_callsign);
    nvs_get_str(s_nvs, KEY_MY_CALL, out->my_callsign, &sz);
    out->my_grid[0] = '\0';
    sz = sizeof(out->my_grid);
    nvs_get_str(s_nvs, KEY_MY_GRID, out->my_grid, &sz);

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

void settings_set_cw_pitch_hz(uint16_t hz)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.cw_pitch_hz == hz) {
        xSemaphoreGive(s_mutex);
        return;
    }
    s_pending.cw_pitch_hz = hz;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CW_PITCH);
}

void settings_set_colormap_idx(uint8_t idx)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.colormap_idx == idx) {
        xSemaphoreGive(s_mutex);
        return;
    }
    s_pending.colormap_idx = idx;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_COLORMAP);
}

void settings_set_brightness_pct(uint8_t pct)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.brightness_pct == pct) {
        xSemaphoreGive(s_mutex);
        return;
    }
    s_pending.brightness_pct = pct;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_BRIGHTNESS);
}

void settings_set_last_ui_mode(uint8_t mode)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.last_ui_mode == mode) {
        xSemaphoreGive(s_mutex);
        return;
    }
    s_pending.last_ui_mode = mode;
    s_dirty &= ~DIRTY_LAST_MODE;  // written synchronously below; nothing left for flush_task
    xSemaphoreGive(s_mutex);

    // Write immediately rather than via the debounced flush task: a mode
    // toggle is a rare, deliberate action, and if it's followed quickly by
    // a reset (e.g. a firmware flash), the 500ms debounce window can lose
    // it, leaving the device booting back into the mode the user just left.
    nvs_set_u8(s_nvs, KEY_LAST_MODE, mode);
    esp_err_t err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit (last_ui_mode) failed: 0x%x", err);
    }
}

void settings_set_last_unix_time(uint32_t unix_sec)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.last_unix_time == unix_sec) {
        xSemaphoreGive(s_mutex);
        return;
    }
    s_pending.last_unix_time = unix_sec;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_LAST_TIME);
}

void settings_set_my_callsign(const char *call)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (call) {
        strncpy(s_pending.my_callsign, call, sizeof(s_pending.my_callsign) - 1);
        s_pending.my_callsign[sizeof(s_pending.my_callsign) - 1] = '\0';
    } else {
        s_pending.my_callsign[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_MY_CALL);
}

void settings_set_my_grid(const char *grid)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (grid) {
        strncpy(s_pending.my_grid, grid, sizeof(s_pending.my_grid) - 1);
        s_pending.my_grid[sizeof(s_pending.my_grid) - 1] = '\0';
    } else {
        s_pending.my_grid[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_MY_GRID);
}

void settings_set_zoom_factor(float v)
{
    if (v < 1.0f) v = 1.0f;
    if (v > 24.0f) v = 24.0f;
    uint32_t bits; memcpy(&bits, &v, 4);
    uint32_t cur_bits; memcpy(&cur_bits, &s_pending.zoom_factor, 4);
    if (bits == cur_bits) return;
    s_pending.zoom_factor = v;
    mark_dirty(DIRTY_ZOOM);
}
void settings_set_cw_cal_hz(int16_t hz)
{
    if (hz < -200) hz = -200;
    if (hz >  200) hz =  200;
    if (s_pending.cw_cal_hz == hz) {
        return;
    }
    s_pending.cw_cal_hz = hz;
    mark_dirty(DIRTY_CW_CAL);
}
