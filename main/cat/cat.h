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
