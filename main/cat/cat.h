#pragma once

#include "esp_err.h"

/**
 * @brief Initialize CAT subsystem.
 *
 * Phase 2.3: USB Host + CDC-ACM to QMX, polls FA; every 200ms,
 *            updates ui_update_frequency() on change.
 *
 * Phase 3.1: also dumps audio class descriptors on first connection.
 */
esp_err_t cat_init(void);

/**
 * @brief Send a frequency-set command (FA; in Kenwood/Elecraft protocol) to the QMX.
 *
 * Rate-limited internally — calls within ~200 ms of the previous one are dropped.
 * The QMX will retune; the next CAT poll cycle picks up the new freq and updates UI.
 *
 * @param freq_hz target frequency in Hz (will be sent as 11-digit padded ASCII)
 * @return ESP_OK if queued, ESP_ERR_INVALID_STATE if QMX not connected,
 *         ESP_ERR_TIMEOUT if rate-limited
 */
esp_err_t cat_set_frequency(uint32_t freq_hz);

/**
 * @brief Same as cat_set_frequency() but bypasses the 200 ms rate-limiter.
 *
 * Use for deliberate user actions (e.g. preset taps) where the write must
 * go through even if another write just happened (e.g. sticky-settings restore).
 */
esp_err_t cat_set_frequency_forced(uint32_t freq_hz);

/**
 * @brief Get the most recently observed VFO frequency.
 *
 * Updated by the CAT poll loop on every FA response. Returns 0 before
 * the first poll completes or if the QMX is disconnected.
 *
 * @return frequency in Hz
 */
uint32_t cat_get_frequency(void);
/* Returns current mode string (e.g. "USB", "CW", "DiGi").
 * Returns "" if CAT not ready or no MD response yet. */
const char *cat_get_mode_str(void);

/**
 * @brief Send a mode-set command (MD; in Kenwood/Elecraft protocol) to the QMX.
 *
 * Translates Hamlib mode strings to Kenwood digits:
 *   LSB=1, USB=2, CW=3, FM=4, AM=5, FSK/DiGi/PKTUSB/RTTY/FT8=6,
 *   CW-R/CWR=7, FSK-R/DIGI-R/RTTYR=9.
 * Anything unrecognised returns ESP_ERR_INVALID_ARG.
 *
 * Shares the 200 ms TX rate-limit with cat_set_frequency().
 *
 * @param mode  Hamlib mode string (case-insensitive)
 * @return ESP_OK on send, ESP_ERR_INVALID_ARG on unknown mode,
 *         ESP_ERR_INVALID_STATE if QMX not connected,
 *         ESP_ERR_TIMEOUT if rate-limited.
 */
esp_err_t cat_set_mode(const char *mode);

/**
 * @brief Send a passband-width command (FW; in Kenwood/Elecraft protocol).
 *
 * Width is rounded to nearest Hz and clipped to 4 digits. Shares the
 * 200 ms TX rate-limit.
 *
 * @param hz  passband width in Hz (50 .. 9999)
 * @return ESP_OK on send, ESP_ERR_INVALID_ARG if out of range,
 *         ESP_ERR_INVALID_STATE if QMX not connected,
 *         ESP_ERR_TIMEOUT if rate-limited.
 */
esp_err_t cat_set_passband_hz(uint32_t hz);

#include <stdbool.h>

/**
 * @brief Returns true once the QMX has fully booted on the CAT link.
 *
 * Set when the first FW (passband width) response is parsed, which is
 * the last leg of the CDC-open -> Q9 1; -> FA -> MD -> FW handshake.
 * Empirically this is the earliest moment the QMX is producing clean,
 * mode-correct I/Q on the USB sound card.
 * Cleared on QMX USB disconnect.
 */
bool cat_is_ready(void);
/**
 * @brief Get the CW offset (Hz) read from QMX at connect time.
 *
 * Read once via MMCW|CW offset; after Q9 1; on CDC open.
 * Returns 700 (QMX default) if not yet read or CAT not connected.
 */
int cat_get_cw_offset_hz(void);
/**
 * @brief QMX firmware version string from the VN; query (e.g. "1_03_002QMX").
 * Returns an empty string until the radio has answered VN; after link-up.
 */
const char *cat_get_qmx_fw(void);
/**
 * @brief Send a raw formatted CAT/MM command string to the QMX.
 * Uses printf-style format. Fire-and-forget, no response parsed.
 */
esp_err_t cat_send_raw_cmd(const char *fmt, ...);

/**
 * @brief Query the QMX's instantaneous power output and SWR (PC; and SW;).
 *
 * Both readings are only valid while the radio is keyed (transmitting) - SW;
 * returns no value in Receive mode. Blocks for the CAT round trip (up to
 * ~400ms total). On success, *power_w and *swr are set to -1.0f if that
 * particular reading was unavailable (e.g. SWR queried while not transmitting).
 *
 * @return ESP_OK if at least one reading was received, ESP_ERR_TIMEOUT if
 *         neither responded, ESP_ERR_INVALID_STATE if CAT is not connected.
 */
esp_err_t cat_query_power_swr(float *power_w, float *swr);

/**
 * @brief Request a mode change (deferred to the poll task).
 *
 * Thread-safe to call from the LVGL/UI thread. The MD<digit>; command is
 * queued and sent by the poll task on its next cycle, avoiding a race with
 * the FA/MD/FW poll on the shared CDC pipe. Any subsequent call before the
 * poll drains it overwrites the previous request (last write wins).
 *
 * @param mode  Hamlib mode string (e.g. "USB", "LSB", "CW", "DiGi")
 */
void cat_request_mode(const char *mode);

/**
 * @brief Request the QMX SSB receive filter bandwidth (Hz: 2500/2700/2900/3200).
 *
 * Thread-safe to call from the LVGL/UI thread. The actual MMSSB|Bandwidth=
 * write is deferred to the poll task (which owns the CDC pipe), avoiding a
 * command-interleave race with the FA/MD/FW poll that made BW changes flaky.
 */
void cat_request_ssb_bandwidth(uint32_t hz);

/**
 * @brief Cooperatively pause/resume the background FA;/MD;/FW; poll loop.
 *
 * v0.12.0 (FT8 TX): while a TX burst owns the CDC-ACM link (sending
 * TX;/TA<freq>;/.../RX; at a precise 160 ms cadence), an interleaved poll
 * could desync the burst timing or garble the stream. The poll task checks
 * this flag cooperatively at the top of its loop (a plain vTaskDelay+continue
 * — never vTaskSuspend, which risks deadlocking on the driver's internal
 * mutex if the task is paused mid-transfer). Idempotent; safe to call from
 * any task.
 *
 * @param paused  true to pause polling, false to resume
 */
void cat_poll_set_paused(bool paused);

#define CAT_MAX_BANDS 16

typedef struct {
    char     name[8];      // e.g. "40"
    uint32_t center_hz;    // Frequency center from band config
} cat_band_entry_t;

/**
 * @brief Get the band list read from QMX at connect time.
 * @param out_count  number of valid entries populated
 * @return pointer to static array of cat_band_entry_t
 */
const cat_band_entry_t *cat_get_band_list(int *out_count);

/**
 * @brief Set the QMX's onboard real-time clock (time-of-day only, no date).
 *
 * Sends TM<hh><mm><ss>; (Kenwood/QMX-specific CAT command). Used to keep
 * the QMX RTC in sync with UTC whenever this device has a good time
 * source (SNTP), so the QMX RTC can later serve as a fallback time
 * source for FT8 when there's no WiFi (e.g. POTA).
 *
 * @return ESP_OK on send, ESP_ERR_INVALID_ARG if hour/min/sec out of range,
 *         ESP_ERR_INVALID_STATE if QMX not connected.
 */
esp_err_t cat_set_qmx_time(int hour, int min, int sec);

/**
 * @brief Query the QMX's onboard real-time clock (time-of-day only, no date).
 *
 * Sends "TM;" and blocks briefly (pausing the background poll loop) for
 * the "TMhhmmss;" response. Used as a fallback UTC time-of-day source for
 * FT8 slot alignment when SNTP/WiFi is unavailable.
 *
 * @return ESP_OK and out_hour/out_min/out_sec populated on success,
 *         ESP_ERR_INVALID_STATE if QMX not connected/ready, ESP_FAIL on
 *         a bad/missing response.
 */
esp_err_t cat_query_qmx_time(int *out_hour, int *out_min, int *out_sec);
