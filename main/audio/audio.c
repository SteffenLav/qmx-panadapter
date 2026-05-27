#include "audio.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "usb/uac_host.h"
#include "iq_balance.h"

static const char *TAG = "audio";

// Internal event types we put on our queue
typedef enum {
    AE_NONE = 0,
    AE_RX_CONNECTED,
    AE_RX_DONE,
    AE_DISCONNECTED,
    AE_TRANSFER_ERROR,
} audio_evt_kind_t;

typedef struct {
    audio_evt_kind_t kind;
    uint8_t addr;
    uint8_t iface_num;
} audio_evt_t;

#define EVT_QUEUE_LEN          16
#define RX_BUF_BYTES           19200
#define INTERNAL_RX_BUF_BYTES  19200
#define STATS_PERIOD_MS        1000

// Ring buffer holds decoded int16 stereo pairs (4 bytes per pair).
// 64 kB / 4 = 16384 pairs = ~341 ms of headroom @ 48 kHz
#define SAMPLE_RING_BYTES      (64 * 1024)

// Producer-side state
static TaskHandle_t s_audio_task = NULL;
static QueueHandle_t s_evt_queue = NULL;
static uac_host_device_handle_t s_uac_dev = NULL;
static RingbufHandle_t s_ring = NULL;

// Stats (touched from RX context, snapshot from task)
static volatile uint32_t s_samples_this_period = 0;
static volatile int16_t  s_peak_left  = 0;
static volatile int16_t  s_peak_right = 0;
static volatile uint32_t s_dropped_this_period = 0;
static int64_t s_period_start_us = 0;

// Discovered at runtime
static uint32_t s_sample_freq = 0;
static uint8_t  s_channels    = 0;
static uint8_t  s_bit_res     = 0;

// Consumer (stub DSP) stats
// Forward decls
static void audio_task(void *arg);
static void uac_lib_event_cb(uint8_t addr, uint8_t iface_num,
                             const uac_host_driver_event_t event, void *arg);
static void uac_dev_event_cb(uac_host_device_handle_t dev_hdl,
                             const uac_host_device_event_t event, void *arg);

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "Audio init (Phase 3.3 - ring-buffered int16 stereo + stub consumer)");

    s_evt_queue = xQueueCreate(EVT_QUEUE_LEN, sizeof(audio_evt_t));
    if (!s_evt_queue) return ESP_ERR_NO_MEM;

    s_ring = xRingbufferCreate(SAMPLE_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!s_ring) {
        ESP_LOGE(TAG, "Failed to create sample ring buffer (%d bytes)",
                 SAMPLE_RING_BYTES);
        return ESP_ERR_NO_MEM;
    }

    iq_balance_init();
    iq_balance_set_enabled(true);  // Phase A: hardcoded ON for first test
    ESP_LOGI(TAG, "IQ balance correction: ENABLED");
    ESP_LOGI(TAG, "Sample ring buffer: %d bytes (~%lu ms @ 48k stereo int16)",
             SAMPLE_RING_BYTES,
             (unsigned long)(SAMPLE_RING_BYTES / 4 * 1000 / 48000));

    const uac_host_driver_config_t cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_lib_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = uac_host_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install failed: 0x%x (%s)",
                 err, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "UAC host driver installed");

    BaseType_t ok = xTaskCreatePinnedToCore(
        audio_task, "audio_task", 4096, NULL, 3, &s_audio_task, 0);  // Phase 5.7: core 0 pri 3
    if (ok != pdPASS) return ESP_FAIL;

    return ESP_OK;
}

size_t audio_read_samples(int16_t *dst, size_t max_pairs, uint32_t timeout_ms)
{
    if (!s_ring || !dst || max_pairs == 0) return 0;

    size_t want_bytes = max_pairs * sizeof(int16_t) * 2;
    size_t got_bytes = 0;
    void *item = xRingbufferReceiveUpTo(
        s_ring, &got_bytes, pdMS_TO_TICKS(timeout_ms), want_bytes);
    if (!item) return 0;

    memcpy(dst, item, got_bytes);
    vRingbufferReturnItem(s_ring, item);
    return got_bytes / (sizeof(int16_t) * 2);
}

static void uac_lib_event_cb(uint8_t addr, uint8_t iface_num,
                             const uac_host_driver_event_t event, void *arg)
{
    audio_evt_t e = { .addr = addr, .iface_num = iface_num };
    switch (event) {
    case UAC_HOST_DRIVER_EVENT_RX_CONNECTED:
        e.kind = AE_RX_CONNECTED;
        ESP_LOGI(TAG, "Lib event: RX_CONNECTED addr=%u iface=%u", addr, iface_num);
        if (s_evt_queue) xQueueSend(s_evt_queue, &e, 0);
        break;
    case UAC_HOST_DRIVER_EVENT_TX_CONNECTED:
        ESP_LOGI(TAG, "Lib event: TX_CONNECTED addr=%u iface=%u (ignored)",
                 addr, iface_num);
        break;
    default:
        ESP_LOGI(TAG, "Lib event %d addr=%u iface=%u", (int)event, addr, iface_num);
        break;
    }
}

static void uac_dev_event_cb(uac_host_device_handle_t dev_hdl,
                             const uac_host_device_event_t event, void *arg)
{
    audio_evt_t e = { .addr = 0, .iface_num = 0 };
    switch (event) {
    case UAC_HOST_DEVICE_EVENT_RX_DONE:
        e.kind = AE_RX_DONE;
        break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        e.kind = AE_TRANSFER_ERROR;
        break;
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        e.kind = AE_DISCONNECTED;
        break;
    default:
        return;
    }
    if (s_evt_queue) xQueueSend(s_evt_queue, &e, 0);
}

static void log_stats(void)
{
    int64_t now = esp_timer_get_time();
    if (s_period_start_us == 0) { s_period_start_us = now; return; }
    int64_t elapsed_us = now - s_period_start_us;
    if (elapsed_us < STATS_PERIOD_MS * 1000) return;

    uint32_t samples = s_samples_this_period;
    int16_t  pL = s_peak_left;
    int16_t  pR = s_peak_right;
    uint32_t dropped = s_dropped_this_period;
    s_samples_this_period = 0;
    s_peak_left = 0;
    s_peak_right = 0;
    s_dropped_this_period = 0;
    s_period_start_us = now;

    uint32_t pairs_per_sec = (uint32_t)((uint64_t)samples * 1000000ULL / (uint64_t)elapsed_us);
    if (dropped > 0) {
        ESP_LOGW(TAG, "RX %u pairs/s peak L=%d R=%d DROPPED=%u (ring full)",
                 (unsigned)pairs_per_sec, (int)pL, (int)pR, (unsigned)dropped);
    } else {
        ESP_LOGI(TAG, "RX %u pairs/s peak L=%d R=%d",
                 (unsigned)pairs_per_sec, (int)pL, (int)pR);
    }
}

// Decode a packed 24-bit little-endian signed PCM sample (3 bytes) into int32_t.
static inline int32_t s24_to_s32(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (u & 0x00800000U) u |= 0xFF000000U;
    return (int32_t)u;
}

static void process_rx(void)
{
    // Raw 24-bit packed bytes from the QMX
    static uint8_t raw[RX_BUF_BYTES];
    // Decoded int16 stereo pairs (max half the bytes from raw, since 6B->4B)
    static int16_t decoded[(RX_BUF_BYTES / 6) * 2 + 2];

    // Phase 5.7: drain loop. First read waits up to 25 ms for data;
    // subsequent reads in the same call are non-blocking so we drain the
    // UAC driver buffer in one polling iteration of audio_task.
    bool first_read = true;
    while (1) {
        uint32_t bytes_read = 0;
        uint32_t to = first_read ? pdMS_TO_TICKS(25) : 0;
        first_read = false;
        esp_err_t err = uac_host_device_read(s_uac_dev, raw, sizeof(raw),
                                             &bytes_read, to);
        if (err != ESP_OK || bytes_read == 0) return;

        // Each stereo pair = 6 bytes (3B L + 3B R, little-endian signed 24-bit)
        size_t pairs = bytes_read / 6;
        if (pairs == 0) return;

        int16_t local_peak_L = s_peak_left;
        int16_t local_peak_R = s_peak_right;

        for (size_t i = 0; i < pairs; i++) {
            const uint8_t *p = raw + 6*i;
            int32_t L = s24_to_s32(p);
            int32_t R = s24_to_s32(p + 3);
            int32_t Ls = L >> 8;
            int32_t Rs = R >> 8;
            if (Ls > 32767) Ls = 32767; else if (Ls < -32768) Ls = -32768;
            if (Rs > 32767) Rs = 32767; else if (Rs < -32768) Rs = -32768;

            int16_t Is = (int16_t)Ls;
            int16_t Qs = (int16_t)Rs;
            iq_balance_apply(&Is, &Qs);
            decoded[2*i]     = Is;
            decoded[2*i + 1] = Qs;

            int16_t aL = (Ls < 0) ? (int16_t)-Ls : (int16_t)Ls;
            int16_t aR = (Rs < 0) ? (int16_t)-Rs : (int16_t)Rs;
            if (aL > local_peak_L) local_peak_L = aL;
            if (aR > local_peak_R) local_peak_R = aR;
        }

        s_peak_left = local_peak_L;
        s_peak_right = local_peak_R;
        s_samples_this_period += pairs;

        size_t bytes_to_push = pairs * sizeof(int16_t) * 2;
        BaseType_t sent = xRingbufferSend(s_ring, decoded, bytes_to_push, 0);
        if (sent != pdTRUE) {
            s_dropped_this_period += pairs;
        }
        // Loop back for another non-blocking drain read.
    }
}

static esp_err_t open_and_start(uint8_t addr, uint8_t iface_num)
{
    const uac_host_device_config_t dev_cfg = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = INTERNAL_RX_BUF_BYTES,
        .buffer_threshold = INTERNAL_RX_BUF_BYTES / 4,
        .callback = uac_dev_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = uac_host_device_open(&dev_cfg, &s_uac_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_open(addr=%u iface=%u) failed: 0x%x (%s)",
                 addr, iface_num, err, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Device opened (addr=%u iface=%u)", addr, iface_num);

    uac_host_dev_alt_param_t alt = {0};
    err = uac_host_get_device_alt_param(s_uac_dev, 1, &alt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_device_alt_param failed: 0x%x", err);
        uac_host_device_close(s_uac_dev);
        s_uac_dev = NULL;
        return err;
    }
    ESP_LOGI(TAG, "Alt 1: channels=%u, %u-bit, first_rate=%lu Hz",
             alt.channels, alt.bit_resolution,
             (unsigned long)alt.sample_freq[0]);

    const uac_host_stream_config_t stream_cfg = {
        .channels = alt.channels,
        .bit_resolution = alt.bit_resolution,
        .sample_freq = alt.sample_freq[0],
        .flags = 0,
    };
    err = uac_host_device_start(s_uac_dev, &stream_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_start failed: 0x%x (%s)", err, esp_err_to_name(err));
        uac_host_device_close(s_uac_dev);
        s_uac_dev = NULL;
        return err;
    }

    s_channels    = alt.channels;
    s_bit_res     = alt.bit_resolution;
    s_sample_freq = alt.sample_freq[0];
    s_period_start_us = esp_timer_get_time();
    s_samples_this_period = 0;
    s_peak_left = 0;
    s_peak_right = 0;
    s_dropped_this_period = 0;

    ESP_LOGI(TAG, "UAC stream started: %lu Hz, %u ch, %u-bit",
             (unsigned long)s_sample_freq, s_channels, s_bit_res);
    return ESP_OK;
}

static void audio_task(void *arg)
{
    // Phase 5.7: polling loop mirroring qrp_companion (Zhenxing Han, N6HAN) mic_task pattern.
    // audio_task pulls UAC bytes in a tight loop with a 200 ms read timeout while the device
    // is up, and yields once per iteration for the watchdog. RX_DONE / TRANSFER_ERROR events
    // are no longer consumed; only connect / disconnect events are handled out-of-band.
    while (1) {
        // Connect / disconnect events still arrive on the queue (non-blocking peek).
        audio_evt_t e;
        if (xQueueReceive(s_evt_queue, &e, 0) == pdTRUE) {
            switch (e.kind) {
            case AE_RX_CONNECTED:
                if (s_uac_dev == NULL) {
                    open_and_start(e.addr, e.iface_num);
                } else {
                    ESP_LOGW(TAG, "Already streaming; ignoring extra RX_CONNECTED");
                }
                break;
            case AE_DISCONNECTED:
                ESP_LOGW(TAG, "UAC disconnected, cleaning up");
                if (s_uac_dev) {
                    uac_host_device_stop(s_uac_dev);
                    uac_host_device_close(s_uac_dev);
                    s_uac_dev = NULL;
                }
                break;
            default:
                break;
            }
        }
        if (s_uac_dev) {
            process_rx();
            // vTaskDelay(1) removed - 10ms at default tick rate, was starving the read
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        log_stats();
    }
}