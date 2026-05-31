#pragma once
#include "esp_err.h"

/**
 * @brief Start the rigctld TCP server on port 4532.
 *
 * Spawns an accept-loop task. Should be called once after WiFi gets an IP.
 * Idempotent; subsequent calls return ESP_OK without restarting.
 *
 * @return ESP_OK on success, ESP_FAIL on socket / task create failure.
 */
esp_err_t rigctld_server_start(void);

/**
 * @brief Stop the rigctld TCP server.
 *
 * Closes the listening socket; per-client tasks finish on their next read.
 */
void rigctld_server_stop(void);
