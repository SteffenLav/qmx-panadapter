// ft8_test.c — Continuous FT8 slot loop with ping-pong decode.
//
// Two tasks cooperate so every 15-second FT8 slot is decoded:
//
//   ft8_task (capture)
//     Owns the slot-boundary loop and TX logic. For each RX slot it
//     captures 15 s of audio into one of two alternating PSRAM buffers,
//     posts a decode_job_t to s_decode_queue, and immediately loops back
//     without waiting for decode to finish.
//
//   ft8_decode_task (decode)
//     Waits on the queue, runs monitor-process + candidate-search +
//     decode (~4 s), records results, advances the QSO state machine,
//     and refreshes the view.
//
// Because decode (~4 s) finishes well before the next capture completes
// (15 s), the queue never backs up and every slot is decoded regardless
// of parity.
//
// TX slots: ft8_task runs ft8_tx_run() instead of capturing; no job is
// queued for that slot (the radio is transmitting, not receiving).
//
// Every other slot is captured — including the parity opposite an armed TX.
// A capture is exactly one slot long (15 s) and ends on the next boundary, so
// the armed burst still fires on time, and capturing the opposite slot is the
// only way to hear the station we're working (they transmit opposite us).

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
#include "freertos/queue.h"
#include "freertos/idf_additions.h"

#include "ft8/message.h"
#include "ft8/decode.h"
#include "ft8/constants.h"
#include "common/monitor.h"

#include "dsp.h"
#include "cat/cat.h"
#include "ui/ui_mode.h"
#include "ui/ft8_screen.h"
#include "ui/ft8_screen_view.h"
#include "ft8_tx.h"
#include "ft8_qso.h"
#include "ft8_status.h"

static const char *TAG = "ft8_test";

#define SR_HZ                 12000
#define SLOT_SAMPLES          180000      // 15 s × 12 kHz
#define SLOT_TIMEOUT_MS       20000
#define SNTP_WAIT_TIMEOUT_MS  30000
#define CAT_WAIT_TIMEOUT_MS   60000
#define DECODE_QUEUE_DEPTH    2           // one in-flight + one ready, never backs up

#define EPOCH_SANE_MIN        1700000000  // 2023-11-14 — SNTP not synced if below this

// ---------------------------------------------------------------------------
// Ping-pong decode queue
// ---------------------------------------------------------------------------

typedef struct {
    float   *audio;     // which ping-pong buffer to decode (NULL = sentinel)
    int64_t  slot_sec;  // UTC slot start; -1 = termination sentinel
    int      slot_idx;
    int      cap_ms;    // measured capture duration, for log line
} decode_job_t;

static QueueHandle_t  s_decode_queue = NULL;
static volatile bool  s_ft8_running  = false;

// ---------------------------------------------------------------------------
// Boot-time gating helpers
// ---------------------------------------------------------------------------

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

// Block until the next 15 s slot boundary strictly after after_sec.
// Returns that slot's UTC second. No fixed arrival window — we return
// the current slot if it's newer than after_sec, which handles the
// case where the previous capture ran a little long and we land
// 100+ ms into the new slot.
static int64_t wait_for_slot_boundary(int64_t after_sec)
{
    while (1) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t slot = (int64_t)(tv.tv_sec / 15) * 15;
        if (slot > after_sec) {
            return slot;
        }
        vTaskDelay(1);  // ~10 ms at default tick rate
    }
}

// ---------------------------------------------------------------------------
// Decode pipeline — called only from ft8_decode_task
// ---------------------------------------------------------------------------

static void decode_slot(float *audio, monitor_t *mon,
                        int64_t slot_sec, int slot_idx, int cap_ms)
{
    int64_t t_start = esp_timer_get_time();

    monitor_reset(mon);
    int blk = mon->block_size;
    for (int pos = 0; pos + blk <= SLOT_SAMPLES; pos += blk) {
        monitor_process(mon, &audio[pos]);
    }
    int mon_ms = (int)((esp_timer_get_time() - t_start) / 1000);

    ftx_candidate_t cands[140];
    int n_cand = ftx_find_candidates(&mon->wf, 140, cands, 10);

    int n_decoded = 0;
    for (int i = 0; i < n_cand; i++) {
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon->wf, &cands[i], 60, &msg, &st)) continue;
        char text[FTX_MAX_MESSAGE_LENGTH];
        ftx_message_offsets_t off;
        if (ftx_message_decode(&msg, NULL, text, &off) == FTX_MESSAGE_RC_OK) {
            n_decoded++;
            ESP_LOGI(TAG, "decoded: '%s' (score=%d freq_off=%d)",
                     text, cands[i].score, cands[i].freq_offset);
            ft8_screen_record_decode(text, cands[i].score, cands[i].freq_offset, slot_sec);
        }
    }
    int dec_ms = (int)((esp_timer_get_time() - t_start) / 1000) - mon_ms;

    size_t heap_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    size_t heap_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024;

    ft8_status_set("RX: %d decoded  (%d candidates)", n_decoded, n_cand);
    ESP_LOGI(TAG,
        "slot %d UTC %lld: cap=%dms mon=%dms dec=%dms cand=%d dec=%d "
        "heap_i=%uKB heap_p=%uKB",
        slot_idx, (long long)slot_sec,
        cap_ms, mon_ms, dec_ms, n_cand, n_decoded,
        (unsigned)heap_i, (unsigned)heap_p);

    ft8_qso_advance(slot_sec);
    ft8_screen_view_request_refresh();
}

// ---------------------------------------------------------------------------
// Decode task
// ---------------------------------------------------------------------------

static void ft8_decode_task(void *arg)
{
    TaskHandle_t notify_target = (TaskHandle_t)arg;

    monitor_t *mon = heap_caps_malloc(sizeof(monitor_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mon) {
        ESP_LOGE(TAG, "decode task: monitor_t alloc failed");
        if (notify_target) xTaskNotify(notify_target, 1, eSetBits);
        vTaskDelete(NULL);
        return;
    }

    monitor_config_t cfg = {
        .f_min       = 200.0f,
        .f_max       = 3000.0f,
        .sample_rate = SR_HZ,
        .time_osr    = 2,
        .freq_osr    = 2,
        .protocol    = FTX_PROTOCOL_FT8,
    };
    monitor_init(mon, &cfg);
    ESP_LOGI(TAG, "decode task ready");

    while (true) {
        decode_job_t job;
        if (xQueueReceive(s_decode_queue, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
            if (!s_ft8_running) break;
            continue;
        }
        if (job.slot_sec < 0) break;    // termination sentinel from capture task
        decode_slot(job.audio, mon, job.slot_sec, job.slot_idx, job.cap_ms);
    }

    monitor_free(mon);
    heap_caps_free(mon);
    ESP_LOGI(TAG, "decode task exiting");
    if (notify_target) xTaskNotify(notify_target, 1, eSetBits);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Capture task
// ---------------------------------------------------------------------------

static void ft8_task(void *arg)
{
    (void)arg;

    ft8_status_set("Waiting for SNTP sync...");
    ESP_LOGI(TAG, "waiting for SNTP...");
    if (!wait_for_sntp(SNTP_WAIT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "SNTP did not sync within %d ms - check WiFi",
                 SNTP_WAIT_TIMEOUT_MS);
        ft8_status_set("SNTP timeout — check WiFi");
        vTaskDelete(NULL);
        return;
    }

    ft8_status_set("Waiting for QMX...");
    ESP_LOGI(TAG, "waiting for CAT (QMX USB + Q9 1; handshake)...");
    if (!wait_for_cat_ready(CAT_WAIT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "CAT did not become ready within %d ms - check QMX USB",
                 CAT_WAIT_TIMEOUT_MS);
        ft8_status_set("QMX not found — check USB");
        vTaskDelete(NULL);
        return;
    }

    // Two ping-pong audio buffers — one captures while the other decodes.
    float *audio[2] = {
        heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM),
        heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM),
    };
    if (!audio[0] || !audio[1]) {
        ESP_LOGE(TAG, "PSRAM alloc for audio ping-pong buffers failed");
        heap_caps_free(audio[0]);
        heap_caps_free(audio[1]);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "audio ping-pong: [%p / %p]  %d floats = %u KB each",
             audio[0], audio[1], SLOT_SAMPLES,
             (unsigned)(SLOT_SAMPLES * sizeof(float) / 1024));

    s_decode_queue = xQueueCreate(DECODE_QUEUE_DEPTH, sizeof(decode_job_t));
    if (!s_decode_queue) {
        ESP_LOGE(TAG, "failed to create decode queue");
        heap_caps_free(audio[0]);
        heap_caps_free(audio[1]);
        vTaskDelete(NULL);
        return;
    }

    // Spawn decode task, passing our handle so it can notify us on exit.
    s_ft8_running = true;
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_decode_task, "ft8_dec", 65536,
        xTaskGetCurrentTaskHandle(),
        tskIDLE_PRIORITY + 1, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn ft8_decode_task (rc=%d)", (int)rc);
        s_ft8_running = false;
        vQueueDelete(s_decode_queue);
        s_decode_queue = NULL;
        heap_caps_free(audio[0]);
        heap_caps_free(audio[1]);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "entering continuous slot loop (ping-pong decode)");

    int slot_idx = 0;
    int buf_idx  = 0;
    struct timeval tv_init;
    gettimeofday(&tv_init, NULL);
    int64_t last_slot = (int64_t)(tv_init.tv_sec / 15) * 15;

    while (ui_mode_get() == UI_MODE_FT8) {
        ESP_LOGI(TAG, "slot %d: waiting for next FT8 boundary...", slot_idx);
        int64_t slot_sec = wait_for_slot_boundary(last_slot);
        last_slot = slot_sec;

        ft8_tx_request_t txreq;
        if (ft8_tx_should_run_this_slot(slot_sec, &txreq)) {
            ft8_status_set("TX: %s", txreq.display_text);
            ft8_tx_run(&txreq);   // blocks ~12.7 s; always restores RX before returning
            ft8_qso_on_tx_complete();  // re-arm the current outgoing message
            ft8_status_set("TX done — waiting for next slot");
            ft8_screen_view_request_refresh();
        } else {
            // RX this slot. We capture every non-TX slot, including the
            // parity opposite an armed TX: with ping-pong decode a capture is
            // exactly one slot long (15 s) and ends right on the next
            // boundary, so the armed burst still fires on time — and capturing
            // the opposite slot is the only way to hear the station we're
            // working (they transmit on the slot opposite ours). Skipping it
            // would make CQ-replies and QSO responses invisible.
            float *buf = audio[buf_idx];
            buf_idx ^= 1;   // alternate buffers: 0→1→0→1...

            ft8_status_set("RX: capturing...");
            int64_t t0 = esp_timer_get_time();
            esp_err_t e = dsp_ft8_capture(buf, SLOT_TIMEOUT_MS);
            int cap_ms = (int)((esp_timer_get_time() - t0) / 1000);

            if (e == ESP_OK) {
                decode_job_t job = { buf, slot_sec, slot_idx, cap_ms };
                if (xQueueSend(s_decode_queue, &job, 0) != pdTRUE) {
                    // Should never happen: decode takes ~4 s, capture takes 15 s
                    ESP_LOGW(TAG, "slot %d: decode queue full — slot dropped", slot_idx);
                }
            } else {
                ft8_status_set("RX: capture error");
                ESP_LOGW(TAG, "slot %d UTC %lld: capture failed (%d)",
                         slot_idx, (long long)slot_sec, e);
            }
        }

        slot_idx++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Signal decode task to stop, then wait for it to drain and exit.
    s_ft8_running = false;
    decode_job_t sentinel = { NULL, -1LL, 0, 0 };
    xQueueSend(s_decode_queue, &sentinel, pdMS_TO_TICKS(1000));
    // Clear any stale notification, then wait up to 10 s for decode task exit.
    xTaskNotifyWait(0x01, 0x01, NULL, pdMS_TO_TICKS(10000));

    vQueueDelete(s_decode_queue);
    s_decode_queue = NULL;
    heap_caps_free(audio[0]);
    heap_caps_free(audio[1]);

    ESP_LOGI(TAG, "ft8_task exiting; processed %d slots", slot_idx);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public entry point: spawn the capture task
// ---------------------------------------------------------------------------

void ft8_self_test(void)
{
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_task, "ft8", 65536, NULL,
        tskIDLE_PRIORITY + 1, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn ft8_task (rc=%d)", (int)rc);
    }
}
