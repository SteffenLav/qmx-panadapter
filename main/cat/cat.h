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

