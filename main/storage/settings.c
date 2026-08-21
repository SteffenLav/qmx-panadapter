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
#define KEY_FT8_FREQ   "ft8_freq"
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
#define KEY_CQ_MAX     "cq_max"
#define KEY_HOUND_MODE "hound_md"
#define KEY_ONBOARDED  "onboarded"
#define KEY_FT8_FILT   "ft8_filt"
#define KEY_KBD_BIND   "kbd_bind"
#define KEY_WIFI_ENABLED "wifi_en"
#define KEY_QMX_GPS      "qmx_gps"
#define KEY_QMX_TPUSH    "qmx_tpush"
#define KEY_FREQ_KP_CALC "freq_kp_calc"
#define KEY_FREQ_KP_DX   "freq_kp_dx"
#define KEY_FREQ_KP_DY   "freq_kp_dy"
#define KEY_FREQ_KP_SMALL "freq_kp_small"
#define KEY_PASSBAND_HZ   "passband_hz"
#define KEY_QRZ_KEY      "qrz_key"
#define KEY_QRZ_UPLOADED "qrz_upl_n"
#define KEY_EQSL_USER    "eqsl_user"
#define KEY_EQSL_PSWD    "eqsl_pswd"
#define KEY_EQSL_UPLOADED "eqsl_upl_n"
#define KEY_CL_URL       "cl_url"
#define KEY_CL_KEY       "cl_key"
#define KEY_CL_STATION   "cl_stn"
#define KEY_CL_UPLOADED  "cl_upl_n"
#define KEY_CW_AUD_EN    "cw_aud_en"
#define KEY_CW_AUD_VOL   "cw_aud_vol"
#define KEY_WF_BLACK     "wf_black"
#define KEY_WF_CONTRAST  "wf_contr"
#define KEY_WF_BLEND     "wf_blend"
#define KEY_WF_WINDOW    "wf_window"
#define KEY_DISP_FLIP    "disp_flip"
#define KEY_QMX_VOL      "qmx_vol_db"
#define KEY_CW_TX_OFF    "cw_tx_off"
#define KEY_CQ_LISTEN    "cq_listen"
#define KEY_SWR_LIMIT    "swr_lim"
#define KEY_ACT_TYPE     "act_type"
#define KEY_ACT_REF      "act_ref"
#define KEY_SNAP_PEAK    "snap_peak"
#define KEY_BP_REGION    "bp_region"
#define KEY_DISTANCE_MILES "dist_miles"
#define KEY_RIT_PILL_SHOW  "rit_pill"
#define KEY_SPUR_SUP       "spur_sup"
#define KEY_FT8_EARLY_DEC  "ft8_earlydec"
#define KEY_GREYLIST_EN    "greylist_en"
#define KEY_PSKREP_EN      "pskrep_en"
#define KEY_PSK_RX_EN      "pskrx_en"
#define KEY_BT_MOUSE_EN    "btmouse_en"
#define KEY_CLUSTER_EN     "cluster_en"
#define KEY_SPOTS_MODE_FLT "spot_modeflt"
#define KEY_SPOTS_EN       "spots_en"
#define KEY_RBN_EN         "rbn_en"
#define KEY_SOTA_EN        "sota_en"
#define KEY_WIFI_KNOWN     "wifi_known"
#define KEY_TX_TONE_HZ     "tx_tone_hz"
#define KEY_TX_TONE_HOLD   "tx_tone_hold"
#define KEY_FT8_SYNC_LINES "ft8_sync_ln"
#define KEY_FIELD_DAY_EN   "fd_en"
#define KEY_FD_CLASS       "fd_class"
#define KEY_FD_SECTION     "fd_sect"
#define KEY_SIM_MODE       "sim_mode"
#define KEY_FT8_OP_MODE    "ft8_op_mode"
#define KEY_CHARGE_LIM_EN  "chg_lim_en"
#define KEY_CHARGE_LIM_PCT "chg_lim_pct"
#define KEY_RESMON_EN      "resmon_en"
#define KEY_RESMON_DX      "resmon_dx"
#define KEY_RESMON_DY      "resmon_dy"
#define KEY_DISP_SLEEP     "disp_sleep"
#define KEY_LOTW_DXCC      "lotw_dxcc"
#define KEY_LOTW_CQZ       "lotw_cqz"
#define KEY_LOTW_ITUZ      "lotw_ituz"
#define KEY_LOTW_STATE     "lotw_state"
#define KEY_LOTW_COUNTY    "lotw_cnty"
#define KEY_LOTW_UPLOADED  "lotw_upl_n"

// Defaults — must match the runtime defaults used elsewhere.
#define DEF_DB_MIN      (-130.0f)
#define DEF_DB_MAX      (-30.0f)
#define DEF_EMA_ALPHA   (0.4f)
#define DEF_IQ_ENABLED  (true)
#define DEF_FLAT_MODE   (true)
#define DEF_CW_PITCH    (700)
/* Per-unit CW display trim. Was -60, which came in with the commit that first
 * read the CW offset from the radio over CAT - i.e. it was calibrated BEFORE
 * that reading existed, and then never revisited. It is now measurably wrong:
 * with it, a signal on Roy KI0ER's 7.060.000 shows at 7.060.040 (see the CW
 * display-offset quirk in CLAUDE.md, which does the arithmetic). Zero is the
 * honest default - the slider stays, for genuine per-unit trimming. */
#define DEF_CW_CAL      (0)
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
#define DEF_CHARGE_LIM_EN  (false)
#define DEF_CHARGE_LIM_PCT (80)

// Debounce: how long we wait after the last change before flushing.
#define DEBOUNCE_MS     500

// ---- Dirty set ---------------------------------------------------------------
// Which fields have changed since the last flush.
//
// This was a uint64_t bitmask until v1.3.4, and by then every one of its 64 bits
// was spoken for - so the next setting that wanted to persist simply could not,
// and the workaround (sharing a bit with an unrelated field, as the LoTW
// state/county fields do) only works for values that are always written
// together. A wider integer wasn't an option either: riscv32 GCC has no
// __int128. So the set is now a word array addressed by plain BIT INDEX, with
// room to grow by changing one number.
//
// Consequence to remember: every DIRTY_* below is an INDEX, not a mask. Never
// write `dirty & DIRTY_X` - use dirty_test(). The struct type makes the old
// bitwise spelling a compile error rather than a silently-wrong test, which is
// what makes this refactor safe to do to 65 call sites at once.
//
// Adding a setting: give it the next free index, bump DIRTY_WORDS if you cross a
// 32-bit boundary past the end, and add the bit to s_config_export_bits[] if
// config_io_export() actually writes the field.
#define DIRTY_WORDS      4                        /* 128 bits; 62 spare today */
#define DIRTY_BITS_MAX   (DIRTY_WORDS * 32)

typedef struct { uint32_t w[DIRTY_WORDS]; } dirty_t;

static inline void dirty_set(dirty_t *d, int bit)
{
    if (bit >= 0 && bit < DIRTY_BITS_MAX) d->w[bit >> 5] |= 1u << (bit & 31);
}

static inline bool dirty_test(const dirty_t *d, int bit)
{
    if (bit < 0 || bit >= DIRTY_BITS_MAX) return false;
    return ((d->w[bit >> 5] >> (bit & 31)) & 1u) != 0;
}

static inline void dirty_clear_bit(dirty_t *d, int bit)
{
    if (bit >= 0 && bit < DIRTY_BITS_MAX) d->w[bit >> 5] &= ~(1u << (bit & 31));
}

static inline void dirty_clear_all(dirty_t *d)
{
    for (int i = 0; i < DIRTY_WORDS; i++) d->w[i] = 0;
}

static inline bool dirty_any(const dirty_t *d)
{
    for (int i = 0; i < DIRTY_WORDS; i++) if (d->w[i]) return true;
    return false;
}

// True if any bit in `bits` (a list of indices) is set - the replacement for
// the old `dirty & SOME_MASK` idiom.
static inline bool dirty_test_any(const dirty_t *d, const uint8_t *bits, size_t n)
{
    for (size_t i = 0; i < n; i++) if (dirty_test(d, bits[i])) return true;
    return false;
}

#define DIRTY_DB_MIN     0
#define DIRTY_DB_MAX     1
#define DIRTY_EMA_ALPHA  2
#define DIRTY_IQ_ENABLED 3
#define DIRTY_FLAT_MODE  7
#define DIRTY_WIFI_SSID  4
#define DIRTY_WIFI_PASS  5
#define DIRTY_LAST_VFO  6
#define DIRTY_CW_PITCH  8
#define DIRTY_COLORMAP  9
#define DIRTY_MY_CALL   10
#define DIRTY_MY_GRID   11
#define DIRTY_CW_CAL    12
#define DIRTY_ZOOM      13
#define DIRTY_BRIGHTNESS 14
#define DIRTY_LAST_MODE  15
#define DIRTY_LAST_TIME  16
#define DIRTY_CQ_MSG0    17
#define DIRTY_CQ_MSG1    18
#define DIRTY_CQ_MSG2    19
#define DIRTY_CQ_SEL     20
// Bit 21 was the last free bit back when this was a 64-bit mask - spending it
// (v1.3.3) is what finally forced the widening above. The LoTW state/county
// fields still share DIRTY_LOTW_DXCC, not because bits are scarce now but
// because those three are only ever written together.
#define DIRTY_QMX_VOL      21
#define DIRTY_ONBOARDED    22
#define DIRTY_FT8_FILT     23
#define DIRTY_WIFI_ENABLED 24
#define DIRTY_QMX_GPS      25
#define DIRTY_FREQ_KP_CALC 26
#define DIRTY_QRZ_KEY      27
#define DIRTY_QRZ_UPLOADED 28
#define DIRTY_EQSL_USER     29
#define DIRTY_EQSL_PSWD     30
#define DIRTY_EQSL_UPLOADED 31
/* Cloudlog / Wavelog (#171) - self-hosted upload target */
#define DIRTY_CL_URL        91
#define DIRTY_CL_KEY        92
#define DIRTY_CL_STATION    93
#define DIRTY_CL_UPLOADED   94
#define DIRTY_CW_AUD_EN     32
#define DIRTY_CW_AUD_VOL    33
#define DIRTY_WF_BLACK      34
#define DIRTY_WF_CONTRAST   35
#define DIRTY_WF_BLEND      36
#define DIRTY_WF_WINDOW     37
#define DIRTY_DISP_FLIP     38
#define DIRTY_SNAP_PEAK     39
#define DIRTY_BP_REGION     40
#define DIRTY_DISTANCE_MILES 41
#define DIRTY_RIT_PILL_SHOW  88
#define DIRTY_SPUR_SUP       89
#define DIRTY_QMX_TPUSH      90
#define DIRTY_FT8_SYNC_LINES 42
#define DIRTY_FIELD_DAY_EN   43
#define DIRTY_FD_CLASS       44
#define DIRTY_FD_SECTION     45
#define DIRTY_SIM_MODE       46
#define DIRTY_FT8_OP_MODE    47
#define DIRTY_FREQ_KP_POS    48
#define DIRTY_FREQ_KP_SMALL  49
#define DIRTY_PASSBAND_HZ    50
#define DIRTY_FT8_FREQ       51
#define DIRTY_CHARGE_LIM_EN  52
#define DIRTY_CHARGE_LIM_PCT 53
#define DIRTY_RESMON_EN      54
#define DIRTY_RESMON_POS     55
#define DIRTY_LOTW_DXCC      56
#define DIRTY_LOTW_CQZ       57
#define DIRTY_LOTW_ITUZ      58
#define DIRTY_LOTW_UPLOADED  59
#define DIRTY_DISP_SLEEP     60
#define DIRTY_FT8_EARLY_DEC  61
#define DIRTY_GREYLIST_EN    62
#define DIRTY_PSKREP_EN      63
// --- past the old 64-bit ceiling (the whole point of DIRTY_WORDS) ---
#define DIRTY_TX_TONE_HZ     64
#define DIRTY_TX_TONE_HOLD   65
// 67..74 are RESERVED for the CW page on branch feat/cw-page (CW_MSG0..5,
// CW_PARK, CW_SIM). Do not reuse them here or the two branches collide on
// merge and settings land in the wrong fields.
#define DIRTY_SPOTS_EN       75
#define DIRTY_RBN_EN         76
#define DIRTY_WIFI_KNOWN     77
#define DIRTY_CQ_MAX_CALLS   66
// 78 and up: after the CW-page reservation above. The CW TX offset lives on
// main (it is a tuning behaviour, not part of the CW page), so it deliberately
// does NOT take one of the reserved 67..74.
#define DIRTY_CW_TX_OFFSET   78
#define DIRTY_CQ_LISTEN      79
#define DIRTY_SWR_LIMIT      80
// One bit for both activation fields: they are only ever written together by
// settings_set_activation(), so a second bit would buy nothing.
#define DIRTY_ACTIVATION     81
#define DIRTY_PSK_RX_EN      82
#define DIRTY_BT_MOUSE_EN    83
#define DIRTY_CLUSTER_EN     84
#define DIRTY_SPOTS_MODE_FLT 85
#define DIRTY_SOTA_EN        86
#define DIRTY_HOUND_MODE     87
#define DIRTY_KBD_BIND       95   /* #233 user-defined keyboard shortcuts */

// Bits that actually affect config_io_export()'s output (storage/config_io.c).
// Bookkeeping bits like DIRTY_LAST_TIME (rewritten every FT8 slot by the
// continuous time-sync correction, ~every 15s) and DIRTY_LAST_MODE are NOT in
// here on purpose: re-mirroring qmx-config.txt to the SD card produces an
// identical file (those fields aren't part of the export), so doing it on
// their account is pure waste — and worse, SD card I/O competes with the
// WiFi co-processor's SDIO link for the same shared SDMMC host peripheral
// (see CLAUDE.md "SD-card screenshot save REMOVED"), so an unnecessary
// every-15-seconds SD write was a standing, unintentional trigger for that
// same hazard. Keep this list in sync with config_io_export()'s fields.
static const uint8_t s_config_export_bits[] = {
    DIRTY_DB_MIN, DIRTY_DB_MAX, DIRTY_EMA_ALPHA, DIRTY_IQ_ENABLED,
    DIRTY_FLAT_MODE, DIRTY_WIFI_SSID, DIRTY_WIFI_PASS, DIRTY_CW_PITCH,
    DIRTY_COLORMAP, DIRTY_MY_CALL, DIRTY_MY_GRID, DIRTY_CW_CAL,
    DIRTY_ZOOM, DIRTY_BRIGHTNESS, DIRTY_CQ_MSG0, DIRTY_CQ_MSG1,
    DIRTY_CQ_MSG2, DIRTY_CQ_SEL, DIRTY_ONBOARDED, DIRTY_FT8_FILT,
    DIRTY_WIFI_ENABLED, DIRTY_QMX_GPS, DIRTY_FREQ_KP_CALC,
    DIRTY_QRZ_KEY, DIRTY_EQSL_USER, DIRTY_EQSL_PSWD,
    DIRTY_CL_URL, DIRTY_CL_KEY, DIRTY_CL_STATION,
    DIRTY_WF_BLACK, DIRTY_WF_CONTRAST, DIRTY_WF_BLEND, DIRTY_WF_WINDOW,
    DIRTY_DISP_FLIP, DIRTY_QMX_VOL, DIRTY_CW_AUD_VOL, DIRTY_CHARGE_LIM_EN,
    DIRTY_CHARGE_LIM_PCT,
    DIRTY_LOTW_DXCC, DIRTY_LOTW_CQZ, DIRTY_LOTW_ITUZ, DIRTY_DISP_SLEEP,
    DIRTY_TX_TONE_HZ, DIRTY_TX_TONE_HOLD, DIRTY_CQ_MAX_CALLS,
    DIRTY_SPOTS_EN, DIRTY_RBN_EN, DIRTY_WIFI_KNOWN, DIRTY_CW_TX_OFFSET,
    DIRTY_CQ_LISTEN, DIRTY_SWR_LIMIT, DIRTY_PSK_RX_EN, DIRTY_BT_MOUSE_EN,
    DIRTY_CLUSTER_EN, DIRTY_SOTA_EN, DIRTY_HOUND_MODE,
};

// ---- Module state ------------------------------------------------------

// Known WiFi networks, most-recently-used first. Deliberately outside
// qmx_settings_t / s_pending so the hot settings_load_all() copies do not carry
// it; see settings.h. Loaded in settings_init(), written on DIRTY_WIFI_KNOWN.
static wifi_known_t s_known[WIFI_KNOWN_MAX];
static int          s_known_n = 0;

static bool             s_ready          = false;
static nvs_handle_t     s_nvs            = 0;
static SemaphoreHandle_t s_mutex         = NULL;
static dirty_t          s_dirty          = {0};
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

        dirty_t dirty_local = {0};
        qmx_settings_t snap;
        bool do_flush = false;

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            if (dirty_any(&s_dirty)) {
                TickType_t age = xTaskGetTickCount() - s_last_change_tick;
                if (age >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
                    dirty_local = s_dirty;
                    snap = s_pending;
                    dirty_clear_all(&s_dirty);
                    do_flush = true;
                }
            }
            xSemaphoreGive(s_mutex);
        }

        if (!do_flush) continue;

        // We hold no mutex now — NVS writes can be slow.
        if (dirty_test(&dirty_local, DIRTY_DB_MIN))     nvs_set_float(KEY_DB_MIN,    snap.db_min);
        if (dirty_test(&dirty_local, DIRTY_DB_MAX))     nvs_set_float(KEY_DB_MAX,    snap.db_max);
        if (dirty_test(&dirty_local, DIRTY_EMA_ALPHA))  nvs_set_float(KEY_EMA_ALPHA, snap.ema_alpha);
        if (dirty_test(&dirty_local, DIRTY_IQ_ENABLED)) nvs_set_u8(s_nvs, KEY_IQ_ENABLED, snap.iq_enabled ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_FLAT_MODE))  nvs_set_u8(s_nvs, KEY_FLAT_MODE,  snap.flat_mode    ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_WIFI_SSID))  nvs_set_str(s_nvs, KEY_WIFI_SSID, snap.wifi_ssid);
        if (dirty_test(&dirty_local, DIRTY_MY_CALL))    nvs_set_str(s_nvs, KEY_MY_CALL,   snap.my_callsign);
        if (dirty_test(&dirty_local, DIRTY_MY_GRID))    nvs_set_str(s_nvs, KEY_MY_GRID,   snap.my_grid);
        if (dirty_test(&dirty_local, DIRTY_WIFI_PASS))  nvs_set_str(s_nvs, KEY_WIFI_PASS, snap.wifi_pass);
        if (dirty_test(&dirty_local, DIRTY_LAST_VFO))  nvs_set_u32(s_nvs, KEY_LAST_VFO, snap.last_vfo_hz);
        if (dirty_test(&dirty_local, DIRTY_FT8_FREQ))  nvs_set_u32(s_nvs, KEY_FT8_FREQ, snap.ft8_freq_hz);
        if (dirty_test(&dirty_local, DIRTY_CW_PITCH))  nvs_set_u16(s_nvs, KEY_CW_PITCH, snap.cw_pitch_hz);
        if (dirty_test(&dirty_local, DIRTY_CW_CAL))    nvs_set_i16(s_nvs, KEY_CW_CAL,   snap.cw_cal_hz);
        if (dirty_test(&dirty_local, DIRTY_ZOOM)) {
            uint32_t bits; memcpy(&bits, &snap.zoom_factor, 4);
            nvs_set_u32(s_nvs, KEY_ZOOM, bits);
        }
        if (dirty_test(&dirty_local, DIRTY_COLORMAP))  nvs_set_u8(s_nvs, KEY_COLORMAP, snap.colormap_idx);
        if (dirty_test(&dirty_local, DIRTY_BRIGHTNESS)) nvs_set_u8(s_nvs, KEY_BRIGHTNESS, snap.brightness_pct);
        if (dirty_test(&dirty_local, DIRTY_LAST_MODE))  nvs_set_u8(s_nvs, KEY_LAST_MODE,  snap.last_ui_mode);
        if (dirty_test(&dirty_local, DIRTY_LAST_TIME))  nvs_set_u32(s_nvs, KEY_LAST_TIME, snap.last_unix_time);
        if (dirty_test(&dirty_local, DIRTY_CQ_MSG0))    nvs_set_str(s_nvs, KEY_CQ_MSG0, snap.cq_msg[0]);
        if (dirty_test(&dirty_local, DIRTY_CQ_MSG1))    nvs_set_str(s_nvs, KEY_CQ_MSG1, snap.cq_msg[1]);
        if (dirty_test(&dirty_local, DIRTY_CQ_MSG2))    nvs_set_str(s_nvs, KEY_CQ_MSG2, snap.cq_msg[2]);
        if (dirty_test(&dirty_local, DIRTY_CQ_SEL))     nvs_set_u8(s_nvs, KEY_CQ_SEL, snap.cq_sel);
        if (dirty_test(&dirty_local, DIRTY_CQ_MAX_CALLS)) nvs_set_u8(s_nvs, KEY_CQ_MAX, snap.cq_max_calls);
        if (dirty_test(&dirty_local, DIRTY_HOUND_MODE))   nvs_set_u8(s_nvs, KEY_HOUND_MODE, snap.hound_mode);
        if (dirty_test(&dirty_local, DIRTY_CQ_LISTEN))    nvs_set_u8(s_nvs, KEY_CQ_LISTEN, snap.cq_listen_every);
        if (dirty_test(&dirty_local, DIRTY_ONBOARDED))  nvs_set_u8(s_nvs, KEY_ONBOARDED, snap.onboarded ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_FT8_FILT))     nvs_set_blob(s_nvs, KEY_FT8_FILT, &snap.ft8_filters, sizeof(snap.ft8_filters));
        if (dirty_test(&dirty_local, DIRTY_KBD_BIND))     nvs_set_blob(s_nvs, KEY_KBD_BIND, &snap.kbd_bindings, sizeof(snap.kbd_bindings));
        if (dirty_test(&dirty_local, DIRTY_WIFI_ENABLED)) nvs_set_u8(s_nvs, KEY_WIFI_ENABLED, snap.wifi_enabled ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_QMX_GPS))      nvs_set_u8(s_nvs, KEY_QMX_GPS,      snap.qmx_gps      ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_QMX_TPUSH))    nvs_set_u8(s_nvs, KEY_QMX_TPUSH,    snap.qmx_time_pushed ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_FREQ_KP_CALC)) nvs_set_u8(s_nvs, KEY_FREQ_KP_CALC, snap.freq_kp_calc ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_FREQ_KP_POS)) {
            nvs_set_i16(s_nvs, KEY_FREQ_KP_DX, snap.freq_kp_dx);
            nvs_set_i16(s_nvs, KEY_FREQ_KP_DY, snap.freq_kp_dy);
        }
        if (dirty_test(&dirty_local, DIRTY_FREQ_KP_SMALL)) nvs_set_u8(s_nvs, KEY_FREQ_KP_SMALL, snap.freq_kp_small ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_PASSBAND_HZ))   nvs_set_u32(s_nvs, KEY_PASSBAND_HZ, snap.passband_width_hz);
        if (dirty_test(&dirty_local, DIRTY_QRZ_KEY))      nvs_set_str(s_nvs, KEY_QRZ_KEY, snap.qrz_api_key);
        if (dirty_test(&dirty_local, DIRTY_QRZ_UPLOADED)) nvs_set_u32(s_nvs, KEY_QRZ_UPLOADED, snap.qrz_uploaded_n);
        if (dirty_test(&dirty_local, DIRTY_EQSL_USER))     nvs_set_str(s_nvs, KEY_EQSL_USER, snap.eqsl_user);
        if (dirty_test(&dirty_local, DIRTY_EQSL_PSWD))     nvs_set_str(s_nvs, KEY_EQSL_PSWD, snap.eqsl_pswd);
        if (dirty_test(&dirty_local, DIRTY_EQSL_UPLOADED)) nvs_set_u32(s_nvs, KEY_EQSL_UPLOADED, snap.eqsl_uploaded_n);
        if (dirty_test(&dirty_local, DIRTY_CL_URL))      nvs_set_str(s_nvs, KEY_CL_URL, snap.cloudlog_url);
        if (dirty_test(&dirty_local, DIRTY_CL_KEY))      nvs_set_str(s_nvs, KEY_CL_KEY, snap.cloudlog_key);
        if (dirty_test(&dirty_local, DIRTY_CL_STATION))  nvs_set_str(s_nvs, KEY_CL_STATION, snap.cloudlog_station);
        if (dirty_test(&dirty_local, DIRTY_CL_UPLOADED)) nvs_set_u32(s_nvs, KEY_CL_UPLOADED, snap.cloudlog_uploaded_n);
        if (dirty_test(&dirty_local, DIRTY_CW_AUD_EN))  nvs_set_u8(s_nvs, KEY_CW_AUD_EN,  snap.cw_audio_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_CW_AUD_VOL)) nvs_set_u8(s_nvs, KEY_CW_AUD_VOL, snap.cw_audio_vol);
        if (dirty_test(&dirty_local, DIRTY_WF_BLACK))    nvs_set_float(KEY_WF_BLACK,    snap.wf_black_db);
        if (dirty_test(&dirty_local, DIRTY_WF_CONTRAST)) nvs_set_float(KEY_WF_CONTRAST, snap.wf_contrast_db);
        if (dirty_test(&dirty_local, DIRTY_WF_BLEND))    nvs_set_u8(s_nvs, KEY_WF_BLEND,  snap.wf_floor_blend);
        if (dirty_test(&dirty_local, DIRTY_WF_WINDOW))   nvs_set_u8(s_nvs, KEY_WF_WINDOW, snap.wf_window);
        if (dirty_test(&dirty_local, DIRTY_DISP_FLIP))   nvs_set_u8(s_nvs, KEY_DISP_FLIP, snap.display_flip ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_QMX_VOL))     nvs_set_u8(s_nvs, KEY_QMX_VOL,   snap.qmx_vol_db);
        if (dirty_test(&dirty_local, DIRTY_CW_TX_OFFSET)) nvs_set_i16(s_nvs, KEY_CW_TX_OFF, snap.cw_tx_offset_hz);
        if (dirty_test(&dirty_local, DIRTY_SWR_LIMIT))    nvs_set_u8(s_nvs, KEY_SWR_LIMIT, snap.swr_limit_x10);
        if (dirty_test(&dirty_local, DIRTY_ACTIVATION)) {
            nvs_set_u8(s_nvs, KEY_ACT_TYPE, snap.act_type);
            nvs_set_str(s_nvs, KEY_ACT_REF, snap.act_ref);
        }
        if (dirty_test(&dirty_local, DIRTY_SNAP_PEAK))   nvs_set_u8(s_nvs, KEY_SNAP_PEAK, snap.snap_to_peak ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_BP_REGION))   nvs_set_u8(s_nvs, KEY_BP_REGION, snap.bandplan_region);
        if (dirty_test(&dirty_local, DIRTY_DISTANCE_MILES)) nvs_set_u8(s_nvs, KEY_DISTANCE_MILES, snap.distance_in_miles ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_RIT_PILL_SHOW)) nvs_set_u8(s_nvs, KEY_RIT_PILL_SHOW, snap.rit_pill_show ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_SPUR_SUP))      nvs_set_u8(s_nvs, KEY_SPUR_SUP, snap.spur_mode);
        if (dirty_test(&dirty_local, DIRTY_FT8_EARLY_DEC)) nvs_set_u8(s_nvs, KEY_FT8_EARLY_DEC, snap.ft8_early_decode ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_GREYLIST_EN))   nvs_set_u8(s_nvs, KEY_GREYLIST_EN,   snap.greylist_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_PSKREP_EN))     nvs_set_u8(s_nvs, KEY_PSKREP_EN,     snap.pskreporter_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_PSK_RX_EN))     nvs_set_u8(s_nvs, KEY_PSK_RX_EN,     snap.psk_rx_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_BT_MOUSE_EN))   nvs_set_u8(s_nvs, KEY_BT_MOUSE_EN,   snap.bt_mouse_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_CLUSTER_EN))    nvs_set_u8(s_nvs, KEY_CLUSTER_EN,    snap.cluster_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_SPOTS_MODE_FLT)) nvs_set_u8(s_nvs, KEY_SPOTS_MODE_FLT, snap.spots_mode_filter ? 1 : 0);
    if (dirty_test(&dirty_local, DIRTY_SPOTS_EN))      nvs_set_u8(s_nvs, KEY_SPOTS_EN,      snap.spots_en ? 1 : 0);
    if (dirty_test(&dirty_local, DIRTY_RBN_EN))        nvs_set_u8(s_nvs, KEY_RBN_EN,        snap.rbn_en ? 1 : 0);
    if (dirty_test(&dirty_local, DIRTY_SOTA_EN))       nvs_set_u8(s_nvs, KEY_SOTA_EN,       snap.sota_en ? 1 : 0);
    if (dirty_test(&dirty_local, DIRTY_WIFI_KNOWN)) {
        // Known-network list: not part of s_pending (see settings.h), so take a
        // consistent copy under the mutex before writing it out.
        // STATIC: this runs on the settings_flush task, whose stack is 3 KB, and
        // this array is ~590 bytes. Only that one task reaches this code, so a
        // file-local scratch is safe. (Learned the hard way on 2026-08-05: the
        // stack version crash-looped with a stack-protection fault here AND in
        // the system event task.)
        static wifi_known_t kn[WIFI_KNOWN_MAX];
        uint8_t kn_n;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memcpy(kn, s_known, sizeof(kn));
        kn_n = (uint8_t)s_known_n;
        xSemaphoreGive(s_mutex);
        nvs_set_blob(s_nvs, KEY_WIFI_KNOWN, kn, (size_t)kn_n * sizeof(wifi_known_t));
    }
        if (dirty_test(&dirty_local, DIRTY_TX_TONE_HZ))    nvs_set_u16(s_nvs, KEY_TX_TONE_HZ,   snap.tx_tone_hz);
        if (dirty_test(&dirty_local, DIRTY_TX_TONE_HOLD))  nvs_set_u8(s_nvs, KEY_TX_TONE_HOLD,  snap.tx_tone_hold ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_FT8_SYNC_LINES)) nvs_set_u8(s_nvs, KEY_FT8_SYNC_LINES, snap.ft8_sync_lines ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_FIELD_DAY_EN)) nvs_set_u8(s_nvs, KEY_FIELD_DAY_EN, snap.field_day_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_FD_CLASS))     nvs_set_str(s_nvs, KEY_FD_CLASS, snap.fd_class);
        if (dirty_test(&dirty_local, DIRTY_FD_SECTION))   nvs_set_str(s_nvs, KEY_FD_SECTION, snap.fd_section);
        if (dirty_test(&dirty_local, DIRTY_SIM_MODE))     nvs_set_u8(s_nvs, KEY_SIM_MODE, snap.sim_mode_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_FT8_OP_MODE))  nvs_set_u8(s_nvs, KEY_FT8_OP_MODE, snap.ft8_op_mode);
        if (dirty_test(&dirty_local, DIRTY_CHARGE_LIM_EN))  nvs_set_u8(s_nvs, KEY_CHARGE_LIM_EN,  snap.charge_limit_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_CHARGE_LIM_PCT)) nvs_set_u8(s_nvs, KEY_CHARGE_LIM_PCT, snap.charge_limit_pct);
        if (dirty_test(&dirty_local, DIRTY_RESMON_EN))  nvs_set_u8(s_nvs, KEY_RESMON_EN, snap.resmon_en ? 1 : 0);
        if (dirty_test(&dirty_local, DIRTY_RESMON_POS)) {
            nvs_set_i16(s_nvs, KEY_RESMON_DX, snap.resmon_dx);
            nvs_set_i16(s_nvs, KEY_RESMON_DY, snap.resmon_dy);
        }
        if (dirty_test(&dirty_local, DIRTY_DISP_SLEEP))    nvs_set_u8(s_nvs, KEY_DISP_SLEEP, snap.display_sleep_min);
        // State/county ride DIRTY_LOTW_DXCC: the dirty bitmap is full (bits
        // 0-63 all allocated) and all three are only ever written together, so
        // one bit covers them. Re-writing an unchanged dxcc costs nothing.
        if (dirty_test(&dirty_local, DIRTY_LOTW_DXCC)) {
            nvs_set_str(s_nvs, KEY_LOTW_DXCC,   snap.lotw_dxcc);
            nvs_set_str(s_nvs, KEY_LOTW_STATE,  snap.lotw_state);
            nvs_set_str(s_nvs, KEY_LOTW_COUNTY, snap.lotw_county);
        }
        if (dirty_test(&dirty_local, DIRTY_LOTW_CQZ))      nvs_set_str(s_nvs, KEY_LOTW_CQZ,  snap.lotw_cqz);
        if (dirty_test(&dirty_local, DIRTY_LOTW_ITUZ))     nvs_set_str(s_nvs, KEY_LOTW_ITUZ, snap.lotw_ituz);
        if (dirty_test(&dirty_local, DIRTY_LOTW_UPLOADED)) nvs_set_u32(s_nvs, KEY_LOTW_UPLOADED, snap.lotw_uploaded_n);

        esp_err_t err = nvs_commit(s_nvs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_commit failed: 0x%x", err);
        } else {
            ESP_LOGI(TAG, "flushed dirty=%08lx%08lx%08lx%08lx",
                     (unsigned long)dirty_local.w[3], (unsigned long)dirty_local.w[2],
                     (unsigned long)dirty_local.w[1], (unsigned long)dirty_local.w[0]);
            // Only re-mirror to SD if something that's actually IN the
            // exported file changed — see s_config_export_bits[] above.
            if (dirty_test_any(&dirty_local, s_config_export_bits,
                               sizeof(s_config_export_bits))) {
                sd_archive_mark_config_dirty();
            }
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
    out->ft8_freq_hz = 14074000;   // 20m FT8 — sane default so FT8 never opens on an inherited odd VFO
    out->cw_pitch_hz = DEF_CW_PITCH;
    out->cw_cal_hz   = DEF_CW_CAL;
    out->rit_pill_show = true;   // opt-OUT, so it must be set here and not left zeroed
    // Spur suppression is opt-IN for now: it nudges the dial 25 Hz to learn a
    // frequency, which is a visible side effect, and it is out with the beta
    // testers before it can be a default.
    out->spur_mode = 0;
    out->zoom_factor = DEF_ZOOM;
    out->colormap_idx = DEF_COLORMAP;
    out->brightness_pct = DEF_BRIGHTNESS;
    out->last_ui_mode = DEF_LAST_MODE;
    out->last_unix_time = 0;
    out->cq_msg[0][0] = '\0';
    out->cq_msg[1][0] = '\0';
    out->cq_msg[2][0] = '\0';
    out->cq_sel = 0;
    out->cq_max_calls = 0;
    out->hound_mode   = 0;   // off: Hound changes TX behaviour, so it is opt-in
    out->cq_listen_every = 0;
    out->onboarded = false;
    out->wifi_enabled = DEF_WIFI_ENABLED;
    out->qmx_gps = false;
    out->qmx_time_pushed = false;
    out->freq_kp_calc = false;
    out->freq_kp_dx = 0;
    out->freq_kp_dy = 0;
    out->freq_kp_small = false;
    out->passband_width_hz = 0;
    out->qrz_api_key[0] = '\0';
    out->qrz_uploaded_n = 0;
    out->eqsl_user[0] = '\0';
    out->eqsl_pswd[0] = '\0';
    out->eqsl_uploaded_n = 0;
    out->cloudlog_url[0] = '\0';
    out->cloudlog_key[0] = '\0';
    out->cloudlog_station[0] = '\0';
    out->cloudlog_uploaded_n = 0;
    out->cw_audio_en  = DEF_CW_AUD_EN;
    out->cw_audio_vol = DEF_CW_AUD_VOL;
    out->wf_black_db    = DEF_WF_BLACK;
    out->wf_contrast_db = DEF_WF_CONTRAST;
    out->wf_floor_blend = DEF_WF_BLEND;
    out->wf_window      = DEF_WF_WINDOW;
    out->display_flip   = false;
    out->qmx_vol_db     = 20;   // fallback slider position only - never sent at boot
    out->snap_to_peak   = true;   // on by default (legacy behaviour)
    out->ft8_early_decode = true; // on by default (WSJT-X-style fast pounce timing)
    out->greylist_en = false;     // opt-in ("Allow grey-listing", Filter modal)
    // ON by default: contributing reception reports is the norm for FT8
    // software (WSJT-X ships PSK Reporter spotting enabled) and the data is
    // inherently public ham activity. Disclosed in the release notes + manual;
    // the FT8 drawer checkbox turns it off. Inert until callsign+grid are set.
    out->pskreporter_en = true;
    out->psk_rx_en      = false;   // opt-in, see settings.h
    out->bt_mouse_en    = false;   // opt-in, see settings.h
    out->cluster_en     = false;   // opt-in, see settings.h
    out->spots_mode_filter = true;  // ON by default, see settings.h
    // Spots on by default: it is read-only use of a public API, and a feature
    // that draws on the spectrum has to be visible to be discovered. Costs
    // nothing until WiFi is up.
    out->spots_en = true;
    out->rbn_en   = false;   // opt-in: a continuous telnet firehose on a fragile link
    out->sota_en  = false;   // opt-in: somebody else's hobby server, see settings.h
    out->tx_tone_hz   = 1500;     // conventional FT8 default; = FT8_TX_CQ_DEFAULT_FREQ_HZ
    out->tx_tone_hold = false;    // auto-pick a clear slot, as it always did
    out->bandplan_region = 0;     // 0 = auto (derive from grid)
    // SWR protection ON by default at 3.0:1. The QMX has no SWR foldback of
    // its own on a digital burst, and an FT8 transmission is 12.7 s of key-down
    // into whatever is connected - a disconnected or wrong-band antenna is the
    // normal way this goes wrong in the field. 3.0 is high enough not to trip
    // on a merely mediocre match; the drawer can raise it or turn it off.
    out->swr_limit_x10 = 30;
    out->act_type   = 0;          // not activating anything
    out->act_ref[0] = '\0';
    memset(&out->ft8_filters, 0, sizeof(out->ft8_filters));
    // n = 0 means "never configured", which ui.c takes as "use the built-in
    // defaults" rather than "the operator deleted every shortcut".
    memset(&out->kbd_bindings, 0, sizeof(out->kbd_bindings));
    out->field_day_en = false;
    out->fd_class[0]  = '\0';
    out->fd_section[0] = '\0';
    out->sim_mode_en = false;
    out->ft8_op_mode = 0;     // FT8
    out->charge_limit_en  = DEF_CHARGE_LIM_EN;
    out->charge_limit_pct = DEF_CHARGE_LIM_PCT;
    out->resmon_en = false;
    out->resmon_dx = 0;
    out->resmon_dy = 0;
    out->display_sleep_min = 0;
    out->lotw_dxcc[0] = '\0';
    out->lotw_cqz[0] = '\0';
    out->lotw_ituz[0] = '\0';
    out->lotw_state[0] = '\0';
    out->lotw_county[0] = '\0';
    out->lotw_uploaded_n = 0;

    if (!s_ready) {
        ESP_LOGW(TAG, "load_all: NVS not ready, using defaults");
        return;
    }

    float fv;
    uint8_t u8v;
    uint16_t u16v;
    if (nvs_get_float(KEY_DB_MIN,    &fv)) out->db_min    = fv;
    if (nvs_get_float(KEY_DB_MAX,    &fv)) out->db_max    = fv;
    if (nvs_get_float(KEY_EMA_ALPHA, &fv)) out->ema_alpha = fv;
    if (nvs_get_u8(s_nvs, KEY_IQ_ENABLED, &u8v) == ESP_OK) out->iq_enabled = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_FLAT_MODE,  &u8v) == ESP_OK) out->flat_mode  = (u8v != 0);
    nvs_get_u32(s_nvs, KEY_LAST_VFO, &out->last_vfo_hz);
    nvs_get_u32(s_nvs, KEY_FT8_FREQ, &out->ft8_freq_hz);
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
    nvs_get_u8(s_nvs, KEY_CQ_MAX, &out->cq_max_calls);
    nvs_get_u8(s_nvs, KEY_HOUND_MODE, &out->hound_mode);
    nvs_get_u8(s_nvs, KEY_CQ_LISTEN, &out->cq_listen_every);
    nvs_get_u8(s_nvs, KEY_SWR_LIMIT, &out->swr_limit_x10);
    nvs_get_u8(s_nvs, KEY_ACT_TYPE, &out->act_type);
    out->act_ref[0] = '\0';
    sz = sizeof(out->act_ref);
    nvs_get_str(s_nvs, KEY_ACT_REF, out->act_ref, &sz);
    if (!out->act_ref[0]) out->act_type = 0;   // a reference-less activation is none

    if (nvs_get_u8(s_nvs, KEY_ONBOARDED,  &u8v) == ESP_OK) out->onboarded  = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_WIFI_ENABLED, &u8v) == ESP_OK) out->wifi_enabled = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_QMX_GPS,      &u8v) == ESP_OK) out->qmx_gps      = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_QMX_TPUSH,    &u8v) == ESP_OK) out->qmx_time_pushed = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_FREQ_KP_CALC, &u8v) == ESP_OK) out->freq_kp_calc = (u8v != 0);
    {
        int16_t i16v;
        if (nvs_get_i16(s_nvs, KEY_FREQ_KP_DX, &i16v) == ESP_OK) out->freq_kp_dx = i16v;
        if (nvs_get_i16(s_nvs, KEY_FREQ_KP_DY, &i16v) == ESP_OK) out->freq_kp_dy = i16v;
    }
    if (nvs_get_u8(s_nvs, KEY_FREQ_KP_SMALL, &u8v) == ESP_OK) out->freq_kp_small = (u8v != 0);
    {
        uint32_t u32v;
        if (nvs_get_u32(s_nvs, KEY_PASSBAND_HZ, &u32v) == ESP_OK) out->passband_width_hz = u32v;
    }
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
    out->cloudlog_url[0] = '\0';
    sz = sizeof(out->cloudlog_url);
    nvs_get_str(s_nvs, KEY_CL_URL, out->cloudlog_url, &sz);
    out->cloudlog_key[0] = '\0';
    sz = sizeof(out->cloudlog_key);
    nvs_get_str(s_nvs, KEY_CL_KEY, out->cloudlog_key, &sz);
    out->cloudlog_station[0] = '\0';
    sz = sizeof(out->cloudlog_station);
    nvs_get_str(s_nvs, KEY_CL_STATION, out->cloudlog_station, &sz);
    nvs_get_u32(s_nvs, KEY_CL_UPLOADED, &out->cloudlog_uploaded_n);

    if (nvs_get_u8(s_nvs, KEY_CW_AUD_EN, &u8v) == ESP_OK) out->cw_audio_en = (u8v != 0);
    nvs_get_u8(s_nvs, KEY_CW_AUD_VOL, &out->cw_audio_vol);

    if (nvs_get_float(KEY_WF_BLACK,    &fv)) out->wf_black_db    = fv;
    if (nvs_get_float(KEY_WF_CONTRAST, &fv)) out->wf_contrast_db = fv;
    nvs_get_u8(s_nvs, KEY_WF_BLEND,  &out->wf_floor_blend);
    nvs_get_u8(s_nvs, KEY_WF_WINDOW, &out->wf_window);
    if (out->wf_floor_blend > 100) out->wf_floor_blend = 100;
    if (out->wf_window > 2)        out->wf_window = 0;
    if (nvs_get_u8(s_nvs, KEY_DISP_FLIP, &u8v) == ESP_OK) out->display_flip = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_QMX_VOL, &u8v) == ESP_OK) out->qmx_vol_db = (u8v <= 199) ? u8v : 199;
    {
        int16_t i16v;
        if (nvs_get_i16(s_nvs, KEY_CW_TX_OFF, &i16v) == ESP_OK) {
            if (i16v >  1000) i16v =  1000;
            if (i16v < -1000) i16v = -1000;
            out->cw_tx_offset_hz = i16v;
        }
    }
    if (nvs_get_u8(s_nvs, KEY_SNAP_PEAK, &u8v) == ESP_OK) out->snap_to_peak = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_BP_REGION, &u8v) == ESP_OK) out->bandplan_region = (u8v <= 3) ? u8v : 0;
    if (nvs_get_u8(s_nvs, KEY_DISTANCE_MILES, &u8v) == ESP_OK) out->distance_in_miles = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_RIT_PILL_SHOW, &u8v) == ESP_OK) out->rit_pill_show = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_SPUR_SUP, &u8v) == ESP_OK) out->spur_mode = (u8v <= 2) ? u8v : 0;
    if (nvs_get_u8(s_nvs, KEY_FT8_EARLY_DEC, &u8v) == ESP_OK) out->ft8_early_decode = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_GREYLIST_EN, &u8v) == ESP_OK) out->greylist_en = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_PSKREP_EN, &u8v) == ESP_OK) out->pskreporter_en = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_PSK_RX_EN, &u8v) == ESP_OK) out->psk_rx_en = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_BT_MOUSE_EN, &u8v) == ESP_OK) out->bt_mouse_en = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_CLUSTER_EN, &u8v) == ESP_OK) out->cluster_en = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_SPOTS_MODE_FLT, &u8v) == ESP_OK) out->spots_mode_filter = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_SPOTS_EN, &u8v) == ESP_OK) out->spots_en = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_RBN_EN,   &u8v) == ESP_OK) out->rbn_en   = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_SOTA_EN,  &u8v) == ESP_OK) out->sota_en  = (u8v != 0);
    if (nvs_get_u16(s_nvs, KEY_TX_TONE_HZ, &u16v) == ESP_OK) out->tx_tone_hz = u16v;
    if (nvs_get_u8(s_nvs, KEY_TX_TONE_HOLD, &u8v) == ESP_OK) out->tx_tone_hold = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_FT8_SYNC_LINES, &u8v) == ESP_OK) out->ft8_sync_lines = (u8v != 0);

    sz = sizeof(out->ft8_filters);
    nvs_get_blob(s_nvs, KEY_FT8_FILT, &out->ft8_filters, &sz);
    sz = sizeof(out->kbd_bindings);
    nvs_get_blob(s_nvs, KEY_KBD_BIND, &out->kbd_bindings, &sz);
    if (out->kbd_bindings.n > KBD_BINDINGS_MAX) out->kbd_bindings.n = 0;  /* corrupt/older blob */

    // Known-network list. Stored as a blob of exactly the used entries, so the
    // returned size gives the count back. A short/absent blob just means "none
    // remembered yet" - never an error worth reporting.
    {
        size_t ksz = sizeof(s_known);
        memset(s_known, 0, sizeof(s_known));
        s_known_n = 0;
        if (nvs_get_blob(s_nvs, KEY_WIFI_KNOWN, s_known, &ksz) == ESP_OK) {
            int n = (int)(ksz / sizeof(wifi_known_t));
            if (n > WIFI_KNOWN_MAX) n = WIFI_KNOWN_MAX;
            // Drop anything with an empty SSID: a truncated or hand-edited blob
            // must not leave a blank entry that the roam scan would try to match.
            for (int i = 0; i < n; i++)
                if (s_known[i].ssid[0]) s_known[s_known_n++] = s_known[i];
        }
    }

    if (nvs_get_u8(s_nvs, KEY_FIELD_DAY_EN, &u8v) == ESP_OK) out->field_day_en = (u8v != 0);
    out->fd_class[0] = '\0';
    sz = sizeof(out->fd_class);
    nvs_get_str(s_nvs, KEY_FD_CLASS, out->fd_class, &sz);
    out->fd_section[0] = '\0';
    sz = sizeof(out->fd_section);
    nvs_get_str(s_nvs, KEY_FD_SECTION, out->fd_section, &sz);

    if (nvs_get_u8(s_nvs, KEY_SIM_MODE, &u8v) == ESP_OK) out->sim_mode_en = (u8v != 0);
    if (nvs_get_u8(s_nvs, KEY_FT8_OP_MODE, &u8v) == ESP_OK) out->ft8_op_mode = u8v;
    if (nvs_get_u8(s_nvs, KEY_CHARGE_LIM_EN, &u8v) == ESP_OK) out->charge_limit_en = (u8v != 0);
    nvs_get_u8(s_nvs, KEY_CHARGE_LIM_PCT, &out->charge_limit_pct);
    if (out->charge_limit_pct < 50 || out->charge_limit_pct > 100) out->charge_limit_pct = DEF_CHARGE_LIM_PCT;
    if (nvs_get_u8(s_nvs, KEY_RESMON_EN, &u8v) == ESP_OK) out->resmon_en = (u8v != 0);
    nvs_get_i16(s_nvs, KEY_RESMON_DX, &out->resmon_dx);
    nvs_get_i16(s_nvs, KEY_RESMON_DY, &out->resmon_dy);
    if (nvs_get_u8(s_nvs, KEY_DISP_SLEEP, &u8v) == ESP_OK) out->display_sleep_min = u8v;
    sz = sizeof(out->lotw_dxcc);
    nvs_get_str(s_nvs, KEY_LOTW_DXCC, out->lotw_dxcc, &sz);
    sz = sizeof(out->lotw_cqz);
    nvs_get_str(s_nvs, KEY_LOTW_CQZ, out->lotw_cqz, &sz);
    sz = sizeof(out->lotw_ituz);
    nvs_get_str(s_nvs, KEY_LOTW_ITUZ, out->lotw_ituz, &sz);
    sz = sizeof(out->lotw_state);
    nvs_get_str(s_nvs, KEY_LOTW_STATE, out->lotw_state, &sz);
    sz = sizeof(out->lotw_county);
    nvs_get_str(s_nvs, KEY_LOTW_COUNTY, out->lotw_county, &sz);
    nvs_get_u32(s_nvs, KEY_LOTW_UPLOADED, &out->lotw_uploaded_n);

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

static void mark_dirty(int bit)
{
    if (!s_ready) return;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        dirty_set(&s_dirty, bit);
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

void settings_set_ft8_freq_hz(uint32_t hz)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.ft8_freq_hz == hz) {
        xSemaphoreGive(s_mutex);
        return;  // unchanged, skip the dirty/flush cycle
    }
    s_pending.ft8_freq_hz = hz;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FT8_FREQ);
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
    dirty_clear_bit(&s_dirty, DIRTY_LAST_MODE);  // written synchronously below; nothing left for flush_task
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

void settings_set_cq_max_calls(uint8_t n)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.cq_max_calls == n) { xSemaphoreGive(s_mutex); return; }
    s_pending.cq_max_calls = n;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CQ_MAX_CALLS);
}

void settings_set_hound_mode(uint8_t m)
{
    if (!s_ready) return;
    if (m > 2) m = 2;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.hound_mode == m) { xSemaphoreGive(s_mutex); return; }
    s_pending.hound_mode = m;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_HOUND_MODE);
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

void settings_set_kbd_bindings(const kbd_bindings_t *b)
{
    if (!s_ready || !b) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.kbd_bindings = *b;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_KBD_BIND);
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

void settings_set_qmx_time_pushed(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending.qmx_time_pushed = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_QMX_TPUSH);
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

void settings_set_freq_kp_pos(int16_t dx, int16_t dy)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.freq_kp_dx == dx && s_pending.freq_kp_dy == dy) { xSemaphoreGive(s_mutex); return; }
    s_pending.freq_kp_dx = dx;
    s_pending.freq_kp_dy = dy;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FREQ_KP_POS);
}

void settings_set_freq_kp_small(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.freq_kp_small == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.freq_kp_small = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FREQ_KP_SMALL);
}

void settings_set_passband_width_hz(uint32_t hz)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.passband_width_hz == hz) { xSemaphoreGive(s_mutex); return; }
    s_pending.passband_width_hz = hz;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_PASSBAND_HZ);
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

/* ---- Cloudlog / Wavelog (#171) --------------------------------------------
 * The URL is the operator's own server, so unlike every other upload target it
 * is stored rather than compiled in. util/net_guard.c decides, per upload,
 * whether that address may be spoken to in the clear. */
void settings_set_cloudlog_url(const char *url)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (url) {
        strncpy(s_pending.cloudlog_url, url, sizeof(s_pending.cloudlog_url) - 1);
        s_pending.cloudlog_url[sizeof(s_pending.cloudlog_url) - 1] = '\0';
    } else {
        s_pending.cloudlog_url[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CL_URL);
}

void settings_set_cloudlog_key(const char *key)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (key) {
        strncpy(s_pending.cloudlog_key, key, sizeof(s_pending.cloudlog_key) - 1);
        s_pending.cloudlog_key[sizeof(s_pending.cloudlog_key) - 1] = '\0';
    } else {
        s_pending.cloudlog_key[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CL_KEY);
}

void settings_set_cloudlog_station(const char *station_id)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (station_id) {
        strncpy(s_pending.cloudlog_station, station_id, sizeof(s_pending.cloudlog_station) - 1);
        s_pending.cloudlog_station[sizeof(s_pending.cloudlog_station) - 1] = '\0';
    } else {
        s_pending.cloudlog_station[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CL_STATION);
}

void settings_set_cloudlog_uploaded_n(uint32_t n)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.cloudlog_uploaded_n == n) { xSemaphoreGive(s_mutex); return; }
    s_pending.cloudlog_uploaded_n = n;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CL_UPLOADED);
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

void settings_set_qmx_vol_db(uint8_t db)
{
    if (!s_ready) return;
    if (db > 199) db = 199;   // CAT_AF_GAIN_DB_MAX; not including cat.h here
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.qmx_vol_db == db) { xSemaphoreGive(s_mutex); return; }
    s_pending.qmx_vol_db = db;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_QMX_VOL);
}

void settings_set_cw_tx_offset_hz(int16_t hz)
{
    if (!s_ready) return;
    if (hz >  1000) hz =  1000;
    if (hz < -1000) hz = -1000;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.cw_tx_offset_hz == hz) { xSemaphoreGive(s_mutex); return; }
    s_pending.cw_tx_offset_hz = hz;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CW_TX_OFFSET);
}

void settings_set_cq_listen_every(uint8_t n)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.cq_listen_every == n) { xSemaphoreGive(s_mutex); return; }
    s_pending.cq_listen_every = n;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CQ_LISTEN);
}

void settings_set_cluster_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.cluster_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.cluster_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CLUSTER_EN);
}

void settings_set_spots_mode_filter(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.spots_mode_filter == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.spots_mode_filter = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_SPOTS_MODE_FLT);
}

void settings_set_bt_mouse_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.bt_mouse_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.bt_mouse_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_BT_MOUSE_EN);
}

void settings_set_psk_rx_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.psk_rx_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.psk_rx_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_PSK_RX_EN);
}

void settings_set_activation(uint8_t type, const char *ref)
{
    if (!s_ready) return;
    char clean[16];
    clean[0] = '\0';
    if (ref) {
        // Trim and upper-case: references are case-insensitive in both schemes
        // but the log should carry the canonical form, and an operator typing
        // on glass in a field leaves stray spaces.
        while (*ref == ' ') ref++;
        size_t n = 0;
        while (*ref && n < sizeof(clean) - 1) {
            char c = *ref++;
            clean[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        while (n > 0 && clean[n - 1] == ' ') n--;
        clean[n] = '\0';
    }
    if (type > 2 || !clean[0]) { type = 0; clean[0] = '\0'; }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool same = (s_pending.act_type == type) && (strcmp(s_pending.act_ref, clean) == 0);
    if (same) { xSemaphoreGive(s_mutex); return; }
    s_pending.act_type = type;
    strncpy(s_pending.act_ref, clean, sizeof(s_pending.act_ref) - 1);
    s_pending.act_ref[sizeof(s_pending.act_ref) - 1] = '\0';
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_ACTIVATION);
}

uint8_t settings_get_activation_type(void)
{
    if (!s_ready) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t t = s_pending.act_type;
    bool has_ref = s_pending.act_ref[0] != '\0';
    xSemaphoreGive(s_mutex);
    return has_ref ? t : 0;
}

bool settings_get_activation_ref(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    if (!s_ready) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool on = (s_pending.act_type != 0) && (s_pending.act_ref[0] != '\0');
    if (on) snprintf(out, out_sz, "%s", s_pending.act_ref);
    xSemaphoreGive(s_mutex);
    return on;
}

const char *settings_activation_sig_name(void)
{
    switch (settings_get_activation_type()) {
        case 1:  return "POTA";
        case 2:  return "SOTA";
        default: return NULL;
    }
}

void settings_set_swr_limit_x10(uint8_t v)
{
    if (!s_ready) return;
    if (v != 0 && v < 15) v = 15;    // below 1.5:1 nothing real would ever pass
    if (v > 99) v = 99;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.swr_limit_x10 == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.swr_limit_x10 = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_SWR_LIMIT);
}

uint8_t settings_get_swr_limit_x10(void)
{
    if (!s_ready) return 30;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t v = s_pending.swr_limit_x10;
    xSemaphoreGive(s_mutex);
    return v;
}

int16_t settings_get_cw_tx_offset_hz(void)
{
    if (!s_ready) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int16_t v = s_pending.cw_tx_offset_hz;
    xSemaphoreGive(s_mutex);
    return v;
}

// Default ON: the RIT pill is a v1.8.0 feature and hiding it by default would make
// it invisible to everyone who never opens the drawer. This is opt-OUT, for
// operators who do not use RIT and do not want the top-right corner spent on it
// (Samuel W7STF).
void settings_set_rit_pill_show(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.rit_pill_show == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.rit_pill_show = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_RIT_PILL_SHOW);
}

// Opt-IN. See spur_map.h: enabling it lets the firmware nudge the dial 25 Hz
// when it meets a frequency it has not learned yet, which is a real (if brief)
// side effect on the operator's radio - not something to switch on for people
// without asking.
void settings_set_spur_mode(uint8_t v)
{
    if (!s_ready) return;
    if (v > 2) v = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.spur_mode == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.spur_mode = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_SPUR_SUP);
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

void settings_set_ft8_early_decode(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.ft8_early_decode == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.ft8_early_decode = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_FT8_EARLY_DEC);
}

void settings_set_greylist_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.greylist_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.greylist_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_GREYLIST_EN);
}

void settings_set_pskreporter_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.pskreporter_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.pskreporter_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_PSKREP_EN);
}

void settings_set_spots_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.spots_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.spots_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_SPOTS_EN);
}

void settings_set_rbn_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.rbn_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.rbn_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_RBN_EN);
}

void settings_set_sota_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.sota_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.sota_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_SOTA_EN);
}

// ---- Known WiFi networks ---------------------------------------------------
//
// Kept in its own small array rather than in qmx_settings_t, so the hot
// settings_load_all() copies do not have to carry it (see settings.h). Loaded
// once in settings_init(), persisted through the normal dirty/flush path.

int settings_wifi_known_count(void)
{
    if (!s_ready) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_known_n;
    xSemaphoreGive(s_mutex);
    return n;
}

int settings_wifi_known_get(wifi_known_t *out, int max)
{
    if (!out || max <= 0 || !s_ready) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_known_n < max ? s_known_n : max;
    memcpy(out, s_known, (size_t)n * sizeof(wifi_known_t));
    xSemaphoreGive(s_mutex);
    return n;
}

void settings_wifi_known_remember(const char *ssid, const char *pass)
{
    if (!s_ready || !ssid || !ssid[0]) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Already known? Move it to the front, refreshing the password in case it
    // changed. Otherwise insert at the front and push the rest down, dropping
    // the least-recently-used entry when the list is full.
    int at = -1;
    for (int i = 0; i < s_known_n; i++)
        if (strcmp(s_known[i].ssid, ssid) == 0) { at = i; break; }

    bool changed = false;
    if (at == 0) {
        // Front already: only a password change is worth a write.
        if (strcmp(s_known[0].pass, pass ? pass : "") != 0) {
            snprintf(s_known[0].pass, sizeof(s_known[0].pass), "%s", pass ? pass : "");
            changed = true;
        }
    } else {
        int from = (at > 0) ? at : (s_known_n < WIFI_KNOWN_MAX ? s_known_n : WIFI_KNOWN_MAX - 1);
        for (int i = from; i > 0; i--) s_known[i] = s_known[i - 1];
        snprintf(s_known[0].ssid, sizeof(s_known[0].ssid), "%s", ssid);
        snprintf(s_known[0].pass, sizeof(s_known[0].pass), "%s", pass ? pass : "");
        if (at < 0 && s_known_n < WIFI_KNOWN_MAX) s_known_n++;
        changed = true;
    }
    xSemaphoreGive(s_mutex);
    if (changed) mark_dirty(DIRTY_WIFI_KNOWN);
}

void settings_wifi_known_set_all(const wifi_known_t *list, int n)
{
    if (!s_ready) return;
    if (n < 0) n = 0;
    if (n > WIFI_KNOWN_MAX) n = WIFI_KNOWN_MAX;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_known, 0, sizeof(s_known));
    s_known_n = 0;
    for (int i = 0; i < n && list; i++) {
        if (!list[i].ssid[0]) continue;      // skip blanks from a hand-edited file
        s_known[s_known_n++] = list[i];
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_WIFI_KNOWN);
}

void settings_wifi_known_forget(const char *ssid)
{
    if (!s_ready || !ssid || !ssid[0]) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = false;
    for (int i = 0; i < s_known_n; i++) {
        if (strcmp(s_known[i].ssid, ssid) != 0) continue;
        for (int j = i; j < s_known_n - 1; j++) s_known[j] = s_known[j + 1];
        memset(&s_known[--s_known_n], 0, sizeof(s_known[0]));
        changed = true;
        break;
    }
    xSemaphoreGive(s_mutex);
    if (changed) mark_dirty(DIRTY_WIFI_KNOWN);
}

void settings_wifi_known_clear(void)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_known, 0, sizeof(s_known));
    s_known_n = 0;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_WIFI_KNOWN);
}

void settings_set_tx_tone_hz(uint16_t v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.tx_tone_hz == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.tx_tone_hz = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_TX_TONE_HZ);
}

void settings_set_tx_tone_hold(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.tx_tone_hold == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.tx_tone_hold = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_TX_TONE_HOLD);
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

void settings_set_charge_limit_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.charge_limit_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.charge_limit_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CHARGE_LIM_EN);
}

void settings_set_charge_limit_pct(uint8_t pct)
{
    if (!s_ready) return;
    if (pct < 50) pct = 50;
    if (pct > 100) pct = 100;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.charge_limit_pct == pct) { xSemaphoreGive(s_mutex); return; }
    s_pending.charge_limit_pct = pct;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_CHARGE_LIM_PCT);
}

void settings_set_resmon_en(bool v)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.resmon_en == v) { xSemaphoreGive(s_mutex); return; }
    s_pending.resmon_en = v;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_RESMON_EN);
}

void settings_set_resmon_pos(int16_t dx, int16_t dy)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.resmon_dx == dx && s_pending.resmon_dy == dy) { xSemaphoreGive(s_mutex); return; }
    s_pending.resmon_dx = dx;
    s_pending.resmon_dy = dy;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_RESMON_POS);
}

void settings_set_display_sleep_min(uint8_t minutes)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.display_sleep_min == minutes) { xSemaphoreGive(s_mutex); return; }
    s_pending.display_sleep_min = minutes;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_DISP_SLEEP);
}

static void set_lotw_str(char *dst, size_t dst_sz, const char *v, uint64_t bit)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (v) {
        strncpy(dst, v, dst_sz - 1);
        dst[dst_sz - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
    mark_dirty(bit);
}

void settings_set_lotw_dxcc(const char *dxcc)
{
    set_lotw_str(s_pending.lotw_dxcc, sizeof(s_pending.lotw_dxcc), dxcc, DIRTY_LOTW_DXCC);
}

void settings_set_lotw_cqz(const char *cqz)
{
    set_lotw_str(s_pending.lotw_cqz, sizeof(s_pending.lotw_cqz), cqz, DIRTY_LOTW_CQZ);
}

void settings_set_lotw_ituz(const char *ituz)
{
    set_lotw_str(s_pending.lotw_ituz, sizeof(s_pending.lotw_ituz), ituz, DIRTY_LOTW_ITUZ);
}

// Both share DIRTY_LOTW_DXCC - see the flush block and settings.h.
void settings_set_lotw_state(const char *state)
{
    set_lotw_str(s_pending.lotw_state, sizeof(s_pending.lotw_state), state, DIRTY_LOTW_DXCC);
}

void settings_set_lotw_county(const char *county)
{
    set_lotw_str(s_pending.lotw_county, sizeof(s_pending.lotw_county), county, DIRTY_LOTW_DXCC);
}

void settings_set_lotw_uploaded_n(uint32_t n)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending.lotw_uploaded_n == n) { xSemaphoreGive(s_mutex); return; }
    s_pending.lotw_uploaded_n = n;
    xSemaphoreGive(s_mutex);
    mark_dirty(DIRTY_LOTW_UPLOADED);
}
