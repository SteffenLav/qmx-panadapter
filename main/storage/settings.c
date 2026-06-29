#include "settings.h"
#include "sd_archive.h"

#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "psram_task.h"

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
#define KEY_CQ_MSG0    "cq_msg0"
#define KEY_CQ_MSG1    "cq_msg1"
#define KEY_CQ_MSG2    "cq_msg2"
#define KEY_CQ_SEL     "cq_sel"
#define KEY_DIAG_LOG   "diag_log"
#define KEY_ONBOARDED  "onboarded"
#define KEY_FT8_FILT   "ft8_filt"
#define KEY_WIFI_ENABLED "wifi_en"
#define KEY_QMX_GPS      "qmx_gps"
#define KEY_FREQ_KP_CALC "freq_kp_calc"
#define KEY_QRZ_KEY      "qrz_key"
#define KEY_QRZ_UPLOADED "qrz_upl_n"
#define KEY_EQSL_USER    "eqsl_user"
#define KEY_EQSL_PSWD    "eqsl_pswd"
#define KEY_EQSL_UPLOADED "eqsl_upl_n"
#define KEY_CW_AUD_EN    "cw_aud_en"
#define KEY_CW_AUD_VOL   "cw_aud_vol"
#define KEY_WF_BLACK     "wf_black"
#define KEY_WF_CONTRAST  "wf_contr"
#define KEY_WF_BLEND     "wf_blend"
#define KEY_WF_WINDOW    "wf_window"
#define KEY_DISP_FLIP    "disp_flip"
#define KEY_SNAP_PEAK    "snap_peak"
#define KEY_BP_REGION    "bp_region"
#define KEY_DISTANCE_MILES "dist_miles"
#define KEY_FT8_SYNC_LINES "ft8_sync_ln"
#define KEY_FIELD_DAY_EN   "fd_en"
#define KEY_FD_CLASS       "fd_class"
#define KEY_FD_SECTION     "fd_sect"
#define KEY_SIM_MODE       "sim_mode"
#define KEY_FT8_OP_MODE    "ft8_op_mode"

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
#define DEF_LAST_MODE     (0)
#define DEF_WIFI_ENABLED  (true)
#define DEF_CW_AUD_EN     (false)
#define DEF_CW_AUD_VOL    (60)
#define DEF_WF_BLACK      (9.0f)
#define DEF_WF_CONTRAST   (45.0f)
#define DEF_WF_BLEND      (100)
#define DEF_WF_WINDOW     (0)

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
#define DIRTY_CQ_MSG0    (1u << 17)
#define DIRTY_CQ_MSG1    (1u << 18)
#define DIRTY_CQ_MSG2    (1u << 19)
#define DIRTY_CQ_SEL     (1u << 20)
#define DIRTY_DIAG_LOG   (1u << 21)
#define DIRTY_ONBOARDED    (1u << 22)
#define DIRTY_FT8_FILT     (1u << 23)
#define DIRTY_WIFI_ENABLED (1u << 24)
#define DIRTY_QMX_GPS      (1u << 25)
#define DIRTY_FREQ_KP_CALC (1u << 26)
#define DIRTY_QRZ_KEY      (1u << 27)
#define DIRTY_QRZ_UPLOADED (1u << 28)
#define DIRTY_EQSL_USER     (1u << 29)
#define DIRTY_EQSL_PSWD     (1u << 30)
#define DIRTY_EQSL_UPLOADED (1u << 31)
#define DIRTY_CW_AUD_EN     (1ull << 32)
#define DIRTY_CW_AUD_VOL    (1ull << 33)
#define DIRTY_WF_BLACK      (1ull << 34)
#define DIRTY_WF_CONTRAST   (1ull << 35)
#define DIRTY_WF_BLEND      (1ull << 36)
#define DIRTY_WF_WINDOW     (1ull << 37)
#define DIRTY_DISP_FLIP     (1ull << 38)
#define DIRTY_SNAP_PEAK     (1ull << 39)
#define DIRTY_BP_REGION     (1ull << 40)
#define DIRTY_DISTANCE_MILES (1ull << 41)
#define DIRTY_FT8_SYNC_LINES (1ull << 42)
#define DIRTY_FIELD_DAY_EN   (1ull << 43)
#define DIRTY_FD_CLASS       (1ull << 44)
#define DIRTY_FD_SECTION     (1ull << 45)
#define DIRTY_SIM_MODE       (1ull << 46)
#define DIRTY_FT8_OP_MODE    (1ull << 47)

// ---- Module state ------------------------------------------------------
static bool             s_ready          = false;
static nvs_handle_t     s_nvs            = 0;
static SemaphoreHandle_t s_mutex         = NULL;
static uint64_t         s_dirty          = 0;
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

static void load_from_nvs(qmx_settings_t *out);

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

        uint64_t dirty_local = 0;
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
        if (dirty_local & DIRTY_CQ_MSG0)    nvs_set_str(s_nvs, KEY_CQ_MSG0, snap.cq_msg[0]);
        if (dirty_local & DIRTY_CQ_MSG1)    nvs_set_str(s_nvs, KEY_CQ_MSG1, snap.cq_msg[1]);
        if (dirty_local & DIRTY_CQ_MSG2)    nvs_set_str(s_nvs, KEY_CQ_MSG2, snap.cq_msg[2]);
        if (dirty_local & DIRTY_CQ_SEL)     nvs_set_u8(s_nvs, KEY_CQ_SEL, snap.cq_sel);
        if (dirty_local & DIRTY_DIAG_LOG)   nvs_set_u8(s_nvs, KEY_DIAG_LOG, snap.diag_log ? 1 : 0);
        if (dirty_local & DIRTY_ONBOARDED)  nvs_set_u8(s_nvs, KEY_ONBOARDED, snap.onboarded ? 1 : 0);
        if (dirty_local & DIRTY_FT8_FILT)     nvs_set_blob(s_nvs, KEY_FT8_FILT, &snap.ft8_filters, sizeof(snap.ft8_filters));
        if (dirty_local & DIRTY_WIFI_ENABLED) nvs_set_u8(s_nvs, KEY_WIFI_ENABLED, snap.wifi_enabled ? 1 : 0);
        if (dirty_local & DIRTY_QMX_GPS)      nvs_set_u8(s_nvs, KEY_QMX_GPS,      snap.qmx_gps      ? 1 : 0);
        if (dirty_local & DIRTY_FREQ_KP_CALC) nvs_set_u8(s_nvs, KEY_FREQ_KP_CALC, snap.freq_kp_calc ? 1 : 0);
        if (dirty_local & DIRTY_QRZ_KEY)      nvs_set_str(s_nvs, KEY_QRZ_KEY, snap.qrz_api_key);
        if (dirty_local & DIRTY_QRZ_UPLOADED) nvs_set_u32(s_nvs, KEY_QRZ_UPLOADED, snap.qrz_uploaded_n);
        if (dirty_local & DIRTY_EQSL_USER)     nvs_set_str(s_nvs, KEY_EQSL_USER, snap.eqsl_user);
        if (dirty_local & DIRTY_EQSL_PSWD)     nvs_set_str(s_nvs, KEY_EQSL_PSWD, snap.eqsl_pswd);
        if (dirty_local & DIRTY_EQSL_UPLOADED) nvs_set_u32(s_nvs, KEY_EQSL_UPLOADED, snap.eqsl_uploaded_n);
        if (dirty_local & DIRTY_CW_AUD_EN)  nvs_set_u8(s_nvs, KEY_CW_AUD_EN,  snap.cw_audio_en ? 1 : 0);
        if (dirty_local & DIRTY_CW_AUD_VOL) nvs_set_u8(s_nvs, KEY_CW_AUD_VOL, snap.cw_audio_vol);
        if (dirty_local & DIRTY_WF_BLACK)    nvs_set_float(KEY_WF_BLACK,    snap.wf_black_db);
        if (dirty_local & DIRTY_WF_CONTRAST) nvs_set_float(KEY_WF_CONTRAST, snap.wf_contrast_db);
        if (dirty_local & DIRTY_WF_BLEND)    nvs_set_u8(s_nvs, KEY_WF_BLEND,  snap.wf_floor_blend);
        if (dirty_local & DIRTY_WF_WINDOW)   nvs_set_u8(s_nvs, KEY_WF_WINDOW, snap.wf_window);
        if (dirty_local & DIRTY_DISP_FLIP)   nvs_set_u8(s_nvs, KEY_DISP_FLIP, snap.display_flip ? 1 : 0);
        if (dirty_local & DIRTY_SNAP_PEAK)   nvs_set_u8(s_nvs, KEY_SNAP_PEAK, snap.snap_to_peak ? 1 : 0);
        if (dirty_local & DIRTY_BP_REGION)   nvs_set_u8(s_nvs, KEY_BP_REGION, snap.bandplan_region);
        if (dirty_local & DIRTY_DISTANCE_MILES) nvs_set_u8(s_nvs, KEY_DISTANCE_MILES, snap.distance_in_miles ? 1 : 0);
        if (dirty_local & DIRTY_FT8_SYNC_LINES) nvs_set_u8(s_nvs, KEY_FT8_SYNC_LINES, snap.ft8_sync_lines ? 1 : 0);
        if (dirty_local & DIRTY_FIELD_DAY_EN) nvs_set_u8(s_nvs, KEY_FIELD_DAY_EN, snap.field_day_en ? 1 : 0);
        if (dirty_local & DIRTY_FD_CLASS)     nvs_set_str(s_nvs, KEY_FD_CLASS, snap.fd_class);
        if (dirty_local & DIRTY_FD_SECTION)   nvs_set_str(s_nvs, KEY_FD_SECTION, snap.fd_section);
        if (dirty_local & DIRTY_SIM_MODE)     nvs_set_u8(s_nvs, KEY_SIM_MODE, snap.sim_mode_en ? 1 : 0);
        if (dirty_local & DIRTY_FT8_OP_MODE)  nvs_set_u8(s_nvs, KEY_FT8_OP_MODE, snap.ft8_op_mode);

        esp_err_t err = nvs_commit(s_nvs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_commit failed: 0x%x", err);
        } else {
            ESP_LOGI(TAG, "flushed dirty=0x%llx", (unsigned long long)dirty_local);
            sd_archive_mark_config_dirty();  // re-mirror config export to SD if a card is in
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
    load_from_nvs(&s_pending);

    // Spawn the debounced flush task. Low priority — IO, not real-time.
    s_flush_task = psram_task_create(flush_task, "settings_flush", 3072, NULL, 3, tskNO_AFFINITY);
    ESP_LOGI(TAG, "ready");
}

// Read persisted values straight from NVS (defaults for anything unset).
// Used to seed s_pending at init. Most callers should use settings_load_all(),
// which returns the live staged state (includes not-yet-flushed changes).
static void load_from_nvs(qmx_settings_t *out)
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
    out->cq_msg[0][0] = '\0';
    out->cq_msg[1][0] = '\0';
    out->cq_msg[2][0] = '\0';
    out->cq_sel = 0;
    out->diag_log = false;
    out->onboarded = false;
    out->wifi_enabled = DEF_WIFI_ENABLED;
    out->qmx_gps = false;
    out->freq_kp_calc = false;
    out->qrz_api_key[0] = '\0';
    out->qrz_uploaded_n = 0;
    out->eqsl_user[0] = '\0';
    out->eqsl_pswd[0] = '\0';
    out->eqsl_uploaded_n = 0;
    out->cw_audio_en  = DEF_CW_AUD_EN;
    out->cw_audio_vol = DEF_CW_AUD_VOL;
    out->wf_black_db    = DEF_WF_BLACK;
    out->wf_contrast_db = DEF_WF_CONTRAST;
    out->wf_floor_blend = DEF_WF_BLEND;
    out->wf_window      = DEF_WF_WINDOW;
    out->display_flip   = false;
    out->snap_to_peak   = true;   // on by default (legacy behaviour)
    out->bandplan_region = 0;     // 0 = auto (derive from grid)
    memset(&out->ft8_filters, 0, sizeof(out->ft8_filters));
    out->field_day_en = false;
    out->fd_class[0]  = '\0';
    out->fd_section[0] = '\0';
    out->sim_mode_en = false;
    out->ft8_op_mode = 0;     // FT8

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

    // FT8 CQ presets
    sz = sizeof(out->cq_msg[0]); nvs_get_str(s_nvs, KEY_CQ_MSG0, out->cq_msg[0], &sz);
    sz = sizeof(out->cq_msg[1]); nvs_get_str(s_nvs, KEY_CQ_MSG1, out->cq_msg[1], &sz);
    sz = sizeof(out->cq_msg[2]); nvs_get_str(s_nvs, KEY_CQ_MSG2, out->cq_msg[2], &sz);
    nvs_get_u8(s_nvs, KEY_CQ_SEL, &out->cq_sel);
    if (out->cq_sel > 2) out->cq_sel = 0;

    if (nvs_get_u8(s_nvs, KEY_DIAG_LOG,    &u8v) == ESP_OK) out->diag_log    = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_ONBOARDED,  &u8v) == ESP_OK) out->onboarded  = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_WIFI_ENABLED, &u8v) == ESP_OK) out->wifi_enabled = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_QMX_GPS,      &u8v) == ESP_OK) out->qmx_gps      = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_FREQ_KP_CALC, &u8v) == ESP_OK) out->freq_kp_calc = (u8v != 0);
    out->qrz_api_key[0] = '\0';
    sz = sizeof(out->qrz_api_key);
    nvs_get_str(s_nvs, KEY_QRZ_KEY, out->qrz_api_key, &sz);
    nvs_get_u32(s_nvs, KEY_QRZ_UPLOADED, &out->qrz_uploaded_n);
    out->eqsl_user[0] = '\0';
    sz = sizeof(out->eqsl_user);
    nvs_get_str(s_nvs, KEY_EQSL_USER, out->eqsl_user, &sz);
    out->eqsl_pswd[0] = '\0';
    sz = sizeof(out->eqsl_pswd);
    nvs_get_str(s_nvs, KEY_EQSL_PSWD, out->eqsl_pswd, &sz);
    nvs_get_u32(s_nvs, KEY_EQSL_UPLOADED, &out->eqsl_uploaded_n);

    if (nvs_get_u8(s_nvs, KEY_CW_AUD_EN, &u8v) == ESP_OK) out->cw_audio_en = (u8v != 0);
    nvs_get_u8(s_nvs, KEY_CW_AUD_VOL, &out->cw_audio_vol);

    if (nvs_get_float(KEY_WF_BLACK,    &fv)) out->wf_black_db    = fv;
    if (nvs_get_float(KEY_WF_CONTRAST, &fv)) out->wf_contrast_db = fv;
    nvs_get_u8(s_nvs, KEY_WF_BLEND,  &out->wf_floor_blend);
    nvs_get_u8(s_nvs, KEY_WF_WINDOW, &out->wf_window);
    if (out->wf_floor_blend > 100) out->wf_floor_blend = 100;
    if (out->wf_window > 2)        out->wf_window = 0;
    if (nvs_get_u8(s_nvs, KEY_DISP_FLIP, &u8v) == ESP_OK) out->display_flip = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_SNAP_PEAK, &u8v) == ESP_OK) out->snap_to_peak = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_BP_REGION, &u8v) == ESP_OK) out->bandplan_region = (u8v <= 3) ? u8v : 0;
    if (nvs_get_u8(s_nvs, KEY_DISTANCE_MILES, &u8v) == ESP_OK) out->distance_in_miles = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_FT8_SYNC_LINES, &u8v) == ESP_OK) out->ft8_sync_lines = (u8v != 0);

    sz = sizeof(out->ft8_filters);
    nvs_get_blob(s_nvs, KEY_FT8_FILT, &out->ft8_filters, &sz);

    if (nvs_get_u8(s_nvs, KEY_FIELD_DAY_EN, &u8v) == ESP_OK) out->field_day_en = (u8v != 0);
    out->fd_class[0] = '\0';
    sz = sizeof(out->fd_class);
    nvs_get_str(s_nvs, KEY_FD_CLASS, out->fd_class, &sz);
    out->fd_section[0] = '\0';
    sz = sizeof(out->fd_section);
    nvs_get_str(s_nvs, KEY_FD_SECTION, out->fd_section, &sz);

    if (nvs_get_u8(s_nvs, KEY_SIM_MODE, &u8v) == ESP_OK) out->sim_mode_en = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_FT8_OP_MODE, &u8v) == ESP_OK) out->ft8_op_mode = u8v;

    ESP_LOGI(TAG, "loaded: db=[%.1f..%.1f] ema=%.2f iq=%d",
             out->db_min, out->db_max, out->ema_alpha, out->iq_enabled);
}

void settings_load_all(qmx_settings_t *out)
{
    if (!out) return;
    // Return the live staged state: it's seeded from NVS at init and updated
    // by every setter, so it reflects changes immediately - even before the
    // debounced flush writes them to flash. (Re-reading NVS here would return
    // stale values for up to DEBOUNCE_MS after a set.)
    if (s_ready && s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        *out = s_pending;
        xSemaphoreGive(s_mutex);
        return;
    }
    load_from_nvs(out);  // not initialised yet: defaults + whatever NVS has
}

static void mark_dirty(uint64_t bit)
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
    // 64-bit mask: ~(1u<<15) is a 32-bit value that would zero-extend and clear
    // the upper dirty bits (e.g. cw_audio, bits 32/33). Cast keeps them intact.
    s_dirty &= ~((uint64_t)DIRTY_LAST_MODE);  // written synchronously below; nothing left for flush_task
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

void settings_set_cq_msg(uint8_t idx, const char *text)
{
    if (!s_ready || idx > 2) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (text) {
        strncpy(s_pending.cq_msg[idx], text, sizeof(s_pending.cq_msg[idx]) - 1);
        s_pending.cq_msg[idx][sizeof(s_pending.cq_msg[idx]) - 1] = '\0';
    } else {
        s_pending.cq_msg[idx][0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(idx == 0 ? DIRTY_CQ_MSG0 : idx == 1 ? DIRTY_CQ_MSG1 : DIRTY_CQ_MSG2);
}

void settings_set_cq_sel(uint8_t idx)
{
    if (!s_ready || idx > 2) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.cq_sel == idx) { xSemaphoreGive(s_mutex); return; }
    s_pending.cq_sel = idx;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CQ_SEL);
}

void settings_set_diag_log(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.diag_log == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.diag_log = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_DIAG_LOG);
}

void settings_set_onboarded(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.onboarded == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.onboarded = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_ONBOARDED);
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

void settings_set_ft8_filters(const ft8_filters_t *f)
{
    if (!s_ready || !f) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.ft8_filters = *f;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FT8_FILT);
}

void settings_set_wifi_enabled(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.wifi_enabled = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_WIFI_ENABLED);
}

void settings_set_qmx_gps(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.qmx_gps = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_QMX_GPS);
}

void settings_set_freq_kp_calc(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.freq_kp_calc == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.freq_kp_calc = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FREQ_KP_CALC);
}

void settings_set_cw_audio_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.cw_audio_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.cw_audio_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CW_AUD_EN);
}

void settings_set_cw_audio_vol(uint8_t v)
{
    if (!s_ready) return;
    if (v > 100) v = 100;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.cw_audio_vol == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.cw_audio_vol = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CW_AUD_VOL);
}

void settings_set_qrz_api_key(const char *key)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (key) {
        strncpy(s_pending.qrz_api_key, key, sizeof(s_pending.qrz_api_key) - 1);
        s_pending.qrz_api_key[sizeof(s_pending.qrz_api_key) - 1] = '\0';
    } else {
        s_pending.qrz_api_key[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_QRZ_KEY);
}

void settings_set_qrz_uploaded_n(uint32_t n)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.qrz_uploaded_n == n) { xSemaphoreGive(s_mutex); return; }
    s_pending.qrz_uploaded_n = n;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_QRZ_UPLOADED);
}

void settings_set_eqsl_user(const char *user)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (user) {
        strncpy(s_pending.eqsl_user, user, sizeof(s_pending.eqsl_user) - 1);
        s_pending.eqsl_user[sizeof(s_pending.eqsl_user) - 1] = '\0';
    } else {
        s_pending.eqsl_user[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_EQSL_USER);
}

void settings_set_eqsl_pswd(const char *pswd)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (pswd) {
        strncpy(s_pending.eqsl_pswd, pswd, sizeof(s_pending.eqsl_pswd) - 1);
        s_pending.eqsl_pswd[sizeof(s_pending.eqsl_pswd) - 1] = '\0';
    } else {
        s_pending.eqsl_pswd[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_EQSL_PSWD);
}

void settings_set_eqsl_uploaded_n(uint32_t n)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.eqsl_uploaded_n == n) { xSemaphoreGive(s_mutex); return; }
    s_pending.eqsl_uploaded_n = n;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_EQSL_UPLOADED);
}

void settings_set_wf_black_db(float db)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.wf_black_db = db;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_WF_BLACK);
}

void settings_set_wf_contrast_db(float db)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.wf_contrast_db = db;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_WF_CONTRAST);
}

void settings_set_wf_floor_blend(uint8_t pct)
{
    if (!s_ready) return;
    if (pct > 100) pct = 100;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.wf_floor_blend == pct) { xSemaphoreGive(s_mutex); return; }
    s_pending.wf_floor_blend = pct;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_WF_BLEND);
}

void settings_set_wf_window(uint8_t idx)
{
    if (!s_ready) return;
    if (idx > 2) idx = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.wf_window == idx) { xSemaphoreGive(s_mutex); return; }
    s_pending.wf_window = idx;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_WF_WINDOW);
}

void settings_set_display_flip(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.display_flip == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.display_flip = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_DISP_FLIP);
}

void settings_set_snap_to_peak(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.snap_to_peak == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.snap_to_peak = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_SNAP_PEAK);
}

void settings_set_distance_in_miles(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.distance_in_miles == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.distance_in_miles = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_DISTANCE_MILES);
}

void settings_set_ft8_sync_lines(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.ft8_sync_lines == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.ft8_sync_lines = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FT8_SYNC_LINES);
}

void settings_set_bandplan_region(uint8_t v)
{
    if (!s_ready) return;
    if (v > 3) v = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.bandplan_region == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.bandplan_region = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_BP_REGION);
}

void settings_set_field_day_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.field_day_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.field_day_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FIELD_DAY_EN);
}

void settings_set_fd_class(const char *cls)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (cls) {
        strncpy(s_pending.fd_class, cls, sizeof(s_pending.fd_class) - 1);
        s_pending.fd_class[sizeof(s_pending.fd_class) - 1] = '\0';
    } else {
        s_pending.fd_class[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FD_CLASS);
}

void settings_set_fd_section(const char *section)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (section) {
        strncpy(s_pending.fd_section, section, sizeof(s_pending.fd_section) - 1);
        s_pending.fd_section[sizeof(s_pending.fd_section) - 1] = '\0';
    } else {
        s_pending.fd_section[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FD_SECTION);
}

void settings_set_sim_mode_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.sim_mode_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.sim_mode_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_SIM_MODE);
}

void settings_set_ft8_op_mode(uint8_t v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.ft8_op_mode == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.ft8_op_mode = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FT8_OP_MODE);
}
