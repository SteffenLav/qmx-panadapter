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
