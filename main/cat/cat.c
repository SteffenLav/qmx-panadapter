#include "cat.h"

#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "bsp/m5stack_tab5.h"

#include "ui.h"
#include "diag_log.h"

static const char *TAG = "cat";

#define QMX_VID  0x0483
#define QMX_PID  0xA34C
#define CAT_BAUD_RATE 38400
#define CAT_POLL_INTERVAL_MS 50   // Phase 5.10H: was 200 -> 100 -> 50 (FA every 150 ms)
#define CAT_RX_BUFFER_SIZE 128

#define EVT_DEV_CONNECTED  BIT0
#define EVT_DEV_GONE       BIT1

// USB Audio Class descriptor sub-types we care about
#define USB_CLASS_AUDIO              0x01
#define USB_SUBCLASS_AUDIOCONTROL    0x01
#define USB_SUBCLASS_AUDIOSTREAMING  0x02
#define USB_DESC_TYPE_CS_INTERFACE   0x24
#define USB_DESC_TYPE_CS_ENDPOINT    0x25
#define UAC_AS_GENERAL               0x01
#define UAC_AS_FORMAT_TYPE           0x02
#define UAC_FORMAT_TYPE_I            0x01

static TaskHandle_t s_poll_task = NULL;
static EventGroupHandle_t s_evt_group = NULL;
static cdc_acm_dev_hdl_t s_cdc_dev = NULL;
static volatile bool s_cat_ready = false;

bool cat_is_ready(void)
{
    return s_cat_ready;
}
static bool s_audio_dumped = false;

static char s_rx_buf[CAT_RX_BUFFER_SIZE];
static size_t s_rx_len = 0;
static char   s_mm_resp[64] = {0};  // last MM response, set by process_cat_message
static size_t s_mm_resp_len = 0;
static char   s_tm_resp[16] = {0};  // last TM response, set by process_cat_message
static size_t s_tm_resp_len = 0;
static volatile int64_t s_tm_resp_us = 0;  // esp_timer time the TM response landed (GPS-tick sync)
static char   s_pc_resp[16] = {0};  // last PC (power output) response
static size_t s_pc_resp_len = 0;
static char   s_sw_resp[16] = {0};  // last SW (SWR) response
static size_t s_sw_resp_len = 0;
static char   s_qmx_fw[24] = {0};   // QMX firmware version from VN; (e.g. "1_03_002QMX")
static char   s_q9_resp[16] = {0};  // last Q9 (IQ mode) response, e.g. "Q91;"
static size_t s_q9_resp_len = 0;
static volatile bool s_iq_mode_confirmed = false;  // true once Q9; readback confirms IQ mode ON
static char   s_q3_resp[16] = {0};  // last Q3 (VOX enable) response, e.g. "Q30;"
static size_t s_q3_resp_len = 0;
static volatile bool s_vox_disabled = false;  // true once Q3; readback confirms VOX OFF
static uint64_t s_diag_poll_hb_us = 0;  // last diag poll-heartbeat timestamp

static uint32_t s_last_freq_hz = 0;
static char s_last_mode_digit = 0;  // Phase 5.10: cached Kenwood mode digit
static int  s_cw_offset_hz = 700;   // CW LO offset read from QMX at connect, default 700
static cat_band_entry_t s_band_list[CAT_MAX_BANDS];
static int              s_band_count = 0;

const cat_band_entry_t *cat_get_band_list(int *out_count)
{
    if (out_count) *out_count = s_band_count;
    return s_band_list;
}
static uint64_t s_last_tx_us = 0;   // for rate-limiting cat_set_frequency
static volatile bool s_poll_paused = false;  // v0.12.0: cooperative pause for FT8 TX bursts

// Pending mode digit (Kenwood MD digit '1'-'9') requested from the LVGL thread.
// 0 = nothing pending. Drained by the poll task to avoid a CDC race.
static volatile char s_pending_mode_digit = 0;
static char hamlib_mode_to_digit(const char *mode);  // forward declaration

// Pending SSB filter bandwidth (Hz) requested from the LVGL thread. The poll
// task drains it on its next cycle so the write happens on the one thread that
// owns the CDC pipe - writing MMSSB|Bandwidth= directly from the UI thread
// raced the FA/MD/FW poll and the QMX got a garbled command (returned ?;),
// which is why BW changes worked only intermittently. 0 = nothing pending.
static volatile uint32_t s_pending_ssb_bw = 0;
// Last SSB filter width the user set. While non-zero and we're in USB/LSB, the
// FW; poll is dropped from the rotation - reading the filter makes the QMX
// re-assert a stale active width and our setting reverts.
static volatile uint32_t s_ssb_bw_pinned = 0;
// Pending AF gain (QMX volume) write, drained by the poll task. Same
// poll-task-owns-the-pipe rule as the filter writes above. Stored +1 so that 0
// can mean "nothing pending" while still allowing a genuine request of AG 0
// (mute) to go through.
static volatile uint32_t s_pending_af_gain_p1 = 0;
// CW filter width pending write, drained by the poll task as "MMCW|CW passband=".
// Same poll-task ownership as SSB: a direct cross-thread write (e.g. from the
// web/httpd thread) would race the FA/MD/FW poll and garble into ?;. CW commits
// cleanly on its own so no pin is needed - the FW; poll reads the new width back.
static volatile uint32_t s_pending_cw_passband = 0;

void cat_request_mode(const char *mode)
{
    s_pending_mode_digit = hamlib_mode_to_digit(mode);
}

void cat_request_cw_passband(uint32_t hz)
{
    s_pending_cw_passband = hz;
    ui_update_passband_width(hz);  // optimistic; FW; poll confirms within ~150 ms
}

void cat_request_af_gain(uint16_t ag)
{
    if (ag > CAT_AF_GAIN_MAX) ag = CAT_AF_GAIN_MAX;
    s_pending_af_gain_p1 = (uint32_t)ag + 1;
}

void cat_request_ssb_bandwidth(uint32_t hz)
{
    s_pending_ssb_bw = hz;
    s_ssb_bw_pinned  = hz;
    // Drive the BW label optimistically from the requested value. While a width
    // is pinned, FW; is dropped from the poll (it makes the QMX revert the live
    // filter), so no FW response ever arrives to refresh the label via
    // ui_update_passband_width(). The touch path updated the label itself; the
    // web path did not, so a web BW change applied to the radio but never showed
    // on the Tab5. Doing it here covers every caller (touch, web, mode restore).
    ui_update_passband_width(hz);
}

int cat_get_cw_offset_hz(void) { return s_cw_offset_hz; }
const char *cat_get_qmx_fw(void) { return s_qmx_fw; }
bool cat_get_iq_mode_confirmed(void) { return s_iq_mode_confirmed; }
bool cat_get_vox_disabled(void) { return s_vox_disabled; }

bool cat_qmx_fw_at_least(int major, int minor, int patch)
{
    if (s_qmx_fw[0] == '\0') return false;
    int maj = 0, min = 0, pat = 0;
    if (sscanf(s_qmx_fw, "%d_%d_%d", &maj, &min, &pat) != 3) return false;
    if (maj != major) return maj > major;
    if (min != minor) return min > minor;
    return pat >= patch;
}

// Extra "PC;SW;" poll step for a live readout while QMX SWR Tune mode (MD8;,
// 1_04+) is transmitting. See cat_tune_poll_set_active() in cat.h.
static volatile bool s_tune_poll_active = false;
void cat_tune_poll_set_active(bool active) { s_tune_poll_active = active; }
esp_err_t cat_send_raw_cmd(const char *fmt, ...)
{
    if (!s_cdc_dev) return ESP_ERR_INVALID_STATE;
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    ESP_LOGI("cat", "raw cmd: %s", buf);
    return cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)buf, len, 200);
}

esp_err_t cat_query_power_swr(float *power_w, float *swr)
{
    if (!s_cdc_dev) return ESP_ERR_INVALID_STATE;

    s_pc_resp_len = 0;
    s_sw_resp_len = 0;

    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)"PC;SW;", 6, 200);
    if (err != ESP_OK) return err;

    for (int wi = 0; wi < 20 && (s_pc_resp_len == 0 || s_sw_resp_len == 0); wi++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (power_w) {
        // QMX PC; power scaling is firmware-dependent. Re-measured 2026-06-28
        // on-air: raw/5 read 2x the real power, so it's raw/10 now (matches
        // standard Kenwood ×10). The earlier raw/5 was calibrated against older
        // QMX firmware (1_03); the 1_04 beta changed PC encoding (see memory
        // reference_qmx_1_04_firmware). Keep this in sync with
        // cat_pwr_swr_async_read() below.
        *power_w = (s_pc_resp_len >= 3) ? (float)atoi(s_pc_resp + 2) / 10.0f : -1.0f;
    }
    if (swr) {
        // Bare "SW;" (len 3, nothing between prefix and terminator) means the
        // radio was in Receive mode when queried - no valid reading.
        *swr = (s_sw_resp_len > 3) ? (float)atoi(s_sw_resp + 2) / 100.0f : -1.0f;
    }

    ESP_LOGI(TAG, "PC;SW; -> pc='%s' sw='%s'", s_pc_resp, s_sw_resp);

    if (s_pc_resp_len == 0 && s_sw_resp_len == 0) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

// Split, non-blocking variant of cat_query_power_swr() for LIVE display during
// an FT8 burst. cat_query_power_swr() blocks up to ~600 ms waiting for the
// response, which would overrun the 160 ms FT8 symbol timing and corrupt the
// transmitted signal. Instead the caller fires _send() once (a ~ms CDC write,
// bounded to 50 ms) right after a symbol, keeps transmitting, and _read()s the
// async-captured response a few symbols later (pure buffer parse, no wait). The
// RX path (process_cat_message) fills s_pc_resp/s_sw_resp regardless of who is
// waiting, so the response lands on its own. The QMX answers PC;/SW; while
// keyed, same as the unchanged end-of-burst query.
esp_err_t cat_pwr_swr_async_send(void)
{
    if (!s_cdc_dev) return ESP_ERR_INVALID_STATE;
    s_pc_resp_len = 0;
    s_sw_resp_len = 0;
    return cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)"PC;SW;", 6, 50);
}

// Parse whatever response has arrived since the last _send(). Returns ESP_OK
// with a valid reading once both PC; and SW; have answered; ESP_ERR_TIMEOUT
// (and *power_w/*swr left at -1) if they haven't yet. Never blocks.
esp_err_t cat_pwr_swr_async_read(float *power_w, float *swr)
{
    if (power_w) *power_w = (s_pc_resp_len >= 3) ? (float)atoi(s_pc_resp + 2) / 10.0f  : -1.0f;  // see cat_query_power_swr re: /10
    if (swr)     *swr     = (s_sw_resp_len >  3) ? (float)atoi(s_sw_resp + 2) / 100.0f : -1.0f;
    if (s_pc_resp_len == 0 || s_sw_resp_len == 0) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

void cat_poll_set_paused(bool paused)
{
    s_poll_paused = paused;
    ESP_LOGI(TAG, "background poll %s", paused ? "PAUSED (TX burst owns the link)" : "resumed");
}

static void link_task(void *arg);
static void poll_task(void *arg);
static bool handle_rx(const uint8_t *data, size_t data_len, void *user_arg);
static void handle_cdc_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
static esp_err_t try_open_qmx(void);
static void process_cat_message(const char *msg, size_t len);
static void diag_log_rx(const char *msg, size_t len);

esp_err_t cat_init(void)
{
    ESP_LOGI(TAG, "CAT init (Phase 3.1 - descriptor dump on first connect)");

    s_evt_group = xEventGroupCreate();
    if (!s_evt_group) return ESP_ERR_NO_MEM;

    esp_err_t err = ESP_OK;
err = cdc_acm_host_install(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "CDC-ACM host driver installed");

    BaseType_t ok = xTaskCreatePinnedToCore(
        link_task, "cat_link", 8192, NULL, 5, NULL, 1);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "CAT link task started, waiting for QMX (VID=0x%04X PID=0x%04X)",
             QMX_VID, QMX_PID);
    return ESP_OK;
}

static void handle_cdc_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "QMX disconnected");
        xEventGroupSetBits(s_evt_group, EVT_DEV_GONE);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        break;
    default:
        break;
    }
}

static bool handle_rx(const uint8_t *data, size_t data_len, void *user_arg)
{
    for (size_t i = 0; i < data_len; i++) {
        char c = (char)data[i];
        if (s_rx_len >= CAT_RX_BUFFER_SIZE - 1) {
            ESP_LOGW(TAG, "RX buffer overflow, dropping accumulated data");
            s_rx_len = 0;
        }
        s_rx_buf[s_rx_len++] = c;
        if (c == ';') {
            s_rx_buf[s_rx_len] = '\0';
            diag_log_rx(s_rx_buf, s_rx_len);
            process_cat_message(s_rx_buf, s_rx_len);
            s_rx_len = 0;
        }
    }
    return true;
}

// Diagnostic RX logging with poll de-duplication. The FA/MD/FW poll responses
// repeat every ~150 ms; logging each verbatim swamps the ring. Log them only
// when the value changes (the per-field "Freq=/Mode=/Passband=" logs already
// mark the real transitions); everything else — MM, VN, ID, ?;, garbles — is
// logged in full since it's infrequent and high-value.
static void diag_log_rx(const char *msg, size_t len)
{
    if (!diag_log_enabled()) return;
    // Dedup routine poll responses (fixed short strings: FA…=14, MD…=4, FW…=7).
    // A response longer than its cache slot is treated as non-routine (e.g. a
    // garble) and always logged.
    static char last_fa[16], last_md[8], last_fw[12];
    char  *slot    = NULL;
    size_t slot_sz = 0;
    if      (len >= 2 && msg[0] == 'F' && msg[1] == 'A') { slot = last_fa; slot_sz = sizeof(last_fa); }
    else if (len >= 2 && msg[0] == 'M' && msg[1] == 'D') { slot = last_md; slot_sz = sizeof(last_md); }
    else if (len >= 2 && msg[0] == 'F' && msg[1] == 'W') { slot = last_fw; slot_sz = sizeof(last_fw); }
    if (slot && len < slot_sz) {
        if (strcmp(msg, slot) == 0) return;   // unchanged poll response — skip
        memcpy(slot, msg, len + 1);           // cache new value (len+1 <= slot_sz)
    }
    ESP_LOGI(TAG, "RX<- %s", msg);
}

static void process_cat_message(const char *msg, size_t len)
{
    if (len == 14 && msg[0] == 'F' && msg[1] == 'A') {
        uint32_t freq_hz = 0;
        for (size_t i = 2; i < 13; i++) {
            char d = msg[i];
            if (d < '0' || d > '9') {
                ESP_LOGW(TAG, "Bad digit in FA response: '%c'", d);
                return;
            }
            freq_hz = freq_hz * 10 + (d - '0');
        }
        if (freq_hz != s_last_freq_hz) {
            s_last_freq_hz = freq_hz;
            ESP_LOGI(TAG, "Freq = %lu Hz (%lu.%03lu MHz)",
                     (unsigned long)freq_hz,
                     (unsigned long)(freq_hz / 1000000),
                     (unsigned long)((freq_hz / 1000) % 1000));
            ui_update_frequency(freq_hz);
        }
        // ui_update_frequency() above (pan reset, freq label, axis labels)
        // is gated on freq change and must stay that way. But the Band
        // label update nested inside it can be silently dropped on the
        // very first FA response after link-up (UI init / display_lock
        // race) - and since the VFO often doesn't move again, the gated
        // path above never re-fires and "Band: ---" sticks forever.
        // ui_refresh_band_label() is cheap (band_from_freq + label set,
        // no side effects) so call it unconditionally every poll.
        ui_refresh_band_label(freq_hz);
        ui_refresh_bandplan_strip(freq_hz);
        return;
    }

    // MD response: "MDn;" — Kenwood mode digit. QMX uses:
    // 1=LSB, 2=USB, 3=CW, 5=AM (1_04+), 6=FSK, 7=CW-R, 8=SWR Tune (1_04+),
    // 9=FSK-R. Digit 4 (FM) never occurs on a QMX but is kept for Kenwood
    // compatibility. See docs/qmx-1_04-cat-comparison.md.
    if (len == 4 && msg[0] == 'M' && msg[1] == 'D') {
        char d = msg[2];
        if (d < '1' || d > '9') {
            ESP_LOGW(TAG, "Bad mode digit in MD response: '%c'", d);
            return;
        }
        static const char *kw_modes[] = {
            "?", "LSB", "USB", "CW", "FM", "AM", "DiGi", "CW-R", "TUNE", "DiGi-R"
        };
        const char *mode_str = kw_modes[d - '0'];
        if (d != s_last_mode_digit) {
            s_last_mode_digit = d;
            ESP_LOGI(TAG, "Mode = %s (raw %c)", mode_str, d);
            ui_update_mode(mode_str);
        }
        return;
    }

    // Phase 5.10G: FW (filter width) response: "FWnnnn;" - 4 digits in Hz.
    // Accept 4..5 digits (len 7 or 8) for safety.
    if ((len == 7 || len == 8) && msg[0] == 'F' && msg[1] == 'W') {
        uint32_t hz = 0;
        for (size_t i = 2; i < len - 1; i++) {
            char d = msg[i];
            if (d < '0' || d > '9') {
                ESP_LOGW(TAG, "Bad digit in FW response: '%c'", d);
                return;
            }
            hz = hz * 10 + (d - '0');
        }
        ui_update_passband_width(hz);
        s_cat_ready = true;
        return;
    }
    // TM response: "TMhhmmss;" - 9 chars, real-time-clock time-of-day.
    if (len == 9 && msg[0] == 'T' && msg[1] == 'M') {
        s_tm_resp_us  = esp_timer_get_time();   // stamp arrival for GPS-tick phase lock
        s_tm_resp_len = len;
        memcpy(s_tm_resp, msg, len);
        s_tm_resp[len] = '\0';
        return;
    }
    // MM response: starts with "MM", ends with ";"
    if (len >= 3 && msg[0] == 'M' && msg[1] == 'M') {
        s_mm_resp_len = len;
        memcpy(s_mm_resp, msg, len < sizeof(s_mm_resp) ? len : sizeof(s_mm_resp) - 1);
        s_mm_resp[len < sizeof(s_mm_resp) ? len : sizeof(s_mm_resp) - 1] = '\0';
        return;
    }
    // PC response: "PCnn;" - power output in tenths of a watt, queried via
    // cat_query_power_swr() during FT8 TX (radio must be keyed for a valid
    // reading).
    if (len >= 3 && msg[0] == 'P' && msg[1] == 'C') {
        s_pc_resp_len = len < sizeof(s_pc_resp) ? len : sizeof(s_pc_resp) - 1;
        memcpy(s_pc_resp, msg, s_pc_resp_len);
        s_pc_resp[s_pc_resp_len] = '\0';
        return;
    }
    // SW response: "SWnnn;" - SWR in hundredths, or bare "SW;" if the radio
    // is in Receive mode (no valid reading). Queried via cat_query_power_swr().
    if (len >= 3 && msg[0] == 'S' && msg[1] == 'W') {
        s_sw_resp_len = len < sizeof(s_sw_resp) ? len : sizeof(s_sw_resp) - 1;
        memcpy(s_sw_resp, msg, s_sw_resp_len);
        s_sw_resp[s_sw_resp_len] = '\0';
        return;
    }
    // QMX returns "?;" for unsupported commands; we just log once.
    if (len == 2 && msg[0] == '?' && msg[1] == ';') {
        static bool warned = false;
        if (!warned) {
            ESP_LOGW(TAG, "QMX returned ?; (one or more poll commands unsupported)");
            warned = true;
        }
        return;
    }
    // VN response: "VN<version>;" — QMX/QDX firmware version string, e.g.
    // "VN1_03_002QMX;". Store the part between "VN" and the trailing ";".
    if (len >= 4 && msg[0] == 'V' && msg[1] == 'N') {
        size_t vlen = len - 3;  // drop "VN" prefix and ";" suffix
        if (vlen >= sizeof(s_qmx_fw)) vlen = sizeof(s_qmx_fw) - 1;
        memcpy(s_qmx_fw, msg + 2, vlen);
        s_qmx_fw[vlen] = '\0';
        ESP_LOGI(TAG, "QMX firmware: %s", s_qmx_fw);
        // Re-evaluate 1_04+-gated drawer sections (AM mode, Tune button) now
        // that the version is known - the drawer may already have been built
        // (lazy, first-open) before VN; answered.
        ui_notify_qmx_fw_known();
        return;
    }
    // Q9 response: "Q9n;" — IQ mode state, queried at link-up to confirm the
    // Q9 1; enable command was actually accepted (the CDC write succeeding
    // only proves the bytes reached the radio, not that it parsed them — see
    // memory project_q9_iq_mode_verification).
    if (len >= 3 && msg[0] == 'Q' && msg[1] == '9') {
        s_q9_resp_len = len < sizeof(s_q9_resp) ? len : sizeof(s_q9_resp) - 1;
        memcpy(s_q9_resp, msg, s_q9_resp_len);
        s_q9_resp[s_q9_resp_len] = '\0';
        return;
    }
    // Q3 response: "Q3n;" — VOX enable state, queried at link-up to confirm
    // VOX was disabled for the session (same write-echo caveat as Q9 above).
    if (len >= 3 && msg[0] == 'Q' && msg[1] == '3') {
        s_q3_resp_len = len < sizeof(s_q3_resp) ? len : sizeof(s_q3_resp) - 1;
        memcpy(s_q3_resp, msg, s_q3_resp_len);
        s_q3_resp[s_q3_resp_len] = '\0';
        return;
    }
    if (len == 6 && msg[0] == 'I' && msg[1] == 'D') {
        ESP_LOGI(TAG, "Radio ID: %s", msg);
        return;
    }
}

static esp_err_t try_open_qmx(void)
{
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 256,
        .in_buffer_size = 256,
        .event_cb = handle_cdc_event,
        .data_cb = handle_rx,
        .user_arg = NULL,
    };

    esp_err_t err = cdc_acm_host_open(QMX_VID, QMX_PID, 0, &cfg, &s_cdc_dev);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "QMX CDC opened");

    const cdc_acm_line_coding_t lc = {
        .dwDTERate = CAT_BAUD_RATE,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
    };
    err = cdc_acm_host_line_coding_set(s_cdc_dev, &lc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "line_coding_set failed: 0x%x", err);
        return err;
    }

    cdc_acm_host_set_control_line_state(s_cdc_dev, true, true);
    ESP_LOGI(TAG, "QMX configured: %d baud, 8N1", CAT_BAUD_RATE);

    cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)"ID;", 3, 500);

    // Phase 3.1 — dump audio descriptors ONCE per session
    if (!s_audio_dumped) {
        ESP_LOGI(TAG, "=== Dumping QMX descriptors ===");
        cdc_acm_host_desc_print(s_cdc_dev);
        ESP_LOGI(TAG, "=== End descriptor dump ===");
        s_audio_dumped = true;
    }

    return ESP_OK;
}

static void poll_task(void *arg)
{
    ESP_LOGI(TAG, "Poll task started (%d ms interval, alternating FA/MD)", CAT_POLL_INTERVAL_MS);
    int phase = 0;
    int poll_fail = 0;   // consecutive poll-TX failures; one transient timeout must not kill the poll
    while (s_cdc_dev != NULL) {
        // v0.12.0: an FT8 TX burst owns the CDC-ACM link exclusively for its
        // ~12.7s duration (precise 160ms-cadence TA<freq>; sequence) - an
        // interleaved poll here would desync its timing or garble the
        // stream. Cooperative check only (never vTaskSuspend - that risks
        // deadlocking on the driver's internal mutex mid-transfer).
        if (s_poll_paused) {
            vTaskDelay(pdMS_TO_TICKS(CAT_POLL_INTERVAL_MS));
            continue;
        }
        // Drain a pending SSB-filter write here (poll-task context owns the
        // CDC pipe), so it can't interleave with a poll command and get a ?;.
        // Target the committed "Filter RX" menu item - that's what FW; reads
        // and what shows in the QMX SSB menu (the "Bandwidth" token is a live
        // value that the QMX reverts). FW; will read the new width back.
        char md = s_pending_mode_digit;
        if (md != 0) {
            s_pending_mode_digit = 0;
            char cmd[8];
            cmd[0] = 'M'; cmd[1] = 'D'; cmd[2] = md; cmd[3] = ';'; cmd[4] = 0;
            esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)cmd, 4, 200);
            ESP_LOGI(TAG, "mode -> MD%c; (%s)", md, err == ESP_OK ? "ok" : "fail");
            vTaskDelay(pdMS_TO_TICKS(CAT_POLL_INTERVAL_MS));
            continue;
        }
        uint32_t ag_p1 = s_pending_af_gain_p1;
        if (ag_p1 != 0) {
            s_pending_af_gain_p1 = 0;
            unsigned ag = (unsigned)(ag_p1 - 1);
            // "AG0" + 3 digits. The vendor manual's own example drops the
            // leading zero ("AG091;" for 91) but its Get reply is "AG0091", so
            // the 3-digit form is the one the radio demonstrably speaks. Value
            // is in 0.25 dB steps, range 0-799 per the manual.
            char cmd[16];
            int n = snprintf(cmd, sizeof cmd, "AG0%03u;", ag);
            esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)cmd,
                                                          (size_t)n, 200);
            ESP_LOGI(TAG, "AF gain -> %s (%.2f dB) (%s)", cmd, ag * 0.25,
                     err == ESP_OK ? "ok" : "fail");
            vTaskDelay(pdMS_TO_TICKS(CAT_POLL_INTERVAL_MS));
            continue;
        }
        uint32_t bw = s_pending_ssb_bw;
        if (bw != 0) {
            s_pending_ssb_bw = 0;
            char mm[32];
            // Two QMX SSB-filter items must agree or the live filter reverts:
            //  - "Filter RX": the committed/stored value (persists, shows in
            //    the QMX menu, but on its own doesn't reload the live filter).
            //  - "Bandwidth": the live/active filter (applies immediately, but
            //    on its own the QMX reverts it to the committed Filter RX).
            // Write both to the same value: live applies AND there's nothing
            // for the FW; poll to revert to. Result sticks and persists.
            int n = snprintf(mm, sizeof(mm), "MMSSB|Filter RX=%lu;", (unsigned long)bw);
            esp_err_t e1 = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)mm, n, 200);
            vTaskDelay(pdMS_TO_TICKS(40));
            n = snprintf(mm, sizeof(mm), "MMSSB|Bandwidth=%lu;", (unsigned long)bw);
            esp_err_t e2 = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)mm, n, 200);
            ESP_LOGI(TAG, "SSB filter -> %lu Hz (RX=%s, BW=%s)", (unsigned long)bw,
                     e1 == ESP_OK ? "ok" : "fail", e2 == ESP_OK ? "ok" : "fail");
            vTaskDelay(pdMS_TO_TICKS(CAT_POLL_INTERVAL_MS));
            continue;
        }
        uint32_t cwbw = s_pending_cw_passband;
        if (cwbw != 0) {
            s_pending_cw_passband = 0;
            char mm[32];
            int n = snprintf(mm, sizeof(mm), "MMCW|CW passband=%lu;", (unsigned long)cwbw);
            esp_err_t e = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)mm, n, 200);
            ESP_LOGI(TAG, "CW passband -> %lu Hz (%s)", (unsigned long)cwbw,
                     e == ESP_OK ? "ok" : "fail");
            vTaskDelay(pdMS_TO_TICKS(CAT_POLL_INTERVAL_MS));
            continue;
        }
        // Phase 5.10G: 3-way rotation FA / MD / FW (passband width). A 4th
        // phase (PC;SW;) is added while cat_tune_poll_set_active(true) - live
        // power/SWR readout during QMX SWR Tune mode (1_04+, see
        // docs/qmx-1_04-cat-comparison.md). MD; stays in rotation during Tune
        // so an exit via the radio's own front panel is still picked up.
        // While an SSB filter is pinned and we're in USB/LSB, skip FW; - the
        // QMX reverts the live filter whenever the filter is read back.
        bool in_ssb = (s_last_mode_digit == '1' || s_last_mode_digit == '2');
        bool skip_fw = (s_ssb_bw_pinned != 0 && in_ssb);
        bool tune_poll = s_tune_poll_active;
        int n_phases = tune_poll ? 4 : 3;
        const char *cmd;
        size_t cmd_len;
        switch (phase) {
            case 0:  cmd = "FA;"; cmd_len = 3; break;
            case 1:  cmd = "MD;"; cmd_len = 3; break;
            case 2:  cmd = skip_fw ? NULL : "FW;"; cmd_len = 3; break;
            default: cmd = "PC;SW;"; cmd_len = 6; break;  // only reached when tune_poll
        }
        if (cmd != NULL) {
            // Diagnostic: don't log every poll TX (FA/MD/FW every ~50ms swamps
            // the ring). Emit a heartbeat every 10s so the log shows polling is
            // alive; the de-duplicated RX side (diag_log_rx) shows actual value
            // changes, and one-off writes (Sent:/SSB filter->/raw cmd) log fully.
            if (diag_log_enabled()) {
                uint64_t hb = esp_timer_get_time();
                if (hb - s_diag_poll_hb_us > 10000000ULL) {
                    s_diag_poll_hb_us = hb;
                    ESP_LOGI(TAG, "poll heartbeat: FA/MD/FW cycling @ %dms (freq=%luHz mode=%s)",
                             CAT_POLL_INTERVAL_MS, (unsigned long)s_last_freq_hz, cat_get_mode_str());
                }
            }
            esp_err_t err = cdc_acm_host_data_tx_blocking(
                s_cdc_dev, (const uint8_t *)cmd, cmd_len, 200);
            if (err != ESP_OK) {
                // A CDC TX failure burst must NEVER permanently kill the poll
                // task. Root-caused 2026-07-10 (serial-log proven): opening any
                // full-screen modal reliably injures the CDC link ~150-250ms
                // later (every USB timeout in a capture followed a modal open,
                // 4/4). Usually one transfer fails and the next recovers; but
                // the old "20 consecutive fails -> break" heuristic could fire
                // on a longer burst and EXIT the poll for the whole session -
                // which froze cat_get_frequency() AND, once the poll stopped
                // draining the link, cascaded into a full USB-host wedge
                // (CAT + audio dead, frozen screen, power-cycle only).
                //
                // A REAL disconnect is signalled independently: the EVT_DEV_GONE
                // handler NULLs s_cdc_dev, which ends this loop via its while
                // condition. So we no longer self-exit on a failure count at
                // all - we just keep retrying this phase until either the link
                // recovers (next TX succeeds, poll_fail resets) or a genuine
                // disconnect tears us down. After a sustained run we back the
                // retry cadence off from 50ms to 500ms so a truly-gone radio
                // (QMX power-cycle often fires NO disconnect event on this
                // board - see audio.c) doesn't spin the CPU or flood the log.
                poll_fail++;
                if (poll_fail == 1 || (poll_fail % 20) == 0) {
                    ESP_LOGW(TAG, "%s send fail: 0x%x (%d consecutive) — retrying, waiting for link to recover",
                             cmd, err, poll_fail);
                }
                int backoff_ms = (poll_fail > 20) ? 500 : CAT_POLL_INTERVAL_MS;
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                continue;  // retry this phase; never break on failure count
            }
            if (poll_fail > 0) {
                ESP_LOGI(TAG, "CAT link recovered after %d consecutive failures", poll_fail);
                poll_fail = 0;
            }
        }
        phase = (phase + 1) % n_phases;
        vTaskDelay(pdMS_TO_TICKS(CAT_POLL_INTERVAL_MS));
    }
    ESP_LOGI(TAG, "Poll task exiting");
    s_poll_task = NULL;
    vTaskDelete(NULL);
}

static void link_task(void *arg)
{
    while (1) {
        esp_err_t err = try_open_qmx();
        if (err == ESP_OK) {
            s_rx_len = 0;
            s_last_freq_hz = 0;
            s_last_mode_digit = 0;
            // Phase 5.10J: enable QMX IQ mode for this session. Q9 1; is
            // session-only (not written to EEPROM), so the user's normal
            // QMX state is restored automatically on disconnect/power-cycle.
            //
            // Retried up to IQ_MODE_MAX_ATTEMPTS times: a field report (Dirk
            // DK7CVD, 2026-06-30) showed the single-shot handshake silently
            // leaving IQ mode off on a real connect (Q9; readback timed out -
            // "(no response)"), which is indistinguishable from a transient
            // USB/CDC hiccup right after enumeration. Without IQ mode the QMX
            // streams plain (non-IQ) audio: the panadapter shows the signal
            // shifted/mirrored, tunable across the whole 48 kHz window with
            // the VFO knob, audio only present once retuned back into range -
            // exactly Dirk's symptom. Previously this only logged a warning
            // and gave up, silently degrading the session until a manual QMX
            // power-cycle. If all attempts fail, s_iq_mode_confirmed stays
            // false and ui_set_iq_mode_warning() raises a persistent on-screen
            // banner so the user isn't left guessing why the spectrum looks
            // wrong.
            {
                s_iq_mode_confirmed = false;
                const int IQ_MODE_MAX_ATTEMPTS = 4;
                for (int attempt = 1; attempt <= IQ_MODE_MAX_ATTEMPTS; attempt++) {
                    const char *iq_on = "Q9 1;";
                    esp_err_t terr = cdc_acm_host_data_tx_blocking(
                        s_cdc_dev, (const uint8_t *)iq_on, 5, 200);
                    if (terr == ESP_OK) {
                        ESP_LOGI(TAG, "QMX IQ mode enabled (Q9 1;) attempt %d/%d",
                                 attempt, IQ_MODE_MAX_ATTEMPTS);
                    } else {
                        ESP_LOGW(TAG, "Failed to enable QMX IQ mode (attempt %d/%d): 0x%x",
                                 attempt, IQ_MODE_MAX_ATTEMPTS, terr);
                    }
                    // Readback: a successful CDC write only proves the bytes
                    // reached the radio, not that it accepted them. Query Q9;
                    // and check the radio actually reports IQ mode on.
                    //
                    // The QMX echoes every write back ("Q91;") asynchronously.
                    // Without the delay below that echo arrives DURING the wait
                    // loop for the real Q9; response and triggers a false
                    // "confirmed ON" — IQ mode never actually turns on, leaving
                    // the FT8 decoder with non-IQ audio (140 candidates, 0
                    // decodes), cleared only by a QMX power cycle.
                    // Wait long enough for the write echo to arrive and be
                    // consumed, THEN flush and send the real query.
                    vTaskDelay(pdMS_TO_TICKS(150));
                    s_q9_resp_len = 0;
                    const char *iq_q = "Q9;";
                    esp_err_t qerr = cdc_acm_host_data_tx_blocking(
                        s_cdc_dev, (const uint8_t *)iq_q, strlen(iq_q), 200);
                    if (qerr == ESP_OK) {
                        for (int wi = 0; wi < 20 && s_q9_resp_len == 0; wi++) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                        }
                        if (s_q9_resp_len >= 3 && s_q9_resp[2] == '1') {
                            ESP_LOGI(TAG, "QMX IQ mode confirmed ON (%s) on attempt %d/%d",
                                     s_q9_resp, attempt, IQ_MODE_MAX_ATTEMPTS);
                            s_iq_mode_confirmed = true;
                            break;
                        } else {
                            ESP_LOGW(TAG, "QMX IQ mode NOT confirmed (attempt %d/%d, raw='%s')",
                                     attempt, IQ_MODE_MAX_ATTEMPTS,
                                     s_q9_resp_len ? s_q9_resp : "(no response)");
                        }
                    } else {
                        ESP_LOGW(TAG, "Failed to query QMX IQ mode state (attempt %d/%d): 0x%x",
                                 attempt, IQ_MODE_MAX_ATTEMPTS, qerr);
                    }
                    if (attempt < IQ_MODE_MAX_ATTEMPTS) {
                        vTaskDelay(pdMS_TO_TICKS(300));
                    }
                }
                if (!s_iq_mode_confirmed) {
                    ESP_LOGE(TAG, "QMX IQ mode NOT confirmed after %d attempts — "
                             "panadapter will show mirrored/aliased spectrum; "
                             "check QMX System Config IQ Mode setting or power-cycle the QMX",
                             IQ_MODE_MAX_ATTEMPTS);
                }
                ui_set_iq_mode_warning(!s_iq_mode_confirmed);
            }
            // Disable QMX VOX for this session (Q3 0;). The panadapter keys the
            // radio purely over CAT (TX;/TA;/RX;), never with transmit audio, so
            // VOX serves no purpose here. It is disabled defensively: with VOX on
            // AND the QMX's SSB TX input set to the USB sound card, stray audio on
            // the USB-audio OUT endpoint could key the radio - we never open that
            // endpoint, but disabling VOX removes the hazard entirely. Users
            // coming from audio-VOX FT8 apps (e.g. iFTX on iOS) typically have VOX
            // ON, so this flips the setting they'd otherwise have to change by
            // hand. Like Q9, Q3 is session-only (not written to EEPROM), so their
            // saved VOX preference is restored on the next QMX power-cycle.
            //
            // Best-effort, unlike the IQ-mode handshake: a failure to disable VOX
            // is non-critical (VOX-on can't misfire while we never feed TX audio),
            // so there is no on-screen warning - just a couple of retries and a
            // log line recording the state seen.
            {
                s_vox_disabled = false;
                const int VOX_MAX_ATTEMPTS = 3;
                for (int attempt = 1; attempt <= VOX_MAX_ATTEMPTS; attempt++) {
                    const char *vox_off = "Q3 0;";
                    esp_err_t terr = cdc_acm_host_data_tx_blocking(
                        s_cdc_dev, (const uint8_t *)vox_off, 5, 200);
                    if (terr != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to send VOX-off (Q3 0;) attempt %d/%d: 0x%x",
                                 attempt, VOX_MAX_ATTEMPTS, terr);
                    }
                    // Same asynchronous write-echo hazard as Q9: the QMX echoes
                    // the write back ("Q30;"), which would otherwise be misread as
                    // the Q3; query response. Wait for the echo, then flush and
                    // send the real query.
                    vTaskDelay(pdMS_TO_TICKS(150));
                    s_q3_resp_len = 0;
                    const char *vox_q = "Q3;";
                    esp_err_t qerr = cdc_acm_host_data_tx_blocking(
                        s_cdc_dev, (const uint8_t *)vox_q, strlen(vox_q), 200);
                    if (qerr == ESP_OK) {
                        for (int w = 0; w < 20 && s_q3_resp_len == 0; w++) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                        }
                        if (s_q3_resp_len >= 3 && s_q3_resp[2] == '0') {
                            ESP_LOGI(TAG, "QMX VOX confirmed OFF (%s) on attempt %d/%d",
                                     s_q3_resp, attempt, VOX_MAX_ATTEMPTS);
                            s_vox_disabled = true;
                            break;
                        } else {
                            ESP_LOGW(TAG, "QMX VOX not yet OFF (attempt %d/%d, raw='%s')",
                                     attempt, VOX_MAX_ATTEMPTS,
                                     s_q3_resp_len ? s_q3_resp : "(no response)");
                        }
                    } else {
                        ESP_LOGW(TAG, "Failed to query QMX VOX state (attempt %d/%d): 0x%x",
                                 attempt, VOX_MAX_ATTEMPTS, qerr);
                    }
                    if (attempt < VOX_MAX_ATTEMPTS) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                }
                if (!s_vox_disabled) {
                    ESP_LOGW(TAG, "QMX VOX not confirmed OFF after %d attempts — "
                             "harmless for panadapter use (we key via CAT, not audio), "
                             "but disable VOX on the QMX if it keys unexpectedly",
                             VOX_MAX_ATTEMPTS);
                }
            }
            // One-shot inline FA/MD/FW round-trip right after link-up, so
            // the top-bar Band/Mode/BW labels populate immediately instead
            // of waiting for the CW-offset query + band-table scan below
            // (7-10+ seconds) to finish before poll_task gets a chance to
            // run. process_cat_message() (called via handle_rx) updates the
            // UI directly as each response arrives.
            {
                static const char *const warmup_cmds[] = { "FA;", "MD;", "FW;" };
                for (size_t wi = 0; wi < sizeof(warmup_cmds) / sizeof(warmup_cmds[0]); wi++) {
                    const char *cmd = warmup_cmds[wi];
                    esp_err_t werr = cdc_acm_host_data_tx_blocking(
                        s_cdc_dev, (const uint8_t *)cmd, strlen(cmd), 200);
                    if (werr != ESP_OK) {
                        ESP_LOGW(TAG, "Warmup %s send failed: 0x%x", cmd, werr);
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }

            // Query QMX firmware version once (VN; -> "VN<ver>QMX;"). Useful
            // for diagnostics and bug reports — different QMX firmware behaves
            // differently (IQ output, TA TX, MMSSB filter tokens).
            {
                const char *vn_q = "VN;";
                esp_err_t verr = cdc_acm_host_data_tx_blocking(
                    s_cdc_dev, (const uint8_t *)vn_q, strlen(vn_q), 200);
                if (verr == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(100));  // response handled in process_cat_message
                } else {
                    ESP_LOGW(TAG, "Failed to query firmware version (VN): 0x%x", verr);
                }
            }

            // Read CW offset from QMX menu (session value, EEPROM-persisted on QMX side)
            {
                const char *cw_q = "MMCW|CW offset;";
                esp_err_t cerr = cdc_acm_host_data_tx_blocking(
                    s_cdc_dev, (const uint8_t *)cw_q, strlen(cw_q), 200);
                if (cerr == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    // Response is in s_mm_resp — parse MMnnn;
                    if (s_mm_resp_len >= 4 && strncmp(s_mm_resp, "MM", 2) == 0) {
                        int val = atoi(s_mm_resp + 2);
                        if (val >= 600 && val <= 800) {
                            s_cw_offset_hz = val;
                            ESP_LOGI(TAG, "QMX CW offset: %d Hz", s_cw_offset_hz);
                        } else {
                            ESP_LOGW(TAG, "CW offset out of range (%d), using 700", val);
                        }
                    } else {
                        ESP_LOGW(TAG, "No valid MM response for CW offset, using 700");
                    }
                    s_rx_len = 0;
                } else {
                    ESP_LOGW(TAG, "Failed to query CW offset: 0x%x", cerr);
                }
            }
            // Query band list from QMX band config (up to 16 slots).
            // Right after CAT link-up (especially after a cold QMX power-on)
            // the menu system can take a while to fully populate, so any
            // individual MM query can come back empty/zero even for a slot
            // that's genuinely configured. Give it time, then retry each
            // slot before treating it as a real gap.
            // This scan runs before poll_task starts, so the Band/Mode/BW
            // top-bar labels stay "---" until it finishes. Diagnostic
            // captures showed valid slots (0-5) always respond on the
            // first try once the menu is ready; keep some margin but don't
            // make the user stare at "---" for 10+ seconds.
            vTaskDelay(pdMS_TO_TICKS(2000));
            {
                s_band_count = 0;
                int consecutive_empty = 0;
                // Two diagnostic captures confirmed this QMX's band table has
                // exactly 6 entries (60/40/30/20/17/15m, slots 0-5) and slots
                // 6-15 are consistently empty. Stop after 2 consecutive empty
                // slots instead of grinding through all 16 (was costing ~24s
                // of scan time for nothing and delaying s_band_count's final
                // value during which the dropdown could be opened mid-scan).
                const int MAX_CONSECUTIVE_EMPTY = 2;
                const int MAX_RETRIES = 8;
                for (int bi = 0; bi < CAT_MAX_BANDS; bi++) {
                    bool got_band = false;
                    char bname[8] = {0};
                    uint32_t cf = 0;
                    bool transport_error = false;

                    for (int retry = 0; retry < MAX_RETRIES && !got_band; retry++) {
                        if (retry > 0) vTaskDelay(pdMS_TO_TICKS(300));

                        // Query band name
                        char qname[48];
                        snprintf(qname, sizeof(qname), "MMBand config.|Band name (m)[%d];", bi);
                        s_rx_len = 0;
                        s_mm_resp_len = 0;
                        esp_err_t be = cdc_acm_host_data_tx_blocking(
                            s_cdc_dev, (const uint8_t *)qname, strlen(qname), 200);
                        if (be != ESP_OK) { transport_error = true; break; }
                        for (int wi = 0; wi < 20 && s_mm_resp_len == 0; wi++) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                        }
                        if (s_mm_resp_len == 0) {
                            ESP_LOGI(TAG, "Band[%d] name query: no response (retry %d)", bi, retry);
                            continue;
                        }
                        if (s_mm_resp_len < 3 || strncmp(s_mm_resp, "MM", 2) != 0 ||
                            strncmp(s_mm_resp, "MM?", 3) == 0) {
                            ESP_LOGI(TAG, "Band[%d] name query: empty slot (len=%u, retry %d)", bi, (unsigned)s_mm_resp_len, retry);
                            continue;
                        }
                        // Parse: MMxx; where xx is band name
                        int nlen = (int)s_mm_resp_len - 3;  // strip "MM" prefix and ";"
                        if (nlen <= 0 || nlen >= (int)sizeof(bname)) continue;
                        snprintf(bname, sizeof(bname), "%.*s", nlen, s_mm_resp + 2);
                        s_mm_resp_len = 0;

                        // Query center frequency
                        char qfreq[56];
                        snprintf(qfreq, sizeof(qfreq), "MMBand config.|Frequency center[%d];", bi);
                        be = cdc_acm_host_data_tx_blocking(
                            s_cdc_dev, (const uint8_t *)qfreq, strlen(qfreq), 200);
                        if (be != ESP_OK) { transport_error = true; break; }
                        for (int wi = 0; wi < 20 && s_mm_resp_len == 0; wi++) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                        }
                        if (s_mm_resp_len == 0) {
                            ESP_LOGI(TAG, "Band[%d] freq query: no response (retry %d)", bi, retry);
                            continue;
                        }
                        if (s_mm_resp_len < 3 || strncmp(s_mm_resp, "MM", 2) != 0) {
                            ESP_LOGI(TAG, "Band[%d] freq query: bad response (len=%u, retry %d)", bi, (unsigned)s_mm_resp_len, retry);
                            continue;
                        }
                        cf = (uint32_t)atoi(s_mm_resp + 2);
                        s_mm_resp_len = 0;
                        if (cf == 0) {
                            ESP_LOGI(TAG, "Band[%d] freq query: zero (retry %d)", bi, retry);
                            continue;
                        }
                        got_band = true;
                    }

                    if (transport_error) break;

                    if (!got_band) {
                        consecutive_empty++;
                        ESP_LOGI(TAG, "Band[%d]: no valid data after %d retries (%d consecutive)", bi, MAX_RETRIES, consecutive_empty);
                        if (consecutive_empty >= MAX_CONSECUTIVE_EMPTY) break;
                        continue;
                    }

                    // 11m (CB): the QMX+ exposes this when configured with no
                    // band limits, and the operator wants to use it — so include
                    // whatever band the firmware reports, including "11". (An
                    // earlier build filtered "11" out on the assumption it was a
                    // spurious slot; that was a mistake — reverted 2026-07-09.)
                    snprintf(s_band_list[s_band_count].name, sizeof(s_band_list[0].name), "%s", bname);
                    s_band_list[s_band_count].center_hz = cf;
                    ESP_LOGI(TAG, "Band[%d]: %sm @ %lu Hz", bi, bname, (unsigned long)cf);
                    s_band_count++;
                    consecutive_empty = 0;
                }
                ESP_LOGI(TAG, "Band list: %d bands found", s_band_count);
            }
            xTaskCreatePinnedToCore(
                poll_task, "cat_poll", 4096, NULL, 5, &s_poll_task, 1);

            xEventGroupWaitBits(s_evt_group, EVT_DEV_GONE,
                                pdTRUE, pdFALSE, portMAX_DELAY);
            ESP_LOGW(TAG, "QMX gone, cleaning up");
            if (s_cdc_dev) {
                cdc_acm_dev_hdl_t dev = s_cdc_dev;
                // Clear the handle FIRST so poll_task's "while (s_cdc_dev !=
                // NULL)" check exits at its next iteration, then WAIT for it
                // to actually exit before calling cdc_acm_host_close(). The
                // v0.18.6 "tolerate 20 consecutive transient failures" poll
                // retry can otherwise still be mid cdc_acm_host_data_tx_blocking()
                // on this exact handle when we close it here — that race hit
                // cdc_acm_host_close()'s usb_host_interface_release(), which
                // returned ESP_ERR_INVALID_STATE into an ESP_ERROR_CHECK and
                // aborted (Dirk DK7CVD, 2026-06-29 serial capture). 200 ms
                // poll interval x 20 retries = up to ~4s worst case; the TX
                // itself is bounded to 200ms, so poll_task notices the NULL
                // and exits well before that in practice.
                s_cdc_dev = NULL;
                s_cat_ready = false;
                int wait_ms = 0;
                while (s_poll_task != NULL && wait_ms < 4000) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    wait_ms += 20;
                }
                if (s_poll_task != NULL) {
                    ESP_LOGW(TAG, "poll_task did not exit within %dms, closing anyway", wait_ms);
                }
                cdc_acm_host_close(dev);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}




esp_err_t cat_set_frequency(uint32_t freq_hz)
{
    if (s_cdc_dev == NULL) {
        return ESP_ERR_INVALID_STATE;  // QMX not connected
    }
    // Rate-limit: drop calls that arrive within 200 ms of previous TX
    uint64_t now = esp_timer_get_time();
    if (now - s_last_tx_us < 200000) {
        return ESP_ERR_TIMEOUT;
    }
    s_last_tx_us = now;

    // Format: "FA" + 11 digits zero-padded + ";"
    char cmd[16];
    int n = snprintf(cmd, sizeof(cmd), "FA%011lu;", (unsigned long)freq_hz);
    if (n != 14) {
        ESP_LOGW(TAG, "cat_set_frequency: snprintf produced %d chars (expected 14)", n);
        return ESP_FAIL;
    }
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)cmd, 14, 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FA TX failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Sent: %s (target %lu Hz)", cmd, (unsigned long)freq_hz);
    return ESP_OK;
}

esp_err_t cat_set_frequency_forced(uint32_t freq_hz)
{
    s_last_tx_us = 0;  // bypass the 200 ms rate-limiter for deliberate user writes
    return cat_set_frequency(freq_hz);
}

uint32_t cat_get_frequency(void)
{
    return s_last_freq_hz;
}

const char *cat_get_mode_str(void)
{
    char d = s_last_mode_digit;
    if (d < '1' || d > '9') return "";
    // Keep in sync with the identical table in process_cat_message()'s MD
    // response handler above.
    static const char *kw_modes[] = {
        "?", "LSB", "USB", "CW", "FM", "AM", "DiGi", "CW-R", "TUNE", "DiGi-R"
    };
    return kw_modes[d - '0'];
}

// ---- Phase 9 (v0.9.6): setters used by rigctld_server -------------------

// Map a Hamlib mode string (case-insensitive) to a Kenwood mode digit.
// Returns 0 for unknown.
static char hamlib_mode_to_digit(const char *mode)
{
    if (!mode) return 0;
    // Uppercase comparison
    char buf[16];
    size_t n = 0;
    while (mode[n] && n < sizeof(buf) - 1) {
        char c = mode[n];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[n++] = c;
    }
    buf[n] = 0;
    // Reverse variants come first so "USB" does not match "CW-R" etc.
    if (strcmp(buf, "CW-R")   == 0 || strcmp(buf, "CWR") == 0)   return '7';
    if (strcmp(buf, "FSK-R")  == 0 || strcmp(buf, "RTTYR") == 0 ||
        strcmp(buf, "DIGI-R") == 0 || strcmp(buf, "PKTLSB") == 0) return '9';
    if (strcmp(buf, "LSB")    == 0) return '1';
    if (strcmp(buf, "USB")    == 0) return '2';
    if (strcmp(buf, "CW")     == 0) return '3';
    if (strcmp(buf, "FM")     == 0) return '4';
    if (strcmp(buf, "AM")     == 0) return '5';
    // SWR Tune mode (1_04+ only, MD8;) - see docs/qmx-1_04-cat-comparison.md.
    // Exit is via cat_request_mode() with the mode string that was active
    // before Tune was entered, NOT a bare "MD0;" - the CAT manual's Set list
    // for MD never lists 0 as a valid value.
    if (strcmp(buf, "TUNE")   == 0) return '8';
    // Digital soundcard family all map to mode 6 (DiGi/FSK)
    if (strcmp(buf, "FSK")    == 0 ||
        strcmp(buf, "DIGI")   == 0 ||
        strcmp(buf, "PKTUSB") == 0 ||
        strcmp(buf, "RTTY")   == 0 ||
        strcmp(buf, "FT8")    == 0 ||
        strcmp(buf, "FT4")    == 0 ||
        strcmp(buf, "JS8")    == 0) return '6';
    return 0;
}

esp_err_t cat_set_mode(const char *mode)
{
    char digit = hamlib_mode_to_digit(mode);
    if (digit == 0) {
        ESP_LOGW(TAG, "cat_set_mode: unknown mode '%s'", mode ? mode : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cdc_dev == NULL) return ESP_ERR_INVALID_STATE;

    uint64_t now = esp_timer_get_time();
    if (now - s_last_tx_us < 200000) return ESP_ERR_TIMEOUT;
    s_last_tx_us = now;

    char cmd[8];
    cmd[0] = 'M'; cmd[1] = 'D'; cmd[2] = digit; cmd[3] = ';'; cmd[4] = 0;
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)cmd, 4, 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MD TX failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Sent: %s (mode '%s')", cmd, mode);
    return ESP_OK;
}

esp_err_t cat_set_passband_hz(uint32_t hz)
{
    if (hz < 50 || hz > 9999) return ESP_ERR_INVALID_ARG;
    if (s_cdc_dev == NULL) return ESP_ERR_INVALID_STATE;

    uint64_t now = esp_timer_get_time();
    if (now - s_last_tx_us < 200000) return ESP_ERR_TIMEOUT;
    s_last_tx_us = now;

    // Format: "FW" + 4 digits zero-padded + ";"
    char cmd[10];
    int n = snprintf(cmd, sizeof(cmd), "FW%04lu;", (unsigned long)hz);
    if (n != 7) {
        ESP_LOGW(TAG, "cat_set_passband_hz: snprintf produced %d chars (expected 7)", n);
        return ESP_FAIL;
    }
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)cmd, 7, 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FW TX failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Sent: %s (passband %lu Hz)", cmd, (unsigned long)hz);
    return ESP_OK;
}

esp_err_t cat_set_qmx_time(int hour, int min, int sec)
{
    if (hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    return cat_send_raw_cmd("TM%02d%02d%02d;", hour, min, sec);
}

esp_err_t cat_query_qmx_time(int *out_hour, int *out_min, int *out_sec)
{
    if (!s_cdc_dev || !s_cat_ready) return ESP_ERR_INVALID_STATE;

    cat_poll_set_paused(true);
    s_tm_resp_len = 0;
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)"TM;", 3, 200);
    if (err == ESP_OK) {
        for (int i = 0; i < 10 && s_tm_resp_len == 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    cat_poll_set_paused(false);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TM; query TX failed: 0x%x", err);
        return err;
    }
    if (s_tm_resp_len != 9 || strncmp(s_tm_resp, "TM", 2) != 0) {
        ESP_LOGW(TAG, "No valid TM response (len=%u)", (unsigned)s_tm_resp_len);
        return ESP_FAIL;
    }
    for (int i = 2; i < 8; i++) {
        if (s_tm_resp[i] < '0' || s_tm_resp[i] > '9') return ESP_FAIL;
    }
    *out_hour = (s_tm_resp[2] - '0') * 10 + (s_tm_resp[3] - '0');
    *out_min  = (s_tm_resp[4] - '0') * 10 + (s_tm_resp[5] - '0');
    *out_sec  = (s_tm_resp[6] - '0') * 10 + (s_tm_resp[7] - '0');
    if (*out_hour > 23 || *out_min > 59 || *out_sec > 59) return ESP_FAIL;
    ESP_LOGI(TAG, "QMX RTC time: %02d:%02d:%02d", *out_hour, *out_min, *out_sec);
    return ESP_OK;
}

// Parse the current TM response buffer into h/m/s. Returns false if not a valid
// "TMhhmmss;" (9 chars, all digits, in range).
static bool parse_tm_resp(int *h, int *m, int *s)
{
    if (s_tm_resp_len != 9 || strncmp(s_tm_resp, "TM", 2) != 0) return false;
    for (int i = 2; i < 8; i++) if (s_tm_resp[i] < '0' || s_tm_resp[i] > '9') return false;
    *h = (s_tm_resp[2]-'0')*10 + (s_tm_resp[3]-'0');
    *m = (s_tm_resp[4]-'0')*10 + (s_tm_resp[5]-'0');
    *s = (s_tm_resp[6]-'0')*10 + (s_tm_resp[7]-'0');
    return (*h <= 23 && *m <= 59 && *s <= 59);
}

// GPS second-tick sync. Rapidly polls TM; and catches the instant the seconds
// field ticks over (N -> N+1) - that flip is the true GPS second boundary. On
// success returns the h/m/s AT the flip (the NEW second) and *out_flip_us = the
// esp_timer time the flipping TM response LANDED (stamped in the RX handler, so
// it carries no wait-loop polling granularity). The caller then phase-locks the
// system clock to that beat instead of the naive whole-second apply, giving
// roughly +/-(one TM round-trip) accuracy - drift-free and WiFi-independent.
// Pauses the poll for the whole burst and blocks up to ~1.3 s (enough to span
// one second boundary). ESP_OK only if a flip was caught. Bails if another op
// already owns the CDC pipe (FT8 TX pauses the same poll flag).
esp_err_t cat_gps_tick_sync(int *out_hour, int *out_min, int *out_sec, int64_t *out_flip_us)
{
    if (!s_cdc_dev || !s_cat_ready) return ESP_ERR_INVALID_STATE;
    if (s_poll_paused)              return ESP_ERR_INVALID_STATE;  // FT8 TX / other op owns the pipe

    cat_poll_set_paused(true);
    int       prev_sec = -1;
    esp_err_t result   = ESP_ERR_TIMEOUT;
    int64_t   start    = esp_timer_get_time();

    while (esp_timer_get_time() - start < 1300000) {   // ~1.3 s cap: spans any 1 s boundary
        s_tm_resp_len = 0;
        if (cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)"TM;", 3, 100) != ESP_OK)
            break;
        // Wait briefly for the async RX handler to stamp + fill the response.
        for (int i = 0; i < 25 && s_tm_resp_len == 0; i++) vTaskDelay(pdMS_TO_TICKS(2));
        int h, m, s;
        if (!parse_tm_resp(&h, &m, &s)) continue;
        if (prev_sec >= 0 && s != prev_sec) {          // the tick
            *out_hour = h; *out_min = m; *out_sec = s;
            *out_flip_us = s_tm_resp_us;               // exact arrival of the flipping reading
            result = ESP_OK;
            break;
        }
        prev_sec = s;
    }

    cat_poll_set_paused(false);
    if (result == ESP_OK)
        ESP_LOGI(TAG, "GPS tick: %02d:%02d:%02d boundary caught", *out_hour, *out_min, *out_sec);
    else
        ESP_LOGW(TAG, "GPS tick: no second flip caught in 1.3 s (err=%d)", result);
    return result;
}
