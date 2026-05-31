#include "webserver_ws.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dsp.h"

#define WS_FRAME_TYPE_SPECTRUM  0x01
#define WS_PUSH_PERIOD_MS       100        // ~10 fps
#define WS_HEADER_LEN           2
#define WS_PAYLOAD_LEN          DSP_FFT_SIZE
#define WS_FRAME_LEN            (WS_HEADER_LEN + WS_PAYLOAD_LEN)

// Quantization range: -130 dBm (q=0) .. -30 dBm (q=255). ~0.39 dB/step.
#define WS_DB_MIN   (-130.0f)
#define WS_DB_MAX   (-30.0f)

// Must match IF_OFFSET_HZ inside main/ui/ui.c (QMX dial sits at +12 kHz baseband).
#define WS_IF_OFFSET_HZ 12000

static const char *TAG = "ws";

// Shared state between the URI handler and the push task. The URI handler
// captures (server, fd) on handshake and returns immediately so the httpd
// worker is free to serve /api/status while a WS session is active.
//
// Async sends MUST use static (non-stack) frame storage -- the v0.8.x lesson
// was that httpd_ws_send_frame_async with a stack-local frame struct silently
// drops frames and returns ESP_OK.
static httpd_handle_t volatile s_server   = NULL;
static int            volatile s_ws_fd    = -1;
static volatile bool           s_session_active = false;

static float          s_spec[DSP_FFT_SIZE];
static uint8_t        s_payload[WS_FRAME_LEN];
static httpd_ws_frame_t s_ws_frame;  // static, NOT stack-local

static TaskHandle_t   s_push_task = NULL;

// httpd worker has 1 task, so this URI handler runs in the only worker.
// We must return ESP_OK quickly so subsequent /api/status requests can be served.
static esp_err_t ws_uri_handler(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        // Incoming WS frame (CLOSE/PING/etc). We do not subscribe to control
        // frames (handle_ws_control_frames=false) so this is mostly unreachable.
        return ESP_OK;
    }

    if (s_session_active) {
        ESP_LOGW(TAG, "Another client already connected; refusing");
        return ESP_FAIL;
    }

    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        ESP_LOGE(TAG, "httpd_req_to_sockfd failed");
        return ESP_FAIL;
    }

    s_ws_fd = fd;
    s_session_active = true;
    ESP_LOGI(TAG, "Client connected, fd=%d -- push task takes over", fd);
    return ESP_OK;  // <-- frees httpd worker immediately
}

// Push task: sleeps when no session, runs at ~10 fps when one is open.
static void ws_push_task(void *arg)
{
    (void)arg;
    s_ws_frame.final   = true;
    s_ws_frame.type    = HTTPD_WS_TYPE_BINARY;
    s_ws_frame.payload = s_payload;
    s_ws_frame.len     = WS_FRAME_LEN;
    s_payload[0] = WS_FRAME_TYPE_SPECTRUM;
    s_payload[1] = 0;

    const int   N        = DSP_FFT_SIZE;
    const int   half     = N / 2;
    const int   if_shift = (WS_IF_OFFSET_HZ * N) / DSP_SAMPLE_RATE_HZ;
    const float scale    = 255.0f / (WS_DB_MAX - WS_DB_MIN);

    TickType_t last   = xTaskGetTickCount();
    uint32_t   sent   = 0;
    TickType_t fps_at = last;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(WS_PUSH_PERIOD_MS));

        if (!s_session_active || s_server == NULL || s_ws_fd < 0) {
            // Idle: reset fps counter so we do not log stale stats on reconnect.
            sent = 0;
            fps_at = xTaskGetTickCount();
            continue;
        }

        if (dsp_get_spectrum(s_spec) != ESP_OK) continue;

        // Match ui_push_spectrum: fftshift + IF offset -> wire byte 0 == leftmost device pixel.
        for (int i = 0; i < N; i++) {
            int bin = (i < half) ? (i + half) : (i - half);
            bin = (bin + if_shift) % N;
            float db = s_spec[bin];
            if (db < WS_DB_MIN) db = WS_DB_MIN;
            else if (db > WS_DB_MAX) db = WS_DB_MAX;
            int q = (int)((db - WS_DB_MIN) * scale + 0.5f);
            s_payload[WS_HEADER_LEN + i] = (uint8_t)q;
        }

        esp_err_t err = httpd_ws_send_frame_async(s_server, s_ws_fd, &s_ws_frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "async send failed fd=%d: %s; closing session",
                     s_ws_fd, esp_err_to_name(err));
            s_session_active = false;
            s_ws_fd = -1;
            continue;
        }

        sent++;
        TickType_t now = xTaskGetTickCount();
        if ((now - fps_at) >= pdMS_TO_TICKS(5000)) {
            float fps = (float)sent * 1000.0f / (float)pdTICKS_TO_MS(now - fps_at);
            ESP_LOGI(TAG, "tx %.1f fps", fps);
            sent = 0;
            fps_at = now;
        }
    }
}

esp_err_t webserver_ws_start(httpd_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    if (s_server) return ESP_OK;

    s_server         = server;
    s_session_active = false;
    s_ws_fd          = -1;

    static const httpd_uri_t uri_ws = {
        .uri                      = "/ws",
        .method                   = HTTP_GET,
        .handler                  = ws_uri_handler,
        .user_ctx                 = NULL,
        .is_websocket             = true,
        .handle_ws_control_frames = false,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /ws: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    if (s_push_task == NULL) {
        BaseType_t ok = xTaskCreate(ws_push_task, "ws_push", 4096, NULL, 5, &s_push_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "xTaskCreate ws_push failed");
            s_server = NULL;
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "WS /ws ready (push task, target %d ms period)", WS_PUSH_PERIOD_MS);
    return ESP_OK;
}

void webserver_ws_stop(void)
{
    if (!s_server) return;
    s_session_active = false;
    s_ws_fd = -1;
    s_server = NULL;
    // Leave the push task running idle -- it will pick up the next start cleanly.
    ESP_LOGI(TAG, "WS stopped");
}
