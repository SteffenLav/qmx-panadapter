#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register /ws URI handler and start the 10 Hz spectrum push task.
 *
 * Must be called AFTER httpd_start(). Idempotent.
 *
 * Single-client policy: latest connection wins. A second client connecting
 * replaces the first; the first is dropped on the next send error.
 */
esp_err_t webserver_ws_start(httpd_handle_t server);

/**
 * @brief Stop the push task and drop any connected client.
 *
 * Idempotent. Must be called BEFORE httpd_stop().
 */
void webserver_ws_stop(void);

#ifdef __cplusplus
}
#endif