// ft8_test.c - Step 4a v0.10: Continuous FT8 RX slot loop.
//
// Boot-time task: waits for SNTP, waits for CAT-ready (QMX CDC +
// Q9 1; + FA + MD + FW handshake), then loops forever capturing
// each 15 s FT8 slot via dsp_ft8_capture() and running the
// monitor + decode pipeline on the result.
//
// 4a delta vs step 3:
//   - one-shot -> continuous loop
//   - audio buffer + monitor_t hoisted out of the loop (no
//     per-slot heap churn)
//   - decoded messages also recorded into a small ring buffer
//     for future UI use (step 4c)
//   - per-slot summary log includes free internal/PSRAM heap
//     so we can spot leaks across slots
//
// Slot cadence: capture 15 s + monitor ~2 s + decode ~2 s = ~19 s.
// FT8 slots are 15 s apart, so we catch every other slot.
// Ping-pong buffers to overlap decode with next capture are a
// future refactor (tracked in step 4 plan).
//
// Output for 4a is still log lines only. UI mode switch is 4b/4c.

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
#include "ui/ui_mode.h"
#include "ui/ft8_screen.h"
#include "ui/ft8_screen_view.h"
#include "ft8_tx.h"
#include "ft8_qso.h"
#include "ft8_status.h"

static const char *TAG = "ft8_test";

#define SR_HZ                 12000
#define SLOT_SAMPLES          180000      // 15 s at 12 kHz
#define SLOT_TIMEOUT_MS       20000       // 15 s + headroom
#define SNTP_WAIT_TIMEOUT_MS  30000
#define CAT_WAIT_TIMEOUT_MS   60000

// Minimum sane Unix epoch: 2023-11-14. Anything below = SNTP not synced.
#define EPOCH_SANE_MIN        1700000000

// ----- Boot-time gating helpers ---------------------------------------------

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

// Block until CAT layer reports QMX is fully handshaken:
// CDC open -> Q9 1; -> FA -> MD -> FW response parsed.
// Empirically this is the earliest moment we see mode-correct I/Q on USB.
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

// ----- Per-slot processing --------------------------------------------------

// Returns ESP_OK on success, propagates dsp_ft8_capture errors.
// On success, mon already has all 93 blocks fed and is ready for candidate
// search. n_cand_out / n_decoded_out / timing pointers receive stats.
static esp_err_t process_one_slot(float *audio, monitor_t *mon,
                                  int64_t slot_sec,
                                  int *n_cand_out, int *n_decoded_out,
                                  int *cap_ms, int *mon_ms, int *dec_ms)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = dsp_ft8_capture(audio, SLOT_TIMEOUT_MS);
    int64_t t_cap = esp_timer_get_time();
    *cap_ms = (int)((t_cap - t0) / 1000);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "slot UTC %lld: capture failed (%d)",
                 (long long)slot_sec, e);
        return e;
    }

    monitor_reset(mon);
    int blk = mon->block_size;
    int n_blocks_fed = 0;
    for (int pos = 0; pos + blk <= SLOT_SAMPLES; pos += blk) {
        monitor_process(mon, &audio[pos]);
        n_blocks_fed++;
    }
    int64_t t_mon = esp_timer_get_time();
    *mon_ms = (int)((t_mon - t_cap) / 1000);

    ftx_candidate_t heap[140];
    int n_cand = ftx_find_candidates(&mon->wf, 140, heap, 10);
    *n_cand_out = n_cand;

    int n_decoded = 0;
    for (int i = 0; i < n_cand; i++) {
        ftx_message_t out_msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon->wf, &heap[i], 60, &out_msg, &st)) {
            continue;
        }
        char text[FTX_MAX_MESSAGE_LENGTH];
        ftx_message_offsets_t off;
        if (ftx_message_decode(&out_msg, NULL, text, &off) == FTX_MESSAGE_RC_OK) {
            n_decoded++;
            ESP_LOGI(TAG, "decoded: '%s' (score=%d freq_off=%d)",
                     text, heap[i].score, heap[i].freq_offset);
            ft8_screen_record_decode(text,
                                     heap[i].score, heap[i].freq_offset,
                                     slot_sec);
        }
    }
    int64_t t_dec = esp_timer_get_time();
    *dec_ms = (int)((t_dec - t_mon) / 1000);
    *n_decoded_out = n_decoded;
    return ESP_OK;
}

// ----- Task ------------------------------------------------------------------

static void ft8_task(void *arg)
{
    (void)arg;

    ft8_status_set("Waiting for SNTP sync...");
    ESP_LOGI(TAG, "Step 4a continuous FT8 RX: waiting for SNTP sync...");
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

    // Reused across all slots; allocated once.
    float *audio = heap_caps_malloc(SLOT_SAMPLES * sizeof(float),
                                    MALLOC_CAP_SPIRAM);
    if (!audio) {
        ESP_LOGE(TAG, "PSRAM alloc for slot buffer failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "slot buffer: %d floats at %p (PSRAM, reused per slot)",
             SLOT_SAMPLES, audio);

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

    ESP_LOGI(TAG, "entering continuous slot loop");

    int slot_idx = 0;
    while (ui_mode_get() == UI_MODE_FT8) {
        ESP_LOGI(TAG, "slot %d: waiting for next FT8 boundary...", slot_idx);
        int64_t slot_sec = wait_for_slot_boundary();

        // v0.12.0: a slot is either RX (decode, as always) or - if a TX
        // request is armed and its required parity matches (or it's a CQ
        // call firing on the next boundary) - a ~12.7s CAT burst. These are
        // structurally exclusive: while keyed up, the QMX's USB audio
        // captures its own TX output, not the air, so running both in the
        // same slot would be meaningless even if it were safe (it isn't -
        // they'd fight over the same audio ring buffer and CDC-ACM link).
        ft8_tx_request_t txreq;
        if (ft8_tx_should_run_this_slot(slot_sec, &txreq)) {
            ft8_status_set("TX: %s", txreq.display_text);
            ft8_tx_run(&txreq);   // blocks ~12.7s; always restores RX before returning
            ft8_status_set("TX done — waiting for next slot");
        } else {
            int n_cand = 0, n_decoded = 0;
            int cap_ms = 0, mon_ms = 0, dec_ms = 0;

            ft8_status_set("RX: capturing...");
            esp_err_t e = process_one_slot(audio, &mon, slot_sec,
                                           &n_cand, &n_decoded,
                                           &cap_ms, &mon_ms, &dec_ms);

            size_t heap_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
            size_t heap_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024;

            if (e == ESP_OK) {
                ft8_status_set("RX: %d decoded  (%d candidates)", n_decoded, n_cand);
                ESP_LOGI(TAG,
                    "slot %d UTC %lld: cap=%dms mon=%dms dec=%dms "
                    "cand=%d dec=%d heap_i=%uKB heap_p=%uKB",
                    slot_idx, (long long)slot_sec,
                    cap_ms, mon_ms, dec_ms, n_cand, n_decoded,
                    (unsigned)heap_i, (unsigned)heap_p);
                ft8_qso_advance(slot_sec);
            } else {
                ft8_status_set("RX: capture error");
                ESP_LOGW(TAG,
                    "slot %d UTC %lld: error %d after cap=%dms heap_i=%uKB heap_p=%uKB",
                    slot_idx, (long long)slot_sec, e, cap_ms,
                    (unsigned)heap_i, (unsigned)heap_p);
            }
        }

        // Step 4c.2: tell the FT8 LVGL view a slot finished so it can
        // snapshot the decode table and rebuild the list.
        ft8_screen_view_request_refresh();
        slot_idx++;
        // Brief yield - slot boundary wait will dominate.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Mode switched away from FT8 (4c will trigger this from UI).
    // Free monitor's internal allocations (~200 KB internal heap:
    // waterfall mag buffer + fft_work + window + last_frame) and the
    // slot buffer (~720 KB PSRAM), then exit. Respawn on next FT8 entry.
    ESP_LOGI(TAG, "ft8_task exiting (mode changed); processed %d slots", slot_idx);
    monitor_free(&mon);
    heap_caps_free(audio);
    vTaskDelete(NULL);
}

// Public entry point: spawn the FT8 task on a 64 KB stack in PSRAM,
// pinned to core 1.
void ft8_self_test(void)
{
    // Stack pinned in PSRAM to keep DMA-capable internal RAM free
    // for USB host endpoint allocation. UAC + CDC both need DMA-able
    // RAM and a 64 KB stack from internal heap starves the CDC claim.
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_task,
        "ft8",
        65536,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn ft8_task (rc=%d)", (int)rc);
    }
}
