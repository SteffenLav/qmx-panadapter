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

static httpd_handle_t s_server = NULL;
// Single-client enforcement: only one /ws session at a time.
static volatile bool s_session_busy = false;

static esp_err_t ws_uri_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // First entry = WS upgrade. Sync handshake is done by esp_http_server.
        if (s_session_busy) {
            ESP_LOGW(TAG, "Another client already connected; refusing");
            return ESP_FAIL;
        }
        s_session_busy = true;
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Client connected, fd=%d -- entering send loop", fd);

        // Per-session stack buffers (handler runs in its own session task).
        float   spec[DSP_FFT_SIZE];
        uint8_t out[WS_FRAME_LEN];
        out[0] = WS_FRAME_TYPE_SPECTRUM;
        out[1] = 0;

        const int   N        = DSP_FFT_SIZE;
        const int   half     = N / 2;
        const int   if_shift = (WS_IF_OFFSET_HZ * N) / DSP_SAMPLE_RATE_HZ;
        const float scale    = 255.0f / (WS_DB_MAX - WS_DB_MIN);

        TickType_t  last     = xTaskGetTickCount();
        uint32_t    sent     = 0;
        TickType_t  fps_at   = last;

        for (;;) {
            vTaskDelayUntil(&last, pdMS_TO_TICKS(WS_PUSH_PERIOD_MS));

            if (dsp_get_spectrum(spec) != ESP_OK) continue;

            // Match ui_push_spectrum: fftshift + IF offset -> wire byte 0 == leftmost device pixel.
            for (int i = 0; i < N; i++) {
                int bin = (i < half) ? (i + half) : (i - half);
                bin = (bin + if_shift) % N;
                float db = spec[bin];
                if (db < WS_DB_MIN) db = WS_DB_MIN;
                else if (db > WS_DB_MAX) db = WS_DB_MAX;
                int q = (int)((db - WS_DB_MIN) * scale + 0.5f);
                out[WS_HEADER_LEN + i] = (uint8_t)q;
            }

            httpd_ws_frame_t ws = {
                .final   = true,
                .type    = HTTPD_WS_TYPE_BINARY,
                .payload = out,
                .len     = WS_FRAME_LEN,
            };
            esp_err_t err = httpd_ws_send_frame(req, &ws);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "send failed: %s; closing session", esp_err_to_name(err));
                break;
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

        ESP_LOGI(TAG, "Client fd=%d disconnected", fd);
        s_session_busy = false;
        return ESP_OK;
    }
    // Incoming WS frame (CLOSE/PING/etc) -- only reached if our handler is
    // re-entered, which doesn't happen because we hold the session in the loop above.
    return ESP_OK;
}

esp_err_t webserver_ws_start(httpd_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    if (s_server) return ESP_OK;
    s_server = server;
    s_session_busy = false;

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
    ESP_LOGI(TAG, "WS /ws ready (sync sender, target %d ms period)", WS_PUSH_PERIOD_MS);
    return ESP_OK;
}

void webserver_ws_stop(void)
{
    if (!s_server) return;
    // Handler-loop sessions will exit on their next send failure when httpd shuts down.
    s_server = NULL;
    ESP_LOGI(TAG, "WS stopped");
}