#include "mic_probe.h"

#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"

static const char *TAG = "mic_probe";

/* Same mechanism factory_reset.c uses, and for the same reason: RTC RAM
 * survives esp_restart() but is NOT zero-initialised, so a cold power-on
 * leaves garbage here. The magic word is what makes a request this
 * power-cycle distinguishable from rubbish. */
RTC_NOINIT_ATTR static uint32_t s_probe_magic;
#define PROBE_MAGIC  0x13C7210Au   /* arbitrary sentinel, ES7210-ish */

void mic_probe_request(void)
{
    s_probe_magic = PROBE_MAGIC;
    ESP_LOGW(TAG, "armed - rebooting into the mic probe");
    vTaskDelay(pdMS_TO_TICKS(250));   /* let the HTTP response and log flush */
    esp_restart();
}

#define PROBE_CHANS    4
#define PROBE_RATE_HZ  48000
#define PROBE_MS       1000
#define CHUNK_FRAMES   512

void mic_probe_run_if_pending(void)
{
    if (s_probe_magic != PROBE_MAGIC) return;
    s_probe_magic = 0;          /* consume first: a crash must not loop here */

    ESP_LOGW(TAG, "=== ES7210 CHANNEL PROBE ===");
    ESP_LOGW(TAG, "feed a tone into the 3.5 mm mic ring; a wired channel will "
                  "stand out from the others");

    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    if (!mic) {
        ESP_LOGE(TAG, "microphone init FAILED - cannot probe");
        goto done;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = PROBE_CHANS,
        .channel_mask    = 0,
        .sample_rate     = PROBE_RATE_HZ,
    };
    if (esp_codec_dev_open(mic, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "codec open FAILED - cannot probe");
        goto done;
    }
    /* Mid gain: enough to show a line-level signal without pinning a live
     * microphone at full scale. */
    esp_codec_dev_set_in_gain(mic, 30.0f);

    /* PSRAM. Internal .bss and internal malloc both come out of the DMA pool,
     * and this runs before the pool has been claimed by USB and WiFi - taking
     * it here would just move the shortage. */
    const size_t chunk_bytes = CHUNK_FRAMES * PROBE_CHANS * sizeof(int16_t);
    int16_t *buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "scratch alloc failed");
        goto done;
    }

    double sumsq[PROBE_CHANS] = { 0 };
    int32_t peak[PROBE_CHANS] = { 0 };
    uint32_t frames = 0;
    const uint32_t want = (uint32_t)PROBE_RATE_HZ * PROBE_MS / 1000;

    while (frames < want) {
        if (esp_codec_dev_read(mic, buf, chunk_bytes) != ESP_OK) {
            ESP_LOGE(TAG, "read failed after %u frames", (unsigned)frames);
            break;
        }
        for (int f = 0; f < CHUNK_FRAMES; f++) {
            for (int c = 0; c < PROBE_CHANS; c++) {
                int32_t v = buf[f * PROBE_CHANS + c];
                sumsq[c] += (double)v * (double)v;
                int32_t a = v < 0 ? -v : v;
                if (a > peak[c]) peak[c] = a;
            }
        }
        frames += CHUNK_FRAMES;
    }

    ESP_LOGW(TAG, "--- %u frames @ %d Hz, %d channels ---",
             (unsigned)frames, PROBE_RATE_HZ, PROBE_CHANS);
    for (int c = 0; c < PROBE_CHANS; c++) {
        double rms = frames ? sqrt(sumsq[c] / (double)frames) : 0.0;
        /* dBFS against a full-scale 16-bit sine, so the numbers mean the same
         * thing as everywhere else in this project. */
        double dbfs = rms > 0.0 ? 20.0 * log10(rms / 32768.0) : -999.0;
        ESP_LOGW(TAG, "  ch%d  rms=%8.1f  (%6.1f dBFS)  peak=%6d", c, rms, dbfs, (int)peak[c]);
    }
    ESP_LOGW(TAG, "two of these are the built-in mic array. A channel that "
                  "tracks the injected tone and the others do not is the jack.");

    heap_caps_free(buf);
    esp_codec_dev_close(mic);

done:
    ESP_LOGW(TAG, "=== PROBE DONE - restarting into a normal boot ===");
    vTaskDelay(pdMS_TO_TICKS(400));    /* let the log drain to the capture */
    esp_restart();
}
