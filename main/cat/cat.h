#pragma once

#include "esp_err.h"

/**
 * @brief Initialize CAT subsystem.
 *
 * Phase 2.1: brings up USB Host, starts host event task.
 *            Logs device connect/disconnect events with VID/PID.
 *
 * Phase 2.2 (later): opens CDC-ACM interface to QMX.
 * Phase 2.3 (later): polls Kenwood TS-480 CAT, updates UI frequency.
 */
esp_err_t cat_init(void);
