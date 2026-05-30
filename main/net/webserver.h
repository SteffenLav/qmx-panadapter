#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the HTTP server on port 80.
 *
 * Called from wifi_task once the station has an IP. Idempotent:
 * calling twice without stop() in between is a no-op (returns ESP_OK).
 *
 * @return ESP_OK on success, or the error from httpd_start().
 */
esp_err_t webserver_start(void);

/**
 * @brief Stop the HTTP server.
 *
 * Called from wifi_task on disconnect. Idempotent: safe to call
 * when not running.
 */
void webserver_stop(void);

#ifdef __cplusplus
}
#endif