// ft8_test.c - Continuous FT8 slot loop with pooled, time-budgeted decode.
//
// Two tasks cooperate so every 15-second FT8 slot is decoded:
//
//   ft8_task (capture)
//     Owns the slot-boundary loop and TX logic. For each RX slot it grabs a
//     free buffer from a small PSRAM pool, captures 15 s of audio into it,
//     posts a decode_job_t to s_decode_queue, and immediately loops back
//     without waiting for decode to finish.
//
//   ft8_decode_task (decode)
//     Waits on the queue, runs monitor-process + candidate-search +
//     decode, records results, releases the buffer back to the pool,
//     advances the QSO state machine, and refreshes the view.
//
// Why a pool + a decode budget (v0.15.13, replacing the old 2-buffer ping-pong):
// on a busy band decode wall-time scales with activity and rivals or exceeds
// the 15 s slot (measured 10-18.8 s, hitting the 140-candidate cap). With only
// two buffers, a slow decode could still own a buffer when capture of the
// slot-after-next came to reuse it - corrupting that slot's audio so it
// produced ~0 decodes. The visible symptom was the decode list filling with
// only ONE slot parity at a time and flipping/emptying whenever an overrun
// shifted the pipeline phase. The fix has two parts that work together:
//   * FT8_NUM_BUFFERS-deep pool: capture never reuses a buffer the decoder
//     still owns (s_buf_busy); transient long slots are absorbed, not corrupted.
//   * FT8_DECODE_BUDGET_MS: the candidate loop stops once the budget is spent
//     (candidates are strongest-first, so only the weakest tail is dropped on
//     the busiest slots). This keeps decode under the slot cadence so the
//     decoder can never fall behind indefinitely on a relentlessly busy band.
//
// TX slots: ft8_task runs ft8_tx_run() instead of capturing; no job is
// queued for that slot (the radio is transmitting, not receiving).
//
// Every other slot is captured - including the parity opposite an armed TX.
// A capture is exactly one slot long (15 s) and ends on the next boundary, so
// the armed burst still fires on time, and capturing the opposite slot is the
// only way to hear the station we're working (they transmit opposite us).

#include "ft8_test.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
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
#include "storage/settings.h"
#include "ui/ui_mode.h"
#include "ui/ft8_screen.h"
#include "ui/ft8_screen_view.h"
#include "ft8_tx.h"
#include "ft8_qso.h"
#include "time_sync.h"
#include "ft8_status.h"

static const char *TAG = "ft8_test";

#define SR_HZ                 12000
#define SLOT_SAMPLES          180000      // 15 s × 12 kHz
#define SLOT_TIMEOUT_MS       20000
#define SNTP_WAIT_TIMEOUT_MS  30000
#define CAT_STATUS_UPDATE_MS  5000

// Capture buffer pool depth. Capture grabs a free buffer each slot and the
// decoder releases it when done, so a slow decode can lag a few slots without
// the next capture overwriting a buffer still in flight. 4 × ~720 KB PSRAM is
// cheap (we boot with >23 MB PSRAM free).
#define FT8_NUM_BUFFERS       4
#define DECODE_QUEUE_DEPTH    FT8_NUM_BUFFERS

// Wall-clock budget for one slot's decode (monitor_process + candidate loop,
// measured from the top of decode_slot). Candidates are processed strongest
// first, so hitting the budget drops only the weakest tail on the busiest
// slots. Must stay safely below the 15 s slot so the decoder can never fall
// behind indefinitely; ~11 s leaves ~4 s/slot of headroom to catch up.
#define FT8_DECODE_BUDGET_MS  11000

// Max LDPC/belief-propagation iterations per candidate. Lowered 60 -> 30
// (v0.15.13): 30 is ample for FT8 (WSJT-X uses a similar range) and roughly
// halves per-candidate cost, so more candidates fit inside the decode budget -
// a net increase in decodes on busy slots.
#define FT8_LDPC_MAX_ITERS    30

#define EPOCH_SANE_MIN        1700000000  // 2023-11-14 - SNTP not synced if below this

// ---------------------------------------------------------------------------
// Decode queue + capture buffer pool
// ---------------------------------------------------------------------------

typedef struct {
    int      buf_idx;   // pool buffer to decode and release; -1 = termination sentinel
    int64_t  slot_sec;  // UTC slot start
    int      slot_idx;
    int      cap_ms;    // measured capture duration, for log line
    int      start_off_ms; // wall-clock at capture start minus the UTC slot
                           // boundary; should stay ~0, drift = the bug
} decode_job_t;

static QueueHandle_t  s_decode_queue = NULL;
static volatile bool  s_ft8_running  = false;

// Capture buffer pool. Allocated in ft8_task. A buffer is owned by capture
// from the moment it's claimed (s_buf_busy[i]=true) until the decoder releases
// it (=false) after fully consuming it. Capture is the only allocator and the
// decoder is the only releaser, so a plain volatile flag array is race-free
// without a mutex.
static float        *s_audio_pool[FT8_NUM_BUFFERS];
static volatile bool s_buf_busy[FT8_NUM_BUFFERS];

// Return the index of a free pool buffer, or -1 if all are in flight.
static int find_free_buffer(void)
{
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        if (!s_buf_busy[i]) return i;
    }
    return -1;
}

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

// No-WiFi (POTA) fallback: derive UTC from the QMX's onboard RTC
// (time-of-day only, "TM;") combined with the last date SNTP gave us
// (persisted to NVS). A stale date is harmless for FT8 slot alignment -
// 86400 s/day is an exact multiple of 15, so unix_sec % 15 is unaffected
// by a date that's off by whole days.
static bool set_time_from_qmx_rtc(void)
{
    int h, m, s;
    if (cat_query_qmx_time(&h, &m, &s) != ESP_OK) return false;
    time_sync_notify_qmx(h, m, s);
    return true;
}

// Blocks until the QMX CAT handshake completes. There is no timeout -
// the QMX may be powered on well after the Tab5 (e.g. persistent FT8
// mode restored at boot before the radio is switched on), so we just
// keep waiting and periodically update the status line so the user
// knows we're still looking.
static void wait_for_cat_ready(void)
{
    int64_t t0 = esp_timer_get_time();
    int64_t last_update = t0;
    while (!cat_is_ready()) {
        int64_t now = esp_timer_get_time();
        if ((now - last_update) / 1000 >= CAT_STATUS_UPDATE_MS) {
            ft8_status_set("Waiting for QMX - check USB/power");
            last_update = now;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "CAT ready (QMX handshake complete)");
}

// Block until the next 15 s slot boundary strictly after after_sec.
// Returns that slot's UTC second. No fixed arrival window - we return
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
// SNR estimation
// ---------------------------------------------------------------------------

// WSJT-X reports SNR referenced to a 2500 Hz noise bandwidth.
#define FT8_SNR_REF_BW_HZ      2500.0f
// Empirical fudge factor - nudge this if a real WSJT-X comparison ever
// becomes available; everything else here is derived from first principles.
#define FT8_SNR_CAL_OFFSET_DB  15.0f

// Mean noise power across the whole slot's waterfall, in dB. Signals occupy a
// small fraction of bins, so the mean tracks the noise floor closely.
//
// THIS IS THE EXPENSIVE PART: a powf() over the entire waterfall (tens of
// thousands of elements). It is identical for every message in the slot, so it
// is computed ONCE per slot and shared - NOT per message. Recomputing it per
// message was the root cause of the parity-skew bug: it made a busy slot's
// decode take ~400 ms × N_decodes (8-18 s for 20-46 decodes), overrunning the
// 15 s slot and corrupting the next capture so the decode list filled with one
// slot parity at a time and flipped/emptied. Hoisted out of the per-message
// path in v0.15.13; per-slot decode now stays at ~2-3 s regardless of how many
// messages decode.
static float ft8_estimate_noise_db(const monitor_t *mon)
{
    const ftx_waterfall_t *wf = &mon->wf;
    int total = wf->num_blocks * wf->block_stride;
    if (total <= 0) {
        return 0.0f;
    }
    double noise_pwr_sum = 0;
    for (int i = 0; i < total; ++i) {
        noise_pwr_sum += powf(10.0f, WF_ELEM_MAG(wf->mag[i]) / 10.0f);
    }
    return 10.0f * log10f((float)(noise_pwr_sum / total));
}

// Estimate a message's SNR (dB, 2500 Hz reference bandwidth) given the slot's
// precomputed noise floor (see ft8_estimate_noise_db). Cheap: only the signal
// loop over 79 symbol blocks × 8 tone bins at the candidate's alignment.
static float ft8_estimate_snr_db(const monitor_t *mon, const ftx_candidate_t *cand,
                                 float noise_db)
{
    const ftx_waterfall_t *wf = &mon->wf;
    int total = wf->num_blocks * wf->block_stride;
    if (total <= 0) {
        return 0.0f;
    }

    int base = ((cand->time_sub * wf->freq_osr) + cand->freq_sub) * wf->num_bins + cand->freq_offset;
    double sig_pwr_sum = 0;
    int sig_n = 0;
    for (int block = 0; block < wf->num_blocks; ++block) {
        float max_mag = -120.0f;
        for (int tone = 0; tone < 8; ++tone) {
            int idx = base + tone * wf->num_bins + block * wf->block_stride;
            if (idx < 0 || idx >= total) {
                continue;
            }
            float m = WF_ELEM_MAG(wf->mag[idx]);
            if (m > max_mag) {
                max_mag = m;
            }
        }
        sig_pwr_sum += powf(10.0f, max_mag / 10.0f);
        sig_n++;
    }
    if (sig_n == 0) {
        return 0.0f;
    }
    float sig_db = 10.0f * log10f((float)(sig_pwr_sum / sig_n));

    float bin_bw_hz = 6.25f / wf->freq_osr;
    float bw_correction_db = 10.0f * log10f(FT8_SNR_REF_BW_HZ / bin_bw_hz);

    return (sig_db - noise_db) - bw_correction_db + FT8_SNR_CAL_OFFSET_DB;
}

// ---------------------------------------------------------------------------
// Decode pipeline - called only from ft8_decode_task
// ---------------------------------------------------------------------------

static void decode_slot(float *audio, monitor_t *mon,
                        int64_t slot_sec, int slot_idx, int cap_ms,
                        int start_off_ms)
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
    int n_attempted = 0;
    // Slot noise floor for SNR: computed once, lazily on the first decode, then
    // reused for every message in the slot. Lazy so 0-decode slots never pay
    // for it. (Hoisting this out of the per-message path is the core fix for the
    // parity-skew bug - see ft8_estimate_noise_db.)
    float slot_noise_db = 0.0f;
    bool  noise_valid   = false;
    for (int i = 0; i < n_cand; i++) {
        // Release the capture buffer on time: stop once the wall-clock budget
        // is spent. cands[] is strongest-first, so we keep the best decodes and
        // drop only the weakest tail on the busiest slots. With the SNR fix this
        // budget is now a safety net (decode runs ~2-3 s), not the usual path.
        if ((int)((esp_timer_get_time() - t_start) / 1000) >= FT8_DECODE_BUDGET_MS) {
            break;
        }
        n_attempted = i + 1;
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon->wf, &cands[i], FT8_LDPC_MAX_ITERS, &msg, &st)) continue;
        char text[FTX_MAX_MESSAGE_LENGTH];
        ftx_message_offsets_t off;
        if (ftx_message_decode(&msg, NULL, text, &off) == FTX_MESSAGE_RC_OK) {
            n_decoded++;
            if (!noise_valid) { slot_noise_db = ft8_estimate_noise_db(mon); noise_valid = true; }
            int snr_db = (int)lroundf(ft8_estimate_snr_db(mon, &cands[i], slot_noise_db));
            ESP_LOGI(TAG, "decoded: '%s' (score=%d freq_off=%d snr=%d)",
                     text, cands[i].score, cands[i].freq_offset, snr_db);
            ft8_screen_record_decode(text, cands[i].score, snr_db, cands[i].freq_offset, slot_sec);
        }
    }
    int n_skipped = n_cand - n_attempted;  // candidates left undecoded when the budget ran out
    int dec_ms = (int)((esp_timer_get_time() - t_start) / 1000) - mon_ms;

    size_t heap_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    size_t heap_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024;

    ft8_status_set("RX: %d decoded  (%d candidates)", n_decoded, n_cand);
    ESP_LOGI(TAG,
        "slot %d UTC %lld: off=%+dms cap=%dms mon=%dms dec=%dms cand=%d dec=%d skip=%d "
        "heap_i=%uKB heap_p=%uKB",
        slot_idx, (long long)slot_sec, start_off_ms,
        cap_ms, mon_ms, dec_ms, n_cand, n_decoded, n_skipped,
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
        if (job.buf_idx < 0) break;     // termination sentinel from capture task
        decode_slot(s_audio_pool[job.buf_idx], mon, job.slot_sec, job.slot_idx,
                    job.cap_ms, job.start_off_ms);
        s_buf_busy[job.buf_idx] = false;   // hand the buffer back to the pool
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

    ft8_status_set("Waiting for QMX...");
    ESP_LOGI(TAG, "waiting for CAT (QMX USB + Q9 1; handshake)...");
    wait_for_cat_ready();

    // QMX GPS/RTC has priority over SNTP: try it first. On a QMX+ the RTC is
    // GPS-disciplined and more accurate than NTP; on a plain QMX it holds the
    // last NTP-synced time pushed by sntp_sync_cb. Either way it is a reliable
    // source for FT8 slot alignment and works without WiFi (POTA).
    ft8_status_set("Getting time from QMX...");
    if (set_time_from_qmx_rtc()) {
        ft8_status_set("Time from QMX GPS/RTC");
        ESP_LOGI(TAG, "time set from QMX RTC/GPS");
    } else {
        // QMX RTC unavailable (TM; not supported or no response) - fall back to SNTP.
        ft8_status_set("Waiting for SNTP sync...");
        ESP_LOGW(TAG, "QMX RTC query failed - waiting for SNTP (up to %d ms)",
                 SNTP_WAIT_TIMEOUT_MS);
        if (!wait_for_sntp(SNTP_WAIT_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "No time source (no QMX GPS/RTC, no SNTP) - check WiFi/QMX");
            ft8_status_set("No time source - check WiFi/QMX");
            vTaskDelete(NULL);
            return;
        }
    }

    // Capture buffer pool (replaces the old 2-buffer ping-pong; see the pool
    // rationale in the file header). Capture claims a free one each slot; the
    // decoder releases it when done.
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        s_audio_pool[i] = heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
        s_buf_busy[i]   = false;
        if (!s_audio_pool[i]) {
            ESP_LOGE(TAG, "PSRAM alloc for audio pool buffer %d/%d failed", i, FT8_NUM_BUFFERS);
            // Free only what this run allocated (0..i-1); entries i..N-1 may hold
            // dangling pointers from a previous FT8 session (this task re-spawns
            // on every mode re-entry), so don't touch them.
            for (int j = 0; j < i; j++) { heap_caps_free(s_audio_pool[j]); s_audio_pool[j] = NULL; }
            vTaskDelete(NULL);
            return;
        }
    }
    ESP_LOGI(TAG, "audio pool: %d buffers x %d floats = %u KB each (%u KB total)",
             FT8_NUM_BUFFERS, SLOT_SAMPLES,
             (unsigned)(SLOT_SAMPLES * sizeof(float) / 1024),
             (unsigned)((size_t)FT8_NUM_BUFFERS * SLOT_SAMPLES * sizeof(float) / 1024));

    s_decode_queue = xQueueCreate(DECODE_QUEUE_DEPTH, sizeof(decode_job_t));
    if (!s_decode_queue) {
        ESP_LOGE(TAG, "failed to create decode queue");
        for (int i = 0; i < FT8_NUM_BUFFERS; i++) heap_caps_free(s_audio_pool[i]);
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
        for (int i = 0; i < FT8_NUM_BUFFERS; i++) heap_caps_free(s_audio_pool[i]);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "entering continuous slot loop (pooled, time-budgeted decode)");

    int slot_idx = 0;
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
            ft8_status_set("TX done - waiting for next slot");
            ft8_screen_view_request_refresh();
        } else {
            // RX this slot. We capture every non-TX slot, including the
            // parity opposite an armed TX: with ping-pong decode a capture is
            // exactly one slot long (15 s) and ends right on the next
            // boundary, so the armed burst still fires on time - and capturing
            // the opposite slot is the only way to hear the station we're
            // working (they transmit on the slot opposite ours). Skipping it
            // would make CQ-replies and QSO responses invisible.
            int bi = find_free_buffer();
            if (bi < 0) {
                // All buffers in flight: the decoder has fallen behind (a run of
                // very busy slots). Skip this capture rather than overwrite a
                // buffer that's still being decoded. The decode time budget
                // makes this rare and self-correcting; log it so a persistent
                // backlog is visible.
                ESP_LOGW(TAG, "slot %d: all %d buffers busy - decoder behind, skipping slot",
                         slot_idx, FT8_NUM_BUFFERS);
                ft8_status_set("RX: decoder catching up...");
                slot_idx++;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            s_buf_busy[bi] = true;
            float *buf = s_audio_pool[bi];

            ft8_status_set("RX: capturing...");
            // Capture-start offset from the UTC slot boundary. Should stay ~0;
            // if it climbs ~0.2 s/slot the capture window is drifting off the
            // FT8 timing grid (the ~3-min decode-death bug).
            struct timeval tv_cap;
            gettimeofday(&tv_cap, NULL);
            int start_off_ms = (int)((tv_cap.tv_sec - slot_sec) * 1000
                                     + tv_cap.tv_usec / 1000);
            // Cap the capture at the next UTC slot boundary so the 15 s window
            // stays anchored to the FT8 grid. Without this the window slides
            // ~0.2 s/slot (capture takes a touch over 15 s) and decoding dies
            // after ~3 min; dsp_ft8_capture zero-pads any shortfall.
            int ms_to_boundary = 15000 - start_off_ms;
            if (ms_to_boundary < 2000)                  ms_to_boundary = 2000;
            if (ms_to_boundary > (int)SLOT_TIMEOUT_MS)  ms_to_boundary = SLOT_TIMEOUT_MS;
            int64_t t0 = esp_timer_get_time();
            esp_err_t e = dsp_ft8_capture(buf, (uint32_t)ms_to_boundary);
            int cap_ms = (int)((esp_timer_get_time() - t0) / 1000);

            if (e == ESP_OK) {
                decode_job_t job = { bi, slot_sec, slot_idx, cap_ms, start_off_ms };
                if (xQueueSend(s_decode_queue, &job, 0) != pdTRUE) {
                    // Shouldn't happen (queue depth == pool size), but if it
                    // does, release the buffer so it isn't lost from the pool.
                    s_buf_busy[bi] = false;
                    ESP_LOGW(TAG, "slot %d: decode queue full - slot dropped", slot_idx);
                }
            } else {
                s_buf_busy[bi] = false;   // capture failed: release the buffer
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
    decode_job_t sentinel = { -1, -1LL, 0, 0, 0 };
    xQueueSend(s_decode_queue, &sentinel, pdMS_TO_TICKS(1000));
    // Clear any stale notification, then wait up to 10 s for decode task exit.
    xTaskNotifyWait(0x01, 0x01, NULL, pdMS_TO_TICKS(10000));

    vQueueDelete(s_decode_queue);
    s_decode_queue = NULL;
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) heap_caps_free(s_audio_pool[i]);

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
