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

// #217: WS health counters for /api/status - lets a reported stall be matched
// against what the device actually did, instead of argued about.
void webserver_ws_stats(uint32_t *sessions, uint32_t *takeovers,
                        uint32_t *closes, uint32_t *partial);

/**
 * @brief Suspend/resume the spectrum push stream.
 *
 * While paused, the push task yields the WiFi TX path so an in-flight network
 * transfer (QRZ/eQSL upload, ADIF/diag download) doesn't have to share the
 * single SDIO->C6 link with the ~10 fps stream. Wrap transfers in
 * set_paused(true)/set_paused(false). Safe to call from any task.
 */
void webserver_ws_set_paused(bool paused);

#ifdef __cplusplus
}
#endif