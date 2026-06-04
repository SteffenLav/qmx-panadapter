// ft8_test.c - Step 3 v0.10: Live FT8 RX capture and decode.
//
// One-shot: waits for SNTP, waits for the next 15 s FT8 slot boundary,
// captures 15 s of live audio via dsp_ft8_capture(), then runs the
// same ft8_lib monitor + find_candidates + decode pipeline that step 2c
// validated on synthetic input.
//
// Output: log lines only. UI integration is step 4.
//
// Prerequisites at boot time:
//   - QMX tuned to an FT8 frequency (e.g. 14.074 MHz USB) with audio
//     streaming over USB UAC.
//   - WiFi connected and SNTP synced (typically <5 s after WiFi up).
//
// Capture path: dsp.c performs the fs/4 sign-flip mixer (removes the
// +12 kHz QMX IF) and decimates 48 kHz I/Q to 12 kHz mono real via a
// 31-tap FIR. Result: 180000 floats in PSRAM, ready for ft8_lib.

#include "ft8_test.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "ft8/message.h"
#include "ft8/decode.h"
#include "ft8/constants.h"
#include "common/monitor.h"

#include "dsp.h"
#include "cat/cat.h"

static const char *TAG = "ft8_test";

#define SR_HZ                 12000
#define SLOT_SAMPLES          180000      // 15 s at 12 kHz
#define SLOT_TIMEOUT_MS       20000       // 15 s + headroom
#define SNTP_WAIT_TIMEOUT_MS  30000       // WiFi + SNTP settle budget

// Minimum sane Unix epoch: 2023-11-14. Anything below = SNTP not synced.
#define EPOCH_SANE_MIN        1700000000

static bool wait_for_sntp(uint32_t timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) / 1000 < (int64_t)timeout_ms) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec > EPOCH_SANE_MIN) {
            ESP_LOGI(TAG, "SNTP synced: tv_sec=%lld", (long long)tv.tv_sec);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

// Block until UTC second crosses a 15 s slot boundary within 100 ms.
// Returns the UTC second at which capture is about to start.
static int64_t wait_for_slot_boundary(void)
{
    while (1) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if ((tv.tv_sec % 15) == 0 && tv.tv_usec < 100000) {
            return (int64_t)tv.tv_sec;
        }
        vTaskDelay(1);  // ~10 ms at default tick rate
    }
}

// Block until CAT layer reports the QMX is fully handshaken:
// CDC open -> Q9 1; -> FA -> MD -> FW response parsed.
// Empirically this is the earliest moment we see mode-correct I/Q
// on the USB sound card. Without this wait we would race the boot
// sequence and capture a mix of garbage (raw L/R demodulated SSB)
// and silence before Q9 1; takes effect.
static bool wait_for_cat_ready(uint32_t timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) / 1000 < (int64_t)timeout_ms) {
        if (cat_is_ready()) {
            ESP_LOGI(TAG, "CAT ready (QMX handshake complete)");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

static void ft8_self_test_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Step 3 live FT8 RX: waiting for SNTP sync...");
    if (!wait_for_sntp(SNTP_WAIT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "SNTP did not sync within %d ms - check WiFi",
                 SNTP_WAIT_TIMEOUT_MS);
        vTaskDelete(NULL);
        return;
    }

    // 720 KB slot buffer in PSRAM, allocated once.
    float *audio = heap_caps_malloc(SLOT_SAMPLES * sizeof(float),
                                    MALLOC_CAP_SPIRAM);
    if (!audio) {
        ESP_LOGE(TAG, "PSRAM alloc for slot buffer failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "slot buffer: %d floats at %p (PSRAM)",
             SLOT_SAMPLES, audio);

    ESP_LOGI(TAG, "waiting for CAT (QMX USB + Q9 1; handshake)...");
    if (!wait_for_cat_ready(60000)) {
        ESP_LOGE(TAG, "CAT did not become ready within 60 s - check QMX USB");
        heap_caps_free(audio);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "waiting for next FT8 slot boundary (UTC %% 15 == 0)...");
    int64_t slot_sec = wait_for_slot_boundary();
    ESP_LOGI(TAG, "slot boundary hit at UTC %lld; starting capture",
             (long long)slot_sec);

    int64_t t_cap0 = esp_timer_get_time();
    esp_err_t e = dsp_ft8_capture(audio, SLOT_TIMEOUT_MS);
    int64_t t_cap1 = esp_timer_get_time();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "dsp_ft8_capture failed: %d", e);
        heap_caps_free(audio);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "capture done in %lld ms (%d samples)",
             (t_cap1 - t_cap0) / 1000, SLOT_SAMPLES);

    // Monitor pipeline - identical to step 2c.
    monitor_t mon;
    monitor_config_t cfg = {
        .f_min       = 200.0f,
        .f_max       = 3000.0f,
        .sample_rate = SR_HZ,
        .time_osr    = 2,
        .freq_osr    = 2,
        .protocol    = FTX_PROTOCOL_FT8,
    };
    monitor_init(&mon, &cfg);
    monitor_reset(&mon);

    int blk = mon.block_size;
    int n_blocks_fed = 0;
    for (int pos = 0; pos + blk <= SLOT_SAMPLES; pos += blk) {
        monitor_process(&mon, &audio[pos]);
        n_blocks_fed++;
    }
    int64_t t_mon = esp_timer_get_time();
    ESP_LOGI(TAG, "monitor done in %lld ms, %d blocks fed (wf.num_blocks=%d)",
             (t_mon - t_cap1) / 1000, n_blocks_fed, mon.wf.num_blocks);

    ftx_candidate_t heap[140];
    int n_cand = ftx_find_candidates(&mon.wf, 140, heap, 10);
    int64_t t_cand = esp_timer_get_time();
    ESP_LOGI(TAG, "found %d candidates in %lld ms",
             n_cand, (t_cand - t_mon) / 1000);

    int best_score = -32768, best_idx = -1;
    for (int i = 0; i < n_cand; i++) {
        if (heap[i].score > best_score) {
            best_score = heap[i].score;
            best_idx = i;
        }
    }
    if (best_idx >= 0) {
        ESP_LOGI(TAG, "top candidate: score=%d time_off=%d freq_off=%d",
                 heap[best_idx].score,
                 heap[best_idx].time_offset,
                 heap[best_idx].freq_offset);
    }

    int n_decoded = 0;
    for (int i = 0; i < n_cand; i++) {
        ftx_message_t out_msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon.wf, &heap[i], 60, &out_msg, &st)) {
            continue;
        }
        char text[FTX_MAX_MESSAGE_LENGTH];
        ftx_message_offsets_t off;
        if (ftx_message_decode(&out_msg, NULL, text, &off) == FTX_MESSAGE_RC_OK) {
            n_decoded++;
            ESP_LOGI(TAG, "decoded: '%s' (score=%d freq_off=%d)",
                     text, heap[i].score, heap[i].freq_offset);
        }
    }
    int64_t t_dec = esp_timer_get_time();
    ESP_LOGI(TAG, "decode pass: %d msgs decoded in %lld ms",
             n_decoded, (t_dec - t_cand) / 1000);
    ESP_LOGI(TAG, "Step 3 live capture done. n_cand=%d n_decoded=%d",
             n_cand, n_decoded);

    monitor_free(&mon);
    heap_caps_free(audio);
    vTaskDelete(NULL);
}

// Public entry point: spawn the FT8 task on a dedicated 64 KB stack
// pinned to core 1. monitor_process declares a ~15 KB float frame buffer
// on its stack and kiss_fft.c::kf_work recurses deeper than expected;
// 32 KB was insufficient.
void ft8_self_test(void)
{
    // Stack pinned in PSRAM to keep DMA-capable internal RAM free
    // for USB host endpoint allocation. UAC + CDC both need DMA-able
    // RAM and a 64 KB stack from internal heap starves the CDC claim.
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_self_test_task,
        "ft8_test",
        65536,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn ft8_self_test_task (rc=%d)", (int)rc);
    }
}
