#include "audio.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "usb/uac_host.h"

static const char *TAG = "audio";

// Driver-event queue entry: which RX device just got connected/disconnected,
// or which device had an RX-done event.
typedef enum {
    AE_NONE = 0,
    AE_RX_CONNECTED,     // lib callback - new RX iface
    AE_RX_DONE,          // device callback - data ready
    AE_DISCONNECTED,     // device callback - device gone
    AE_TRANSFER_ERROR,
} audio_evt_kind_t;

typedef struct {
    audio_evt_kind_t kind;
    uint8_t addr;
    uint8_t iface_num;
} audio_evt_t;

#define EVT_QUEUE_LEN          16
#define RX_BUF_BYTES           4096
#define INTERNAL_RX_BUF_BYTES  19200   // matches the example's microphone path
#define STATS_PERIOD_MS        1000

static TaskHandle_t s_audio_task = NULL;
static QueueHandle_t s_evt_queue = NULL;
static uac_host_device_handle_t s_uac_dev = NULL;

static volatile uint32_t s_samples_this_period = 0;
static volatile int16_t  s_peak_left  = 0;
static volatile int16_t  s_peak_right = 0;
static int64_t s_period_start_us = 0;

static uint32_t s_sample_freq = 0;  // discovered at open time
static uint8_t  s_channels    = 0;
static uint8_t  s_bit_res     = 0;

static void audio_task(void *arg);
static void uac_lib_event_cb(uint8_t addr, uint8_t iface_num,
                             const uac_host_driver_event_t event, void *arg);
static void uac_dev_event_cb(uac_host_device_handle_t dev_hdl,
                             const uac_host_device_event_t event, void *arg);

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "Audio init (Phase 3.2 - UAC RX, discover params dynamically)");

    s_evt_queue = xQueueCreate(EVT_QUEUE_LEN, sizeof(audio_evt_t));
    if (!s_evt_queue) return ESP_ERR_NO_MEM;

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
        audio_task, "audio_task", 4096, NULL, 5, &s_audio_task, 1);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
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
    if (s_period_start_us == 0) {
        s_period_start_us = now;
        return;
    }
    int64_t elapsed_us = now - s_period_start_us;
    if (elapsed_us < STATS_PERIOD_MS * 1000) return;

    uint32_t samples = s_samples_this_period;
    int16_t  pL = s_peak_left;
    int16_t  pR = s_peak_right;
    s_samples_this_period = 0;
    s_peak_left = 0;
    s_peak_right = 0;
    s_period_start_us = now;

    uint32_t pairs_per_sec = (uint32_t)((uint64_t)samples * 1000000ULL / (uint64_t)elapsed_us);
    ESP_LOGI(TAG, "RX %u pairs/s (target %u), peak L=%d R=%d",
             (unsigned)pairs_per_sec, (unsigned)s_sample_freq,
             (int)pL, (int)pR);
}

// Decode a packed 24-bit little-endian signed PCM sample (3 bytes) into int32_t,
// sign-extended.
static inline int32_t s24_to_s32(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    // sign-extend from 24-bit to 32-bit
    if (u & 0x00800000U) u |= 0xFF000000U;
    return (int32_t)u;
}

static void process_rx(void)
{
    static uint8_t buf[RX_BUF_BYTES];
    uint32_t bytes_read = 0;

    esp_err_t err = uac_host_device_read(s_uac_dev, buf, sizeof(buf),
                                         &bytes_read, 0);
    if (err != ESP_OK || bytes_read == 0) return;

    // QMX delivers 24-bit packed stereo: L[3B] R[3B] = 6 bytes per stereo pair.
    size_t pairs = bytes_read / 6;

    // Track peak as scaled-to-16-bit for display compatibility:
    //   24-bit range is [-8388608 .. +8388607]
    //   shift right by 8 to fit into int16 range [-32768 .. +32767]
    int16_t local_peak_L = s_peak_left;
    int16_t local_peak_R = s_peak_right;

    for (size_t i = 0; i < pairs; i++) {
        const uint8_t *p = buf + 6*i;
        int32_t L = s24_to_s32(p);          // sample L
        int32_t R = s24_to_s32(p + 3);      // sample R
        int32_t aL = (L < 0) ? -L : L;
        int32_t aR = (R < 0) ? -R : R;
        // Scale 24-bit magnitude to 16-bit range for logging readability
        int16_t aL16 = (int16_t)(aL >> 8);
        int16_t aR16 = (int16_t)(aR >> 8);
        if (aL16 > local_peak_L) local_peak_L = aL16;
        if (aR16 > local_peak_R) local_peak_R = aR16;
    }

    s_peak_left = local_peak_L;
    s_peak_right = local_peak_R;
    s_samples_this_period += pairs;
}

// Open the UAC RX device that the lib just told us about, and start streaming.
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

    // Discover what format the interface actually supports.
    // Alt setting 1 is the "active" one (alt 0 = zero-bandwidth state).
    uac_host_dev_alt_param_t alt = {0};
    err = uac_host_get_device_alt_param(s_uac_dev, 1, &alt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_device_alt_param failed: 0x%x", err);
        uac_host_device_close(s_uac_dev);
        s_uac_dev = NULL;
        return err;
    }
    ESP_LOGI(TAG, "Alt 1: channels=%u, %u-bit, sample_freq_type=%u, first_rate=%lu Hz",
             alt.channels, alt.bit_resolution, alt.sample_freq_type,
             (unsigned long)alt.sample_freq[0]);

    const uac_host_stream_config_t stream_cfg = {
        .channels = alt.channels,
        .bit_resolution = alt.bit_resolution,
        .sample_freq = alt.sample_freq[0],   // pick first offered rate
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

    ESP_LOGI(TAG, "UAC stream started: %lu Hz, %u ch, %u-bit",
             (unsigned long)s_sample_freq, s_channels, s_bit_res);
    return ESP_OK;
}

static void audio_task(void *arg)
{
    while (1) {
        audio_evt_t e;
        if (xQueueReceive(s_evt_queue, &e, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (e.kind) {
            case AE_RX_CONNECTED:
                if (s_uac_dev != NULL) {
                    ESP_LOGW(TAG, "Already streaming; ignoring extra RX_CONNECTED");
                    break;
                }
                open_and_start(e.addr, e.iface_num);
                break;

            case AE_RX_DONE:
                process_rx();
                break;

            case AE_TRANSFER_ERROR:
                ESP_LOGW(TAG, "Transfer error reported");
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
        log_stats();
    }
}





