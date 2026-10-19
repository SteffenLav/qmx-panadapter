#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Web task stacks. HERE, not as literals at the xTaskCreate/config sites.
 *
 * ⚠ 2026-10-10: both sizes were duplicated literals - config.stack_size, the
 * xTaskCreate call, the stack_hwm log line and its JSON all carried their own
 * copy. Raising one would have left the INSTRUMENT reporting the old size, so
 * the headroom percentage would have been wrong in the only place anyone
 * looks. One definition each, used everywhere.
 *
 * ⛔ NEITHER may move to PSRAM. Handlers write SPIFFS on the httpd task
 * (/api/lotw_cert, the ADIF clear and delete paths) and flash writes run with
 * the cache off, where a task with a PSRAM stack cannot run - see
 * util/psram_task.h, hardware-confirmed by the #218 OTA panic. Both therefore
 * cost MALLOC_CAP_DMA pool, which is why they are not simply made huge.
 *
 * ⛔ A HIGH-WATER MARK ONLY REPORTS PATHS ALREADY TAKEN. Every cut to these
 * two has been made on a watermark that looked roomy because the deep path had
 * not run in that session, and both then turned out near-full when it did.
 * Do not cut either without exercising the SPIFFS/TLS/upload handlers first.
 */

/* httpd: measured peak use ~4.7 KB on 2026-08-28, but ~8.94 KB on 2026-10-10
 * (1,300 B free of 10,240 after 1.84 MB of /ss.bmp + 177 KB of / + three
 * passes of every safe GET). Raised 10240 -> 14336 the same day. */
#define WEB_HTTPD_STACK_SIZE    14336

/* ws_push: 3072 -> 4096 on 2026-09-06 "with 364 B of headroom left", i.e. a
 * peak use of ~3.7 KB against 4,096 - 91% full at its worst. On 2026-10-10 it
 * read 3,164 B free, which only means that deep path did not run. 364 B is not
 * a margin; raised to 6144. */
#define WEB_WS_PUSH_STACK_SIZE  6144

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