// ft8_test.c - Continuous FT8 slot loop with pooled, time-budgeted decode.
//
// Two tasks + a decode helper cooperate so every 15-second FT8 slot is decoded
// as fast as possible - aiming to finish the decode BEFORE the slot boundary,
// which a PC running WSJT-X never bothers to do (it idles until t=15 s).
//
//   ft8_task (capture + streaming STFT)
//     Owns the slot-boundary loop and TX logic. For each RX slot it claims a
//     free monitor_t from a small PSRAM pool, then captures audio AND builds
//     that monitor's waterfall block-by-block AS the audio arrives (streaming
//     STFT). An FT8 burst is only 12.64 s of a 15 s slot, so by the time the
//     signal ends the waterfall is already fully built - the STFT cost is
//     overlapped with capture instead of paid afterwards. It posts a
//     decode_job_t (the monitor index) and loops back without waiting.
//
//   ft8_decode_task (decode, core 1) + ft8_decode_worker_task (core 0)
//     The decode task pulls a job, runs candidate-search on the pre-built
//     waterfall, then fans the (independent, reentrant) LDPC candidate loop
//     across BOTH P4 cores: it takes the even-indexed candidates itself while
//     the core-0 helper takes the odd ones, joins on a completion semaphore,
//     merges results, records decodes, advances the QSO state machine, and
//     releases the monitor back to the pool.
//
// Pool + decode budget (carried over from v0.15.13's parity-skew fix): capture
// claims a monitor (s_buf_busy) and the decoder releases it, so capture never
// reuses a waterfall the decoder still owns. FT8_DECODE_BUDGET_MS bounds the
// per-worker candidate loop (strongest-first, so only the weakest tail is
// dropped on the busiest slots) - now a safety net rather than the usual path,
// since streaming STFT + dual-core decode keep a slot's work well under 15 s.
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

// Max FT8 candidates considered per slot (matches the cands[] buffer in
// decode_slot) - also the upper bound on each worker's timing-sample array.
#define FT8_MAX_CANDIDATES    140

// Monitor pool depth. Capture claims a free monitor each slot (holding it for
// the whole 15 s while it streams the STFT in) and the decoder releases it when
// done. At most TWO are ever in use - one capturing, one decoding - and decode
// now finishes in ~2 s (dual-core), long before the next capture needs a third,
// so 2 is sufficient. Each monitor's waterfall is ~166 KB (PSRAM), but its
// window+last_frame (~15 KB each) fall under the 16 KB spill threshold and land
// in INTERNAL RAM, so keeping the count minimal matters: 3 monitors left only
// ~9 KB internal free; 2 restores comfortable headroom.
#define FT8_NUM_BUFFERS       2
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
    int      mon_idx;   // monitor pool slot to decode and release; -1 = termination sentinel
    int64_t  slot_sec;  // UTC slot start
    int      slot_idx;
    int      cap_ms;    // capture wall-clock duration (incl. streaming STFT), for log
    int      stft_ms;   // STFT compute time, overlapped with capture (should be small)
    int      start_off_ms; // wall-clock at capture start minus the UTC slot
                           // boundary; should stay ~0, drift = the bug
} decode_job_t;

static QueueHandle_t  s_decode_queue = NULL;
static volatile bool  s_ft8_running  = false;

// Timing error from the last decoded slot: positive = system clock is fast.
// Written by ft8_decode_task; read by the LVGL UI (ft8_time_modal).
// volatile int is sufficient — single writer, single reader, display hint only.
static volatile int      s_last_timing_ms    = 0;
static volatile bool     s_last_timing_valid = false;
// Bumped every time s_last_timing_ms is updated from a genuine decode, so
// the UI can detect "a new sync just happened" even though it polls at 1 Hz
// while decodes land roughly every 15 s.
static volatile uint32_t s_timing_seq        = 0;

bool ft8_get_last_timing_ms(int *out_ms)
{
    if (!s_last_timing_valid) return false;
    *out_ms = s_last_timing_ms;
    return true;
}

uint32_t ft8_get_timing_seq(void)
{
    return s_timing_seq;
}

// Monitor pool. Allocated + monitor_init'd in ft8_task. A monitor is owned by
// capture from the moment it's claimed (s_buf_busy[i]=true) - capture streams
// the STFT into its waterfall over the whole slot - until the decoder releases
// it (=false) after fully consuming it. Capture is the only allocator and the
// decoder is the only releaser, so a plain volatile flag array is race-free
// without a mutex.
static monitor_t    *s_mon_pool[FT8_NUM_BUFFERS];
static volatile bool s_buf_busy[FT8_NUM_BUFFERS];
// Single reusable capture scratch buffer (decimated 12 kHz audio). Consumed by
// the streaming STFT during capture; not needed once the waterfall is built, so
// one buffer serves every slot (capture is strictly sequential).
static float        *s_cap_scratch = NULL;

// Return the index of a free pool monitor, or -1 if all are in flight.
static int find_free_buffer(void)
{
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        if (!s_buf_busy[i]) return i;
    }
    return -1;
}

// Free the monitor pool + capture scratch (idempotent; safe on partial alloc).
static void free_capture_pool(void)
{
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        if (s_mon_pool[i]) {
            monitor_free(s_mon_pool[i]);
            heap_caps_free(s_mon_pool[i]);
            s_mon_pool[i] = NULL;
        }
    }
    if (s_cap_scratch) { heap_caps_free(s_cap_scratch); s_cap_scratch = NULL; }
}

// ---------------------------------------------------------------------------
// Dual-core decode (Stage 2): a persistent helper on core 0 decodes the
// odd-indexed candidates while the decode task (core 1) takes the even ones.
// ftx_decode_candidate is reentrant (const waterfall, stack-local work) and
// ft8_screen_record_decode is mutex-protected, so the two run safely against
// one shared monitor. See decode_slot for the fan-out/join.
// ---------------------------------------------------------------------------
typedef struct {
    int   n_decoded;
    int   n_attempted;
    float timing[FT8_MAX_CANDIDATES];  // one sample per decoded candidate
    int   n_timing;
} decode_result_t;

typedef struct {
    monitor_t              *mon;
    const ftx_candidate_t  *cands;
    int                     n_cand;
    float                   noise_db;
    int64_t                 slot_sec;
    int64_t                 t_start_us;   // shared budget anchor
    int                     start_off_ms;
    int                     start;        // first candidate index
    int                     step;         // index stride (2 for the dual-core split)
    decode_result_t        *result;
} worker_job_t;

static QueueHandle_t     s_worker_queue = NULL;  // carries worker_job_t*
static SemaphoreHandle_t s_worker_done  = NULL;

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

// Timing samples within this many ms of the median are treated as "in sync"
// and averaged; anything further out is a false sync / mis-decode outlier and
// is dropped. 60 ms is a bit under half an FT8 symbol period (160 ms), wide
// enough to cover normal propagation/clock jitter between genuine stations
// while still rejecting a sync hit that landed on the wrong symbol entirely.
#define FT8_TIMING_OUTLIER_MS 60.0f

// Robust average of per-candidate timing samples: sorts a (small) working
// copy, takes the median, then means only the samples within
// FT8_TIMING_OUTLIER_MS of it. A single false-decode outlier no longer
// swings the result - that was the cause of the "SS run-away" in the
// time-sync modal, since one bad sample would sit there (held until the next
// decode) looking like a wild jump.
static float robust_mean_timing_ms(float *vals, int n)
{
    for (int i = 1; i < n; i++) {
        float key = vals[i];
        int j = i - 1;
        while (j >= 0 && vals[j] > key) { vals[j + 1] = vals[j]; j--; }
        vals[j + 1] = key;
    }
    float median = (n % 2) ? vals[n / 2] : (vals[n / 2 - 1] + vals[n / 2]) * 0.5f;

    double sum = 0;
    int    cnt = 0;
    for (int i = 0; i < n; i++) {
        if (fabsf(vals[i] - median) <= FT8_TIMING_OUTLIER_MS) {
            sum += vals[i];
            cnt++;
        }
    }
    return cnt > 0 ? (float)(sum / cnt) : median;
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
// Decode pipeline - called only from ft8_decode_task (+ its core-0 helper)
// ---------------------------------------------------------------------------

// Decode candidates [start, n_cand) with the given stride, recording each
// successful decode and collecting its timing sample into *out. Reentrant: it
// reads only the (const) waterfall and writes to its own result struct plus the
// mutex-protected decode list, so the decode task and the core-0 helper run it
// concurrently against one shared monitor. The shared wall-clock budget
// (t_start_us) bounds the busiest slots; candidates are strongest-first so only
// the weakest tail is dropped.
static void decode_candidate_range(monitor_t *mon, const ftx_candidate_t *cands,
                                   int n_cand, int start, int step,
                                   float noise_db, int64_t slot_sec,
                                   int64_t t_start_us, int start_off_ms,
                                   decode_result_t *out)
{
    out->n_decoded   = 0;
    out->n_attempted = 0;
    out->n_timing    = 0;
    for (int i = start; i < n_cand; i += step) {
        if ((int)((esp_timer_get_time() - t_start_us) / 1000) >= FT8_DECODE_BUDGET_MS) {
            break;
        }
        out->n_attempted++;
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon->wf, &cands[i], FT8_LDPC_MAX_ITERS, &msg, &st)) continue;
        char text[FTX_MAX_MESSAGE_LENGTH];
        ftx_message_offsets_t off;
        if (ftx_message_decode(&msg, NULL, text, &off) == FTX_MESSAGE_RC_OK) {
            out->n_decoded++;
            // SR=12000, block_size=1920 samples/symbol, time_osr=2 -> subblock=960
            // samples. start_off_ms anchors this candidate's sync position back to
            // the UTC slot boundary, same as every other candidate this slot.
            if (out->n_timing < FT8_MAX_CANDIDATES) {
                out->timing[out->n_timing++] =
                    (float)start_off_ms + (cands[i].time_offset * 1920 + cands[i].time_sub * 960) / 12.0f;
            }
            int snr_db = (int)lroundf(ft8_estimate_snr_db(mon, &cands[i], noise_db));
            ESP_LOGI(TAG, "decoded: '%s' (score=%d freq_off=%d snr=%d)",
                     text, cands[i].score, cands[i].freq_offset, snr_db);
            ft8_screen_record_decode(text, cands[i].score, snr_db, cands[i].freq_offset, slot_sec);
        }
    }
}

// Persistent core-0 decode helper: waits for one sub-range job, decodes it,
// signals completion. Only one job is ever in flight (the decode task blocks on
// s_worker_done before issuing the next).
static void ft8_decode_worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "decode worker ready (core %d)", xPortGetCoreID());
    while (true) {
        worker_job_t *job = NULL;
        if (xQueueReceive(s_worker_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (!job) break;   // termination sentinel
        decode_candidate_range(job->mon, job->cands, job->n_cand, job->start, job->step,
                               job->noise_db, job->slot_sec, job->t_start_us,
                               job->start_off_ms, job->result);
        xSemaphoreGive(s_worker_done);
    }
    ESP_LOGI(TAG, "decode worker exiting");
    vTaskDelete(NULL);
}

// Decode one slot's PRE-BUILT waterfall (the STFT was streamed in during
// capture). Candidate search, then dual-core LDPC fan-out, merge, record,
// advance the QSO state machine.
static void decode_slot(monitor_t *mon, int64_t slot_sec, int slot_idx,
                        int cap_ms, int stft_ms, int start_off_ms)
{
    int64_t t_start = esp_timer_get_time();

    ftx_candidate_t cands[FT8_MAX_CANDIDATES];
    int n_cand = ftx_find_candidates(&mon->wf, FT8_MAX_CANDIDATES, cands, 10);

    // Slot noise floor for SNR: one powf sweep over the whole waterfall, shared
    // by every message (hoisting this out of the per-message path was the
    // v0.15.13 parity-skew fix). Computed eagerly before the fan-out so both
    // workers read it without synchronising.
    float noise_db = (n_cand > 0) ? ft8_estimate_noise_db(mon) : 0.0f;

    // Fan out across both cores: the helper (core 0) takes odd candidates, we
    // take even. Both share the same const waterfall (read-only) and the
    // mutex-protected decode list. r_worker is static (one helper, one slot at a
    // time); r_main is a stack local. We block on s_worker_done before reading
    // r_worker, so neither result struct is ever touched concurrently.
    static decode_result_t r_worker;
    decode_result_t r_main;

    worker_job_t job = {
        .mon = mon, .cands = cands, .n_cand = n_cand, .noise_db = noise_db,
        .slot_sec = slot_sec, .t_start_us = t_start, .start_off_ms = start_off_ms,
        .start = 1, .step = 2, .result = &r_worker,
    };
    bool dispatched = false;
    if (s_worker_queue && n_cand > 1) {
        worker_job_t *jp = &job;   // job stays valid: we block on s_worker_done below
        if (xQueueSend(s_worker_queue, &jp, 0) == pdTRUE) dispatched = true;
    }

    // Our half (even indices).
    decode_candidate_range(mon, cands, n_cand, 0, 2, noise_db, slot_sec,
                           t_start, start_off_ms, &r_main);

    if (dispatched) {
        xSemaphoreTake(s_worker_done, portMAX_DELAY);
    } else {
        // No helper (or <=1 candidate): decode the odd half inline too.
        decode_candidate_range(mon, cands, n_cand, 1, 2, noise_db, slot_sec,
                               t_start, start_off_ms, &r_worker);
    }

    int n_decoded   = r_main.n_decoded   + r_worker.n_decoded;
    int n_attempted = r_main.n_attempted + r_worker.n_attempted;
    int n_skipped   = n_cand - n_attempted;  // candidates left undecoded if the budget ran out

    // Merge both halves' timing samples, then robust (outlier-rejecting) average
    // into the system-clock error estimate. Positive = clock fast; negative = slow.
    float timing_ms_arr[FT8_MAX_CANDIDATES];
    int   n_timing = 0;
    for (int i = 0; i < r_main.n_timing   && n_timing < FT8_MAX_CANDIDATES; i++)
        timing_ms_arr[n_timing++] = r_main.timing[i];
    for (int i = 0; i < r_worker.n_timing && n_timing < FT8_MAX_CANDIDATES; i++)
        timing_ms_arr[n_timing++] = r_worker.timing[i];
    if (n_timing > 0) {
        s_last_timing_ms    = (int)roundf(robust_mean_timing_ms(timing_ms_arr, n_timing));
        s_last_timing_valid = true;
        s_timing_seq++;
    }

    int dec_ms = (int)((esp_timer_get_time() - t_start) / 1000);

    size_t heap_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    size_t heap_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024;

    ft8_status_set("RX: %d decoded  (%d candidates)", n_decoded, n_cand);
    ESP_LOGI(TAG,
        "slot %d UTC %lld: off=%+dms cap=%dms stft=%dms dec=%dms cand=%d dec=%d skip=%d "
        "heap_i=%uKB heap_p=%uKB",
        slot_idx, (long long)slot_sec, start_off_ms,
        cap_ms, stft_ms, dec_ms, n_cand, n_decoded, n_skipped,
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

    // Dual-core decode helper: a 1-deep job queue + completion semaphore + a
    // worker pinned to core 0. In FT8 mode core 0 runs only the (light) UAC
    // producer / audio task (pri 5/3) and the FT8 list LVGL render, so a pri-2
    // worker uses its spare cycles without ever starving audio. If the spawn
    // fails we drop the queue and decode_slot falls back to single-core.
    TaskHandle_t worker = NULL;
    s_worker_queue = xQueueCreate(1, sizeof(worker_job_t *));
    s_worker_done  = xSemaphoreCreateBinary();
    if (s_worker_queue && s_worker_done) {
        BaseType_t wrc = xTaskCreatePinnedToCoreWithCaps(
            ft8_decode_worker_task, "ft8_dec0", 32768, NULL,
            tskIDLE_PRIORITY + 2, &worker, 0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (wrc != pdPASS) {
            ESP_LOGW(TAG, "core-0 decode worker spawn failed (rc=%d); single-core decode", (int)wrc);
            worker = NULL;
        }
    }
    if (!worker) {
        if (s_worker_queue) { vQueueDelete(s_worker_queue); s_worker_queue = NULL; }
        if (s_worker_done)  { vSemaphoreDelete(s_worker_done); s_worker_done = NULL; }
    }

    ESP_LOGI(TAG, "decode task ready (core %d, dual-core=%s)",
             xPortGetCoreID(), worker ? "yes" : "no");

    while (true) {
        decode_job_t job;
        if (xQueueReceive(s_decode_queue, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
            if (!s_ft8_running) break;
            continue;
        }
        if (job.mon_idx < 0) break;     // termination sentinel from capture task
        decode_slot(s_mon_pool[job.mon_idx], job.slot_sec, job.slot_idx,
                    job.cap_ms, job.stft_ms, job.start_off_ms);
        s_buf_busy[job.mon_idx] = false;   // hand the monitor back to the pool
    }

    // Stop the core-0 worker (blocked on its queue): send a NULL sentinel, let
    // it self-delete, then tear down the queue/semaphore.
    if (worker && s_worker_queue) {
        worker_job_t *sentinel = NULL;
        xQueueSend(s_worker_queue, &sentinel, pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_worker_queue) { vQueueDelete(s_worker_queue); s_worker_queue = NULL; }
    if (s_worker_done)  { vSemaphoreDelete(s_worker_done); s_worker_done = NULL; }

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

    // Capture scratch (decimated 12 kHz audio, reused every slot) + monitor pool.
    // Each monitor's waterfall (~166 KB) spills to PSRAM automatically. Capture
    // streams the STFT into the claimed monitor over the slot; the decoder reads
    // its waterfall and releases it (see the file header). s_mon_pool/s_cap_scratch
    // are zeroed statics, so a partial-alloc failure frees cleanly.
    const monitor_config_t cfg = {
        .f_min       = 200.0f,
        .f_max       = 3000.0f,
        .sample_rate = SR_HZ,
        .time_osr    = 2,
        .freq_osr    = 2,
        .protocol    = FTX_PROTOCOL_FT8,
    };
    s_cap_scratch = heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_cap_scratch) {
        ESP_LOGE(TAG, "PSRAM alloc for capture scratch failed");
        vTaskDelete(NULL);
        return;
    }
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        s_mon_pool[i] = heap_caps_malloc(sizeof(monitor_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_buf_busy[i] = false;
        if (!s_mon_pool[i]) {
            ESP_LOGE(TAG, "alloc for monitor %d/%d failed", i, FT8_NUM_BUFFERS);
            free_capture_pool();
            vTaskDelete(NULL);
            return;
        }
        monitor_init(s_mon_pool[i], &cfg);   // allocates the ~166 KB waterfall in PSRAM
        // Relocate the STFT window (~15 KB) to PSRAM. monitor_init malloc()s it,
        // and at <16 KB it lands in scarce INTERNAL RAM (the 16 KB PSRAM-spill
        // threshold) - across the pool that starves internal heap (main runs at
        // ~39 KB free in FT8; without this we drop to ~7 KB). The window is
        // read-only in monitor_process, so PSRAM costs nothing on the hot path;
        // last_frame stays internal since it's written every block.
        {
            monitor_t *m = s_mon_pool[i];
            float *w = heap_caps_malloc((size_t)m->nfft * sizeof(float),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (w) {
                memcpy(w, m->window, (size_t)m->nfft * sizeof(float));
                heap_caps_free(m->window);   // IDF heap_caps_free handles the malloc'd block
                m->window = w;
            }
        }
    }
    ESP_LOGI(TAG, "monitor pool: %d x monitor_t (waterfall ~%u KB each); scratch %u KB",
             FT8_NUM_BUFFERS,
             (unsigned)((size_t)s_mon_pool[0]->wf.max_blocks * s_mon_pool[0]->wf.block_stride
                        * sizeof(s_mon_pool[0]->wf.mag[0]) / 1024),
             (unsigned)(SLOT_SAMPLES * sizeof(float) / 1024));

    s_decode_queue = xQueueCreate(DECODE_QUEUE_DEPTH, sizeof(decode_job_t));
    if (!s_decode_queue) {
        ESP_LOGE(TAG, "failed to create decode queue");
        free_capture_pool();
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
        free_capture_pool();
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
                // All monitors in flight: the decoder has fallen behind (a run of
                // very busy slots). Skip this capture rather than overwrite a
                // waterfall still being decoded. The decode budget makes this
                // rare and self-correcting; log it so a persistent backlog shows.
                ESP_LOGW(TAG, "slot %d: all %d monitors busy - decoder behind, skipping slot",
                         slot_idx, FT8_NUM_BUFFERS);
                ft8_status_set("RX: decoder catching up...");
                slot_idx++;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            s_buf_busy[bi] = true;
            monitor_t *mon = s_mon_pool[bi];
            monitor_reset(mon);

            ft8_status_set("RX: capturing...");
            // Capture-start offset from the UTC slot boundary. Should stay ~0;
            // if it climbs ~0.2 s/slot the capture window is drifting off the
            // FT8 timing grid (the ~3-min decode-death bug). Self-anchors: it's
            // remeasured every slot from gettimeofday, so a small overrun past
            // the boundary doesn't accumulate.
            struct timeval tv_cap;
            gettimeofday(&tv_cap, NULL);
            int start_off_ms = (int)((tv_cap.tv_sec - slot_sec) * 1000
                                     + tv_cap.tv_usec / 1000);
            // Cap the capture at the next UTC slot boundary so the 15 s window
            // stays anchored to the FT8 grid. Without this the window slides
            // ~0.2 s/slot (capture takes a touch over 15 s) and decoding dies
            // after ~3 min; the finalize step zero-pads any shortfall.
            int ms_to_boundary = 15000 - start_off_ms;
            if (ms_to_boundary < 2000)                  ms_to_boundary = 2000;
            if (ms_to_boundary > (int)SLOT_TIMEOUT_MS)  ms_to_boundary = SLOT_TIMEOUT_MS;

            // Streaming STFT: arm capture, then FFT each 1920-sample symbol block
            // the instant it lands, so the waterfall is fully built by the time
            // the FT8 signal ends (~12.6 s in). The STFT cost overlaps capture
            // instead of being paid in the post-slot decode window.
            int64_t t0 = esp_timer_get_time();
            esp_err_t e = dsp_ft8_capture_begin(s_cap_scratch, SLOT_SAMPLES);
            int     blk       = mon->block_size;        // 1920 @ 12 kHz
            int     n_blocks  = SLOT_SAMPLES / blk;      // 93 full symbol blocks
            int     processed = 0;
            int64_t stft_us   = 0;
            if (e == ESP_OK) {
                while (processed < n_blocks) {
                    int avail = dsp_ft8_capture_progress();
                    while ((processed + 1) * blk <= avail && processed < n_blocks) {
                        int64_t ts = esp_timer_get_time();
                        monitor_process(mon, &s_cap_scratch[processed * blk]);
                        stft_us += esp_timer_get_time() - ts;
                        processed++;
                    }
                    if (avail >= SLOT_SAMPLES) break;                                  // whole slot in
                    if ((int)((esp_timer_get_time() - t0) / 1000) >= ms_to_boundary) break;  // boundary
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
                // Disarm + zero-pad the dead-air tail, then FFT any remaining
                // whole blocks (late in-flight samples or zero-pad -> noise).
                e = dsp_ft8_capture_finish(60);
                while ((processed + 1) * blk <= SLOT_SAMPLES) {
                    int64_t ts = esp_timer_get_time();
                    monitor_process(mon, &s_cap_scratch[processed * blk]);
                    stft_us += esp_timer_get_time() - ts;
                    processed++;
                }
            }
            int cap_ms  = (int)((esp_timer_get_time() - t0) / 1000);
            int stft_ms = (int)(stft_us / 1000);

            if (e == ESP_OK) {
                decode_job_t job = { bi, slot_sec, slot_idx, cap_ms, stft_ms, start_off_ms };
                if (xQueueSend(s_decode_queue, &job, 0) != pdTRUE) {
                    // Shouldn't happen (queue depth == pool size), but if it
                    // does, release the monitor so it isn't lost from the pool.
                    s_buf_busy[bi] = false;
                    ESP_LOGW(TAG, "slot %d: decode queue full - slot dropped", slot_idx);
                }
            } else {
                s_buf_busy[bi] = false;   // capture failed: release the monitor
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
    decode_job_t sentinel = { .mon_idx = -1, .slot_sec = -1LL };
    xQueueSend(s_decode_queue, &sentinel, pdMS_TO_TICKS(1000));
    // Clear any stale notification, then wait up to 10 s for decode task exit
    // (which also tears down its core-0 worker before notifying us).
    xTaskNotifyWait(0x01, 0x01, NULL, pdMS_TO_TICKS(10000));

    vQueueDelete(s_decode_queue);
    s_decode_queue = NULL;
    free_capture_pool();

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
