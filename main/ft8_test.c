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

#include <stdio.h>
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
#include "ft8/encode.h"
#include "common/monitor.h"

#include "dsp.h"
#include "audio/audio.h"
#include "dsp/iq_balance.h"
#include "cat/cat.h"
#include "storage/settings.h"
#include "ui/ui_mode.h"
#include "ui/ft8_screen.h"
#include "ui/ft8_screen_view.h"
#include "ft8_tx.h"
#include "ft8_qso.h"
#include "ft8_robot.h"
#include "time_sync.h"
#include "ft8_status.h"
#include "ui/ui.h"

static const char *TAG = "ft8_test";

#define SR_HZ                 12000
#define SLOT_SAMPLES          180000      // 15 s × 12 kHz (FT8 slot; also the
                                          // scratch-buffer / max size - FT4's
                                          // 7.5 s = 90000 samples fits inside)
#define SLOT_TIMEOUT_MS       20000

// Per-protocol slot length. FT8 = 15 s, FT4 = 7.5 s. The slot loop derives the
// active sample count (period_ms × 12 samples/ms) and the UTC boundary grid
// from these at runtime, keyed off the FT8/FT4 sub-mode (ft8_op_mode_get()).
#define FT8_SLOT_MS           15000
#define FT4_SLOT_MS           7500
#define SNTP_WAIT_TIMEOUT_MS  30000
#define CAT_STATUS_UPDATE_MS  5000

// Operating sub-mode (FT8/FT4). Written from the LVGL thread (the preset
// dropdown's FT4 column), read from the ft8_task slot loop. A single-word
// store is atomic on RV32, so volatile is sufficient for this advisory flag -
// no mutex needed. See ft8_op_mode_set/get and the note in ft8_test.h.
static volatile ft8_op_mode_t s_op_mode = FT8_OP_MODE_FT8;
static volatile bool          s_op_mode_loaded = false;   // lazy NVS load, once

void ft8_op_mode_set(ft8_op_mode_t m)
{
    s_op_mode = m;
    s_op_mode_loaded = true;   // a deliberate set always wins over the lazy load
    settings_set_ft8_op_mode((uint8_t)m);   // sticky across reboot (debounced flush)
    ESP_LOGI(TAG, "operating sub-mode -> %s", m == FT8_OP_MODE_FT4 ? "FT4" : "FT8");
}

ft8_op_mode_t ft8_op_mode_get(void)
{
    // First call after boot: pull the persisted value before anyone (the
    // preset dropdown, the slot loop) reads the flag, so a saved FT4
    // selection survives a reboot without main.c needing to know this
    // module exists.
    if (!s_op_mode_loaded) {
        s_op_mode_loaded = true;
        qmx_settings_t cfg;
        settings_load_all(&cfg);
        s_op_mode = (cfg.ft8_op_mode == (uint8_t)FT8_OP_MODE_FT4) ? FT8_OP_MODE_FT4 : FT8_OP_MODE_FT8;
    }
    return s_op_mode;
}

int ft8_op_mode_slot_ms(void)
{
    return (ft8_op_mode_get() == FT8_OP_MODE_FT4) ? FT4_SLOT_MS : FT8_SLOT_MS;
}

// Max FT8 candidates considered per slot (matches the cands[] buffer in
// decode_slot) - also the upper bound on each worker's timing-sample array.
//
// MEASURED 2026-06-27: tried raising this 140 -> 300 because the per-slot
// diagnostic showed cand PINNED at the ceiling every slot (>140 sync
// candidates above FT8_FIND_MIN_SCORE exist on a busy band) with the decode
// budget ~85% idle. A controlled before/after on 20 m showed it was a CLEAN
// NEGATIVE: cand went to 300 (still pinned, so the band makes 300+ sync hits)
// but decodes did NOT rise (mean 12.6 vs 14.8 — within band variance), while
// decode CPU ~doubled (1.6s -> 3.0s). Candidates #141-300 are noise (score-5
// sync false-positives that correctly fail LDPC); the real decodable signals
// already live in the strongest ~140. So 140 is NOT clipping real signals and
// raising it only burns core-1 CPU that then competes with the next slot's
// capture STFT. Yield here is band-limited, not candidate-limited. Kept at 140.
#define FT8_MAX_CANDIDATES    140

// Minimum Costas sync score for a candidate to be attempted. ft8_lib's demo
// uses 10; we lower it to 5 to attempt weaker candidates, spending the per-slot
// headroom freed by Stages 1+2 on a few more real decodes (most low-score
// candidates fail LDPC harmlessly).
#define FT8_FIND_MIN_SCORE    5

// Reply-on-the-immediate-slot window. A reply is decoded from the slot we hear
// the partner in, but that decode only finishes ~1-2 s into the NEXT (our-TX)
// slot - normally too late to arm in time, so the reply slips a full 15 s cycle.
// While capturing a slot, ft8_task polls for a freshly-armed reply during this
// opening window and, if one lands, aborts the (discardable) RX capture and
// fires the burst on THIS slot. A burst started this late is still inside
// ft8_lib's time-search range (~-1.6..+3.2 s) so the partner still decodes it.
// WSJT-X gets here by decoding in the 2.36 s dead-air gap; this is our equivalent.
#define FT8_REPLY_TX_WINDOW_MS 2500

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

// Stuck-pipeline watchdog: consecutive idle-RX slots with candidates-but-zero-
// decodes before a soft audio+IQ reset. A genuinely wedged pipeline stays
// wedged forever, so a long dwell costs nothing to wait out; a merely quiet band
// recovers within a slot or two. Was 2 slots (far too twitchy - fired on any
// brief lull, and its reset empties the decode list + reconverges IQ, which
// glitched the display and disrupted in-progress QSOs).
//
// Expressed as WALL-CLOCK time, not a fixed slot count, so it dwells the same
// ~600 s (10 min) in both FT8 (15 s slots -> 40 slots) and FT4 (7.5 s slots -> 80 slots).
// A fixed 8-slot count fired after only ~60 s in FT4 - and since FT4 is far less
// populated than FT8, a blank decode list is routine there, so a spurious reset
// on a merely quiet band is too common. Extended to 10 min to almost never fire
// on normal band conditions. The effective per-mode threshold is FT8_STUCK_RESET_MS / slot_ms.
#define FT8_STUCK_RESET_MS 600000

// Wall-clock budget for one slot's decode (monitor_process + candidate loop,
// measured from the top of decode_slot). Candidates are processed strongest
// first, so hitting the budget drops only the weakest tail on the busiest
// slots. Must stay safely below the 15 s slot so the decoder can never fall
// behind indefinitely; ~11 s leaves ~4 s/slot of headroom to catch up.
#define FT8_DECODE_BUDGET_MS  11000

// Max LDPC/belief-propagation iterations per candidate, FT8 only. Lowered
// 60 -> 30 (v0.15.13), then 30 -> 15 (2026-06-29): bp_decode() only exits
// early on success (errors==0) or the degenerate all-zero case - a failing
// candidate (the majority on a busy band; cand=140 routinely hits
// FT8_MAX_CANDIDATES) always burns the FULL iteration count. On a
// continuing QSO this dec_ms tail eats into the ~15 s slot deadline for our
// own next reply (capture alone already runs ~15.1 s to the UTC boundary,
// leaving ~0 margin before dec_ms even starts) - every reply in a real QSO
// needed a resend because decode of the partner's message kept finishing
// after our next TX had already fired with stale content. Halving again
// trades a bit more weak-signal sensitivity for closing that gap.
//
// 2026-06-30: this constant was being applied to FT4 too (decode_slot()
// doesn't distinguish protocol) and it badly hurt FT4 decode - confirmed by
// field report immediately after release. FT4's slot is half FT8's (7.5 s
// vs 15 s) so the resend-timing pressure that motivated the FT8 cut doesn't
// even apply the same way, and FT4's LDPC code (different parity matrix)
// apparently needs more headroom to converge than FT8's. Split into a
// separate FT4 constant restored to the pre-cut value; decode_candidate_range()
// picks the right one off s_pool_proto at decode time.
#define FT8_LDPC_MAX_ITERS    15
#define FT4_LDPC_MAX_ITERS    30

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
    int      arm_backlog;  // diag: audio-ring backlog (pairs) discarded at arm;
                           // should stay small/flat — growth was the cliff bug
    int      drop_delta;   // diag: ring-full sample drops during this slot
} decode_job_t;

static QueueHandle_t  s_decode_queue = NULL;
static volatile bool  s_ft8_running  = false;
// True for the whole lifetime of one ft8_task (capture) instance. A fast
// Panadapter<->FT8 toggle could otherwise spawn a SECOND ft8_task before the
// first finished tearing down; the two clobber the shared s_decode_queue and
// one decode task ends up calling xQueueReceive(NULL) -> assert/reboot.
static volatile bool  s_ft8_task_alive = false;

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
// Protocol the pool is currently built for. The monitor's waterfall/block sizes
// are protocol-specific (FT8: 1920-sample blocks, 93/slot; FT4: 576, 156/slot),
// so switching FT8<->FT4 rebuilds the pool. -1 = not yet built.
static int           s_pool_proto = -1;   // ftx_protocol_t, or -1
// Single reusable capture scratch buffer (decimated 12 kHz audio). Consumed by
// the streaming STFT during capture; not needed once the waterfall is built, so
// one buffer serves every slot (capture is strictly sequential).
static float        *s_cap_scratch = NULL;

// Shared kiss-FFT scratch, reused by EVERY monitor's STFT (monitor_process).
// monitor_process is called only from the capture task, one monitor at a time,
// strictly sequentially (captures never overlap; decode works on the pre-built
// waterfall and never touches the FFT), so a single shared config is race-free.
// Sharing exists to force the hot FFT buffer into fast INTERNAL SRAM: with the
// per-monitor buffers monitor_init allocates, only the first won the scarce-
// internal-RAM lottery and the second spilled to PSRAM, making its STFT ~10x
// slower (measured) and collapsing decode on every slot that landed on it -
// especially in FT4, whose tight 7.5 s slot can't absorb a 4.9 s STFT. One
// shared buffer also halves the pool's internal-RAM footprint.
static void         *s_shared_fft_work = NULL;
static kiss_fftr_cfg s_shared_fft_cfg  = NULL;
static bool          s_shared_fft_internal = false;  // diag: did it land internal?

// Return the index of a free pool monitor, or -1 if all are in flight.
static int find_free_buffer(void)
{
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        if (!s_buf_busy[i]) return i;
    }
    return -1;
}

// Release the shared FFT scratch (once). Callers must first NULL each monitor's
// fft_work so monitor_free() doesn't also try to free it (double-free).
static void free_shared_fft(void)
{
    if (s_shared_fft_work) { free(s_shared_fft_work); s_shared_fft_work = NULL; }
    s_shared_fft_cfg = NULL;
    s_shared_fft_internal = false;
}

// Free the monitor pool + capture scratch (idempotent; safe on partial alloc).
static void free_capture_pool(void)
{
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        if (s_mon_pool[i]) {
            // Only detach the shared FFT (freed once below). If a partial build
            // left this monitor with its OWN per-monitor fft_work (shared not
            // set up yet), leave it so monitor_free() frees it - no leak.
            if (s_mon_pool[i]->fft_work == s_shared_fft_work)
                s_mon_pool[i]->fft_work = NULL;
            monitor_free(s_mon_pool[i]);
            heap_caps_free(s_mon_pool[i]);
            s_mon_pool[i] = NULL;
        }
    }
    free_shared_fft();
    if (s_cap_scratch) { heap_caps_free(s_cap_scratch); s_cap_scratch = NULL; }
    s_pool_proto = -1;
}

// Map the FT8/FT4 sub-mode flag to a decoder protocol.
static inline ftx_protocol_t proto_for_mode(void)
{
    return (ft8_op_mode_get() == FT8_OP_MODE_FT4) ? FTX_PROTOCOL_FT4
                                                  : FTX_PROTOCOL_FT8;
}

// Free just the monitor objects (keep s_cap_scratch, which is protocol-agnostic
// and sized for the larger FT8 slot). Used by the FT8<->FT4 rebuild path.
static void free_monitor_objects(void)
{
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        if (s_mon_pool[i]) {
            if (s_mon_pool[i]->fft_work == s_shared_fft_work)
                s_mon_pool[i]->fft_work = NULL;  // shared - freed once below
            monitor_free(s_mon_pool[i]);
            heap_caps_free(s_mon_pool[i]);
            s_mon_pool[i] = NULL;
        }
    }
    // The shared FFT is nfft-specific, so a protocol rebuild must drop it too;
    // build_monitor_pool() allocates a fresh one for the new protocol.
    free_shared_fft();
    s_pool_proto = -1;
}

// Allocate + monitor_init the pool for `proto`, relocating each STFT window to
// PSRAM (see the long note inline). Returns false on any alloc failure (caller
// frees). Sets s_pool_proto on success.
static bool build_monitor_pool(ftx_protocol_t proto)
{
    const monitor_config_t cfg = {
        .f_min       = 200.0f,
        .f_max       = 3000.0f,
        .sample_rate = SR_HZ,
        .time_osr    = 2,
        .freq_osr    = 2,
        .protocol    = proto,
    };
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        s_mon_pool[i] = heap_caps_malloc(sizeof(monitor_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_buf_busy[i] = false;
        if (!s_mon_pool[i]) {
            ESP_LOGE(TAG, "alloc for monitor %d/%d failed", i, FT8_NUM_BUFFERS);
            return false;
        }
        monitor_init(s_mon_pool[i], &cfg);   // allocates the waterfall in PSRAM
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
                heap_caps_free(m->window);
                m->window = w;
            }
        }
    }

    // Build ONE shared kiss-FFT config in internal SRAM and repoint every
    // monitor at it (see s_shared_fft_work comment). Try internal first; if the
    // fragmented internal heap can't hold it right now, fall back to PSRAM -
    // that's symmetric and no worse than the old spilled buffer, and it can
    // never fail the pool build.
    size_t fft_work_size = 0;
    kiss_fftr_alloc(s_mon_pool[0]->nfft, 0, NULL, &fft_work_size);
    s_shared_fft_work = heap_caps_malloc(fft_work_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_shared_fft_internal = (s_shared_fft_work != NULL);
    if (!s_shared_fft_work) {
        s_shared_fft_work = heap_caps_malloc(fft_work_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_shared_fft_work) {
        ESP_LOGE(TAG, "shared FFT scratch alloc (%u B) failed", (unsigned)fft_work_size);
        return false;
    }
    size_t chk = fft_work_size;
    s_shared_fft_cfg = kiss_fftr_alloc(s_mon_pool[0]->nfft, 0, s_shared_fft_work, &chk);
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) {
        // Drop the per-monitor FFT scratch monitor_init created (allocated with
        // plain malloc, so free() it) and point at the shared one.
        free(s_mon_pool[i]->fft_work);
        s_mon_pool[i]->fft_work = s_shared_fft_work;
        s_mon_pool[i]->fft_cfg  = s_shared_fft_cfg;
    }

    s_pool_proto = (int)proto;
    ESP_LOGI(TAG, "monitor pool built for %s: block=%d samples, %d blocks/slot, %u KB waterfall each",
             proto == FTX_PROTOCOL_FT4 ? "FT4" : "FT8",
             s_mon_pool[0]->block_size, s_mon_pool[0]->wf.max_blocks,
             (unsigned)((size_t)s_mon_pool[0]->wf.max_blocks * s_mon_pool[0]->wf.block_stride
                        * sizeof(s_mon_pool[0]->wf.mag[0]) / 1024));
    ESP_LOGI(TAG, "shared FFT scratch: %u B in %s (nfft=%d) - all %d monitors share it",
             (unsigned)fft_work_size, s_shared_fft_internal ? "INTERNAL" : "PSRAM",
             s_mon_pool[0]->nfft, FT8_NUM_BUFFERS);
    return true;
}

// Top-of-slot check: if the operator switched FT8<->FT4, rebuild the monitor
// pool for the new protocol. We're about to free monitors the decoder may still
// be reading, so first drain any in-flight decode (bounded wait on the busy
// flags), then free + rebuild. On rebuild failure the pool is left empty and
// the loop's find_free_buffer() simply skips slots until the next mode toggle.
static void reinit_pool_if_mode_changed(void)
{
    ftx_protocol_t want = proto_for_mode();
    if ((int)want == s_pool_proto) return;

    ESP_LOGI(TAG, "sub-mode change -> rebuilding monitor pool for %s",
             want == FTX_PROTOCOL_FT4 ? "FT4" : "FT8");
    for (int guard = 0; guard < 400; guard++) {   // ~4 s ceiling
        bool any_busy = false;
        for (int i = 0; i < FT8_NUM_BUFFERS; i++) if (s_buf_busy[i]) any_busy = true;
        if (!any_busy) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    free_monitor_objects();
    if (!build_monitor_pool(want)) {
        ESP_LOGE(TAG, "monitor pool rebuild for %s FAILED - freeing partial",
                 want == FTX_PROTOCOL_FT4 ? "FT4" : "FT8");
        free_monitor_objects();
    }
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

static QueueHandle_t     s_worker_queue  = NULL;  // carries worker_job_t*
static SemaphoreHandle_t s_worker_done   = NULL;
static SemaphoreHandle_t s_worker_exited = NULL;  // worker gives this just before vTaskDelete(NULL);
                                                  // the decode task JOINS on it before freeing the
                                                  // queue/semaphore and returning (freeing its stack).
                                                  // Without the join, a worker still holding a job
                                                  // pointer into the decode task's stack faulted on
                                                  // freed memory ("Load address misaligned").

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
//
// Returns true if the CAT query itself succeeded (so ft8_task knows it has
// SOME working time source and can proceed) - NOT whether the clock was
// actually set from QMX. *out_applied (if non-NULL) reports that separately:
// time_sync_notify_qmx() silently skips the apply when SNTP is already
// authoritative, so the caller's status message must check this rather than
// assume "query succeeded" means "time came from QMX".
static bool set_time_from_qmx_rtc(bool *out_applied)
{
    int h, m, s;
    if (cat_query_qmx_time(&h, &m, &s) != ESP_OK) return false;
    bool applied = time_sync_notify_qmx(h, m, s);
    if (out_applied) *out_applied = applied;
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
// Block until the next slot boundary strictly after `after_ms`, returning the
// boundary as a UTC millisecond value (multiple of period_ms). Millisecond
// resolution is required for FT4: its 7.5 s grid lands on half-seconds
// (..., 22500, 30000, 37500 ms), which a whole-second test can't represent.
static int64_t wait_for_slot_boundary_ms(int64_t after_ms, int period_ms)
{
    while (1) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        int64_t boundary = (now_ms / period_ms) * period_ms;
        if (boundary > after_ms) {
            return boundary;
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

// Auto-apply the per-slot robust timing average to the system clock instead
// of requiring the operator to open the time-sync modal and tap Apply. This
// is deliberately the population average of everyone currently decoded, not
// absolute UTC truth - which is the more useful target for actually working
// people: if the active stations you're trying to copy are collectively
// offset from true time by some amount, matching THEM gets you in sync with
// who you need to communicate with, where matching GPS/NTP exactly would not.
// SNTP still sets the whole-second/date baseline (rare resyncs, ~hourly); this
// nudges the sub-second placement every slot while FT8 is actively decoding,
// and naturally yields to a fresh SNTP/QMX sync whenever one of those fires.
//
// Two guards keep this from chasing noise on a quiet band: require at least
// FT8_AUTOSYNC_MIN_SAMPLES surviving the outlier rejection (more candidates =
// more confidence the average reflects the on-air population, not one or two
// marginal decodes), and skip corrections under FT8_AUTOSYNC_MIN_MS (not
// worth an RTC/NVS write for a correction smaller than the sync measurement's
// own noise floor).
#define FT8_AUTOSYNC_MIN_SAMPLES 3
#define FT8_AUTOSYNC_MIN_MS      15

// Loop-filter gain: apply only this fraction of each slot's raw measurement,
// not the full value. A single slot's robust mean is still a noisy estimator
// of the population's true offset (only a handful of candidates, each good to
// only tens of ms of sync precision) - field data showed the undamped version
// bouncing between roughly +200 and -300 ms slot to slot with no convergence,
// i.e. chasing measurement noise rather than tracking real drift. Applying a
// fraction each cycle averages that noise down over several slots (same idea
// as an NTP/PLL loop filter) while a genuine sustained offset still gets
// corrected, just over a few cycles instead of in one noisy jump.
#define FT8_AUTOSYNC_GAIN 0.3f

// Hard clamp on the per-slot clock nudge, applied AFTER the gain. The gain
// alone assumes the raw measurement is only mildly noisy (it was tuned on FT8,
// whose raw offsets sit around ±200-300 ms) - but FT4's tighter slot and fewer
// decodes/slot throw much larger raw values (field data: ±800-950 ms), and 30%
// of that is still a ~250 ms single-slot jump, enough to visibly shift the slot
// countdown/parity on screen. Clamping the applied step keeps the deliberate
// "track the on-air population's collective offset" behaviour (a genuine
// sustained offset is still followed, just approached a few ms per slot over
// several slots) while guaranteeing no single noisy slot can yank the clock far
// enough to be seen. This is the "no big fluctuations" guarantee.
#define FT8_AUTOSYNC_MAX_STEP_MS 20

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
    int max_iters = (s_pool_proto == (int)FTX_PROTOCOL_FT4) ? FT4_LDPC_MAX_ITERS : FT8_LDPC_MAX_ITERS;
    for (int i = start; i < n_cand; i += step) {
        if ((int)((esp_timer_get_time() - t_start_us) / 1000) >= FT8_DECODE_BUDGET_MS) {
            break;
        }
        out->n_attempted++;
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon->wf, &cands[i], max_iters, &msg, &st)) continue;
        char text[FTX_MAX_MESSAGE_LENGTH];
        ftx_message_offsets_t off;
        if (ftx_message_decode(&msg, NULL, text, &off) == FTX_MESSAGE_RC_OK) {
            out->n_decoded++;
            // SR=12000. block_size/subblock_size come from mon itself rather than
            // being hardcoded to FT8's 1920/960 - FT4 uses 576/288, and using the
            // FT8 constants for FT4 candidates silently produced a ~3.3x-too-large
            // (and wrongly-scaled) timing offset. start_off_ms anchors this
            // candidate's sync position back to the UTC slot boundary, same as
            // every other candidate this slot.
            if (out->n_timing < FT8_MAX_CANDIDATES) {
                out->timing[out->n_timing++] =
                    (float)start_off_ms +
                    (cands[i].time_offset * mon->block_size + cands[i].time_sub * mon->subblock_size) / 12.0f;
            }
            int snr_db = (int)lroundf(ft8_estimate_snr_db(mon, &cands[i], noise_db));
            // cands[i].freq_offset is a coarse FFT bin index RELATIVE TO mon->min_bin
            // (6.25 Hz/bin - FT8 tone spacing), not an absolute audio Hz value. Every
            // downstream consumer of ft8_call_t.last_freq (CQ clear-tone scan, TX
            // clash detection, and the reply tone itself via
            // ft8_tx_build_request's target_audio_freq_hz) treats it as Hz, so it
            // must be converted here - same formula ft8_lib's own demo uses
            // (decode_ft8.c: freq_hz = (min_bin + freq_offset + freq_sub/freq_osr) /
            // symbol_period). Omitting this put every recorded/replied tone off by
            // mon->min_bin*6.25 (200 Hz) plus a ~6.25x scale error.
            int freq_hz = (int)lroundf((mon->min_bin + cands[i].freq_offset) / mon->symbol_period);
            ESP_LOGI(TAG, "decoded: '%s' (score=%d freq=%dHz snr=%d)",
                     text, cands[i].score, freq_hz, snr_db);
            ft8_screen_record_decode(text, cands[i].score, snr_db, freq_hz, slot_sec);
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
    // Signal the join BEFORE self-deleting: after this the worker touches
    // nothing shared (queue/semaphore/job), so the decode task can safely
    // free them once it sees this.
    if (s_worker_exited) xSemaphoreGive(s_worker_exited);
    vTaskDelete(NULL);
}

// Decode one slot's PRE-BUILT waterfall (the STFT was streamed in during
// capture). Candidate search, then dual-core LDPC fan-out, merge, record,
// advance the QSO state machine.
static void decode_slot(monitor_t *mon, int64_t slot_sec, int slot_idx,
                        int cap_ms, int stft_ms, int start_off_ms,
                        int arm_backlog, int drop_delta)
{
    int64_t t_start = esp_timer_get_time();

    ftx_candidate_t cands[FT8_MAX_CANDIDATES];
    // Deeper search: min sync score 10 -> 5 surfaces weaker candidates to attempt.
    // Stages 1+2 freed ~4 s of per-slot headroom, so the extra (mostly-failing)
    // LDPC attempts are affordable and net a few more real weak-signal decodes;
    // strongest-first ordering + FT8_DECODE_BUDGET_MS still protect the busiest slots.
    int n_cand = ftx_find_candidates(&mon->wf, FT8_MAX_CANDIDATES, cands, FT8_FIND_MIN_SCORE);

    // "Let the others wait": if we're mid pounce-exchange, the message we
    // actually need is the partner's, at a known tone — move any candidate
    // near it to the FRONT of cands[] (still strongest-first within each
    // partition) so it's one of the very first picks on whichever core
    // reaches it, regardless of how many other (irrelevant) candidates a busy
    // band produced. This is what lets the reply-on-immediate-slot window
    // (FT8_REPLY_TX_WINDOW_MS) actually be hit on a crowded band instead of
    // racing the full candidate list first.
    int priority_freq_hz;
    if (n_cand > 1 && ft8_qso_get_priority_freq(&priority_freq_hz)) {
        ftx_candidate_t reordered[FT8_MAX_CANDIDATES];
        int np = 0, nr = 0;
        for (int i = 0; i < n_cand; i++) {
            float hz = (mon->min_bin + cands[i].freq_offset) / mon->symbol_period;
            if (fabsf(hz - (float)priority_freq_hz) <= 25.0f) {
                reordered[np++] = cands[i];
            }
        }
        for (int i = 0; i < n_cand; i++) {
            float hz = (mon->min_bin + cands[i].freq_offset) / mon->symbol_period;
            if (fabsf(hz - (float)priority_freq_hz) > 25.0f) {
                reordered[np + nr++] = cands[i];
            }
        }
        memcpy(cands, reordered, (size_t)n_cand * sizeof(cands[0]));
    }

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

        // Auto-sync: nudge the system clock to the on-air population average
        // every slot, not just when the operator opens the time-sync modal
        // and taps Apply. See FT8_AUTOSYNC_MIN_SAMPLES/_MS above for why both
        // guards exist. Previously FT8-only - the timing offset was
        // protocol-scaled wrong for FT4 (see the mon->block_size fix above);
        // now that it's computed from the real per-protocol block sizes, FT4
        // is included too.
        if (n_timing >= FT8_AUTOSYNC_MIN_SAMPLES &&
            abs(s_last_timing_ms) >= FT8_AUTOSYNC_MIN_MS) {
            // Apply only a damped fraction of the raw measurement - see
            // FT8_AUTOSYNC_GAIN above. _quiet: skips the QMX CAT push, which
            // is a blocking, non-poll-task-routed CDC write - unsafe to call
            // from this hot path (would race the CAT poll task and stall
            // decode). The periodic 5-min QMX poll / manual modal Apply
            // already cover keeping the QMX's own RTC in the ballpark.
            int damped_ms = (int)roundf(s_last_timing_ms * FT8_AUTOSYNC_GAIN);
            // Clamp the per-slot step so a noisy slot can't visibly jump the
            // clock (see FT8_AUTOSYNC_MAX_STEP_MS). A sustained collective
            // offset is still tracked, a few ms per slot.
            if (damped_ms >  FT8_AUTOSYNC_MAX_STEP_MS) damped_ms =  FT8_AUTOSYNC_MAX_STEP_MS;
            if (damped_ms < -FT8_AUTOSYNC_MAX_STEP_MS) damped_ms = -FT8_AUTOSYNC_MAX_STEP_MS;
            if (damped_ms != 0) {
                time_sync_apply_correction_ms_quiet(damped_ms);
            }
        }
    }

    int dec_ms = (int)((esp_timer_get_time() - t_start) / 1000);

    size_t heap_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    size_t heap_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024;
    // Low-water mark + largest contiguous block: the SDIO TX path needs an
    // internal DMA buffer, so what matters for the transport_drv crash is the
    // worst-case internal free and whether a single contiguous chunk survives.
    size_t heap_i_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;
    size_t heap_i_lblk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024;

    ft8_status_set("RX: %d decoded", n_decoded);
    ESP_LOGI(TAG,
        "slot %d UTC %lld: off=%+dms cap=%dms stft=%dms dec=%dms cand=%d dec=%d skip=%d "
        "backlog=%dpr drop=%dpr timing=%+dms heap_i=%uKB(min=%uKB,lblk=%uKB) heap_p=%uKB",
        slot_idx, (long long)slot_sec, start_off_ms,
        cap_ms, stft_ms, dec_ms, n_cand, n_decoded, n_skipped,
        arm_backlog, drop_delta,
        s_last_timing_valid ? s_last_timing_ms : 0,
        (unsigned)heap_i, (unsigned)heap_i_min, (unsigned)heap_i_lblk, (unsigned)heap_p);

    // When internal DRAM gets dangerously low, dump the full internal-heap
    // breakdown so we can see the consumer/fragmentation picture right before
    // the SDIO TX path (transport_drv_sta_tx -> copy_buff) starves and panics.
    // Threshold chosen above the ~17 KB seen at the crash; logs at most once per
    // crossing so it doesn't spam a healthy session.
    static bool s_heap_warned = false;
    if (heap_i < 40) {
        if (!s_heap_warned) {
            s_heap_warned = true;
            ESP_LOGW(TAG, "=== INTERNAL DRAM LOW (%uKB free, %uKB largest block) — dumping internal heap ===",
                     (unsigned)heap_i, (unsigned)heap_i_lblk);
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        }
    } else if (heap_i > 60) {
        s_heap_warned = false;  // re-arm once we recover comfortably
    }

    // Stuck-decoder watchdog: cand>20 with dec=0 was meant to catch a corrupted
    // audio pipeline (bad UAC samples, IQ mode dropout, ring-buffer glitch) and
    // soft-reset it (IQ reconverge + floor reseed) instead of needing a QMX
    // power cycle. Two caveats learned the hard way:
    //  - cand is ~always 140 (noise false-positives hit the search cap every
    //    slot), so the real trigger is just "N consecutive zero-decode RX
    //    slots" - which a merely quiet band produces legitimately. Hence the
    //    long dwell (FT8_STUCK_RESET_MS) before acting (was 2 = far too twitchy).
    //  - the reset empties the decode list and briefly de-syncs IQ, so it must
    //    NOT fire mid-QSO or while transmitting: sparse RX is normal then (own
    //    TX desenses RX; the partner may be the only station on our tone), and
    //    a reset there disrupts the exchange - it was actively timing out real
    //    QSOs. Skip entirely (and reset the counter) whenever TX or a QSO is in
    //    progress.
    // Counter is static (persists across slots; this is the decode task, one
    // instance, no concurrency concern).
    {
        static int s_stuck_slots = 0;
        const char *proto = (ft8_op_mode_get() == FT8_OP_MODE_FT4) ? "FT4" : "FT8";
        int stuck_thresh = FT8_STUCK_RESET_MS / ft8_op_mode_slot_ms();  // 8 FT8 / 16 FT4
        bool tx_or_qso = (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_IDLE) ||
                         (ft8_qso_get_state() != FT8_QSO_IDLE);
        if (tx_or_qso) {
            s_stuck_slots = 0;   // sparse RX is expected here; never reset mid-exchange
        } else if (n_cand > 20 && n_decoded == 0) {
            s_stuck_slots++;
            if (s_stuck_slots >= stuck_thresh) {
                ESP_LOGW(TAG, "%s: %d consecutive zero-decode idle RX slots "
                         "(cand=%d) — soft-resetting audio+IQ",
                         proto, s_stuck_slots, n_cand);
                s_stuck_slots = 0;
                iq_balance_reset();
                audio_request_reset();
            }
        } else {
            s_stuck_slots = 0;
        }
    }

    ft8_qso_advance(slot_sec);
    // Robot auto-answer: runs after advance() (so the existing machine reacts
    // first); self-gates to IDLE, so it only acts when no QSO is in progress.
    // Its ft8_qso_start() arms a reply for the next slot, which reply-on-
    // immediate-slot then fires — same fast path as a human-started pounce.
    ft8_robot_tick(slot_sec);
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
    s_worker_queue  = xQueueCreate(1, sizeof(worker_job_t *));
    s_worker_done   = xSemaphoreCreateBinary();
    s_worker_exited = xSemaphoreCreateBinary();
    if (s_worker_queue && s_worker_done && s_worker_exited) {
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
        if (s_worker_queue)  { vQueueDelete(s_worker_queue); s_worker_queue = NULL; }
        if (s_worker_done)   { vSemaphoreDelete(s_worker_done); s_worker_done = NULL; }
        if (s_worker_exited) { vSemaphoreDelete(s_worker_exited); s_worker_exited = NULL; }
    }

    ESP_LOGI(TAG, "decode task ready (core %d, dual-core=%s)",
             xPortGetCoreID(), worker ? "yes" : "no");

    while (true) {
        decode_job_t job;
        QueueHandle_t q = s_decode_queue;
        if (q == NULL) break;   // queue torn down (shutdown) — exit cleanly, never xQueueReceive(NULL)
        if (xQueueReceive(q, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
            if (!s_ft8_running) break;
            continue;
        }
        if (job.mon_idx < 0) break;     // termination sentinel from capture task
        decode_slot(s_mon_pool[job.mon_idx], job.slot_sec, job.slot_idx,
                    job.cap_ms, job.stft_ms, job.start_off_ms,
                    job.arm_backlog, job.drop_delta);
        s_buf_busy[job.mon_idx] = false;   // hand the monitor back to the pool
    }

    // Stop the core-0 worker (blocked on its queue): send a NULL sentinel, then
    // JOIN — wait for the worker to actually reach vTaskDelete (it gives
    // s_worker_exited first) before deleting the queue/semaphore and returning.
    // The old code waited a FIXED 50 ms, which was not enough if the worker was
    // still holding a job pointer into this task's stack: freeing the stack out
    // from under it caused a "Load address misaligned" panic on FT8 exit. The
    // worker is idle here (decode_slot always joins on s_worker_done before
    // returning), so this normally returns in well under a millisecond; the
    // generous bound only guards a pathological in-flight decode.
    if (worker && s_worker_queue && s_worker_exited) {
        worker_job_t *sentinel = NULL;
        xQueueSend(s_worker_queue, &sentinel, pdMS_TO_TICKS(1000));
        if (xSemaphoreTake(s_worker_exited, pdMS_TO_TICKS(12000)) != pdTRUE) {
            ESP_LOGW(TAG, "worker join timed out — deleting shared state anyway");
        }
    }
    if (s_worker_queue)  { vQueueDelete(s_worker_queue); s_worker_queue = NULL; }
    if (s_worker_done)   { vSemaphoreDelete(s_worker_done); s_worker_done = NULL; }
    if (s_worker_exited) { vSemaphoreDelete(s_worker_exited); s_worker_exited = NULL; }

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
    s_ft8_task_alive = true;

    ft8_status_set("Waiting for QMX...");
    ESP_LOGI(TAG, "waiting for CAT (QMX USB + Q9 1; handshake)...");
    wait_for_cat_ready();

    // Query the QMX's RTC first so we have a working time source even with
    // no WiFi (POTA). Whether it's actually USED is decided inside
    // time_sync_notify_qmx() - SNTP wins whenever WiFi is up and has
    // synced, since the QMX's onboard RTC is no more accurate than that and
    // (on a plain, non-GPS QMX) drifts freely. Report whichever source is
    // truly authoritative, not just "the CAT query worked".
    ft8_status_set("Getting time from QMX...");
    bool applied_from_qmx = false;
    if (set_time_from_qmx_rtc(&applied_from_qmx)) {
        if (applied_from_qmx) {
            ft8_status_set("Time from QMX GPS/RTC");
            ESP_LOGI(TAG, "time set from QMX RTC/GPS");
        } else {
            ft8_status_set("Time from SNTP (WiFi)");
            ESP_LOGI(TAG, "QMX RTC query OK but SNTP already authoritative - kept SNTP time");
        }
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

    // Capture scratch (decimated 12 kHz audio, reused every slot) + monitor
    // pool. The scratch is sized for the larger FT8 slot (180000) and shared by
    // FT4 (90000, fits inside). The pool is built for the CURRENT sub-mode; the
    // slot loop rebuilds it on an FT8<->FT4 toggle (reinit_pool_if_mode_changed).
    // s_mon_pool/s_cap_scratch are zeroed statics, so a partial-alloc frees clean.
    s_cap_scratch = heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_cap_scratch) {
        ESP_LOGE(TAG, "PSRAM alloc for capture scratch failed");
        s_ft8_task_alive = false;
        vTaskDelete(NULL);
        return;
    }
    if (!build_monitor_pool(proto_for_mode())) {
        ESP_LOGE(TAG, "initial monitor pool build failed");
        free_capture_pool();
        s_ft8_task_alive = false;
        vTaskDelete(NULL);
        return;
    }

    s_decode_queue = xQueueCreate(DECODE_QUEUE_DEPTH, sizeof(decode_job_t));
    if (!s_decode_queue) {
        ESP_LOGE(TAG, "failed to create decode queue");
        free_capture_pool();
        s_ft8_task_alive = false;
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
        s_ft8_task_alive = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "entering continuous slot loop (pooled, time-budgeted decode)");

    int slot_idx = 0;
    struct timeval tv_init;
    gettimeofday(&tv_init, NULL);
    int64_t now_ms_init   = (int64_t)tv_init.tv_sec * 1000 + tv_init.tv_usec / 1000;
    int     period_ms_init = (proto_for_mode() == FTX_PROTOCOL_FT4) ? FT4_SLOT_MS : FT8_SLOT_MS;
    int64_t last_boundary_ms = (now_ms_init / period_ms_init) * period_ms_init;

    while (ui_mode_get() == UI_MODE_FT8) {
        // Rebuild the monitor pool if the operator toggled FT8<->FT4 since the
        // last slot. Done at the top, before claiming a buffer, so the pool is
        // settled for this whole slot.
        reinit_pool_if_mode_changed();
        bool   is_ft4    = (s_pool_proto == (int)FTX_PROTOCOL_FT4);
        int    period_ms = is_ft4 ? FT4_SLOT_MS : FT8_SLOT_MS;
        int    slot_samples = period_ms * (SR_HZ / 1000);   // 90000 (FT4) / 180000 (FT8)
        // FT4 TX is now implemented (ft8_tx.c) but force-routed through the
        // simulation interlock for ANY FT4 request - the 48 ms CAT cadence is
        // unverified on real hardware, so ft8_tx_run()/ft8_tx_arm() never let
        // an FT4 burst reach a connected QMX regardless of this slot loop's
        // own gating. So the slot loop itself can simply try TX in both
        // protocols; the real safety boundary lives in ft8_tx.c, not here.

        ESP_LOGI(TAG, "slot %d: waiting for next %s boundary...", slot_idx, is_ft4 ? "FT4" : "FT8");
        int64_t boundary_ms = wait_for_slot_boundary_ms(last_boundary_ms, period_ms);
        last_boundary_ms = boundary_ms;
        int64_t slot_sec = boundary_ms / 1000;   // whole-second slot id (record/aging)

        ft8_tx_request_t txreq;
        // Pass boundary_ms (exact, undistorted) not slot_sec - see the
        // ft8_tx_should_run_this_slot doc comment for why the whole-second
        // truncation breaks FT4 parity.
        if (ft8_tx_should_run_this_slot(boundary_ms, &txreq)) {
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
            int64_t now_ms_cap = (int64_t)tv_cap.tv_sec * 1000 + tv_cap.tv_usec / 1000;
            int start_off_ms = (int)(now_ms_cap - boundary_ms);
            // Cap the capture at the next UTC slot boundary so the window stays
            // anchored to the protocol's timing grid (period_ms = 15000 FT8 /
            // 7500 FT4). Without this the window slides ~0.2 s/slot (capture
            // takes a touch over a slot) and decoding dies after a few min; the
            // finalize step zero-pads any shortfall.
            int ms_to_boundary = period_ms - start_off_ms;
            if (ms_to_boundary < 2000)                  ms_to_boundary = 2000;
            if (ms_to_boundary > (int)SLOT_TIMEOUT_MS)  ms_to_boundary = SLOT_TIMEOUT_MS;

            // Streaming STFT: arm capture, then FFT each symbol block (1920 @ FT8,
            // 576 @ FT4) the instant it lands, so the waterfall is fully built by
            // the time the signal ends. The STFT cost overlaps capture instead of
            // being paid in the post-slot decode window.
            int64_t t0 = esp_timer_get_time();
            // Diag: ring backlog about to be discarded by the arm-time flush
            // (this is the accumulated latency that was time-shifting later
            // captures), and a per-slot drop counter snapshot. Both logged in
            // the per-slot decode line so the cliff is directly observable.
            int arm_backlog = (int)audio_ring_backlog_pairs();
            uint32_t drop_before = audio_get_dropped_total();
            esp_err_t e = dsp_ft8_capture_begin(s_cap_scratch, slot_samples);
            int     blk       = mon->block_size;        // 1920 (FT8) / 576 (FT4)
            int     n_blocks  = slot_samples / blk;      // 93 (FT8) / 156 (FT4)
            int     processed = 0;
            int64_t stft_us   = 0;
            ft8_tx_request_t late_txreq;
            bool    late_tx   = false;
            if (e == ESP_OK) {
                while (processed < n_blocks) {
                    int avail = dsp_ft8_capture_progress();
                    while ((processed + 1) * blk <= avail && processed < n_blocks) {
                        int64_t ts = esp_timer_get_time();
                        monitor_process(mon, &s_cap_scratch[processed * blk]);
                        stft_us += esp_timer_get_time() - ts;
                        processed++;
                    }
                    if (avail >= slot_samples) break;                                  // whole slot in
                    int into_slot_ms = (int)((esp_timer_get_time() - t0) / 1000);
                    if (into_slot_ms >= ms_to_boundary) break;                          // boundary
                    // Reply-on-the-immediate-slot: if the prior slot's decode just
                    // armed a reply matching THIS slot's parity, fire it now instead
                    // of waiting a full cycle. See FT8_REPLY_TX_WINDOW_MS. Safe -
                    // should_run only returns a legitimately-armed, correct-parity
                    // request, so this can never misfire a spurious/wrong-parity TX.
                    // FT8-only: this optimization's timing/parity assumptions are
                    // tuned to the 15 s FT8 grid and FT8 QSO automation isn't (yet)
                    // mirrored for FT4 (FT4 currently only supports CQ, no
                    // auto-reply) - so there's never a legitimate FT4 reply to catch
                    // here anyway. Gate explicitly rather than rely on that.
                    if (!is_ft4 && into_slot_ms <= FT8_REPLY_TX_WINDOW_MS &&
                        ft8_tx_should_run_this_slot(boundary_ms, &late_txreq)) {
                        late_tx = true;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
                // Disarm capture (stop the dsp FT8 branch writing) before we
                // either queue the decode or switch to TX on this slot.
                e = dsp_ft8_capture_finish(60);
            }

            if (late_tx) {
                // Abort this slot's RX - we're transmitting the freshly-armed reply
                // on it. Discard the partial capture (we don't RX our own TX slot).
                s_buf_busy[bi] = false;
                ESP_LOGI(TAG, "slot %d: reply armed mid-slot (+%dms) - TX on immediate slot: %s",
                         slot_idx, (int)((esp_timer_get_time() - t0) / 1000), late_txreq.display_text);
                ft8_status_set("TX: %s", late_txreq.display_text);
                ft8_tx_run(&late_txreq);          // blocks ~12.7 s; always restores RX
                ft8_qso_on_tx_complete();
                ft8_status_set("TX done - waiting for next slot");
                ft8_screen_view_request_refresh();
            } else if (e == ESP_OK) {
                // FFT any remaining whole blocks (late in-flight samples or the
                // zero-padded dead-air tail -> noise), then queue the decode.
                while ((processed + 1) * blk <= slot_samples) {
                    int64_t ts = esp_timer_get_time();
                    monitor_process(mon, &s_cap_scratch[processed * blk]);
                    stft_us += esp_timer_get_time() - ts;
                    processed++;
                }
                int cap_ms  = (int)((esp_timer_get_time() - t0) / 1000);
                int stft_ms = (int)(stft_us / 1000);
                int drop_delta = (int)(audio_get_dropped_total() - drop_before);
                decode_job_t job = { bi, slot_sec, slot_idx, cap_ms, stft_ms,
                                     start_off_ms, arm_backlog, drop_delta };
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
    s_ft8_task_alive = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// ARRL Field Day end-to-end self-test: encode -> GFSK audio synthesis ->
// the REAL on-device STFT/candidate-search/LDPC decode pipeline -> decode.
// Heap-allocated port of ft8_lib's own gen_ft8.c demo synthesizer (synth_gfsk/
// gfsk_pulse) - the original uses ~620 KB of stack-allocated VLAs, which is
// fine on a PC demo but would blow any ESP32 task stack, so every array here
// is heap_caps_malloc'd (PSRAM) instead. This is the closest thing to "did we
// just hear a real Field Day station" without an actual second radio: it
// exercises the exact same monitor_process/ftx_find_candidates/
// ftx_decode_candidate code path that runs on live RF, just fed a synthetic
// (noise-free) waveform instead of USB audio.
// ---------------------------------------------------------------------------

#define FD_E2E_GFSK_BT     2.0f          // FT8 GFSK shaping bandwidth factor
#define FD_E2E_GFSK_K      5.336446f     // pi * sqrt(2 / log(2))

static bool synth_gfsk_heap(const uint8_t *symbols, int n_sym, float f0,
                            float symbol_bt, float symbol_period,
                            int signal_rate, float *signal)
{
    int n_spsym = (int)(0.5f + signal_rate * symbol_period);
    int n_wave  = n_sym * n_spsym;
    float dphi_peak = 2.0f * (float)M_PI / n_spsym;  // hmod = 1.0

    float *dphi  = heap_caps_malloc((size_t)(n_wave + 2 * n_spsym) * sizeof(float), MALLOC_CAP_SPIRAM);
    float *pulse = heap_caps_malloc((size_t)(3 * n_spsym) * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!dphi || !pulse) {
        if (dphi) heap_caps_free(dphi);
        if (pulse) heap_caps_free(pulse);
        return false;
    }

    for (int i = 0; i < n_wave + 2 * n_spsym; i++) {
        dphi[i] = 2.0f * (float)M_PI * f0 / signal_rate;
    }

    for (int i = 0; i < 3 * n_spsym; i++) {
        float t = i / (float)n_spsym - 1.5f;
        float arg1 = FD_E2E_GFSK_K * symbol_bt * (t + 0.5f);
        float arg2 = FD_E2E_GFSK_K * symbol_bt * (t - 0.5f);
        pulse[i] = (erff(arg1) - erff(arg2)) / 2.0f;
    }

    for (int i = 0; i < n_sym; i++) {
        int ib = i * n_spsym;
        for (int j = 0; j < 3 * n_spsym; j++) {
            dphi[j + ib] += dphi_peak * symbols[i] * pulse[j];
        }
    }
    for (int j = 0; j < 2 * n_spsym; j++) {
        dphi[j]                  += dphi_peak * pulse[j + n_spsym] * symbols[0];
        dphi[j + n_sym * n_spsym] += dphi_peak * pulse[j] * symbols[n_sym - 1];
    }

    float phi = 0;
    for (int k = 0; k < n_wave; k++) {
        signal[k] = sinf(phi);
        phi = fmodf(phi + dphi[k + n_spsym], 2.0f * (float)M_PI);
    }
    int n_ramp = n_spsym / 8;
    for (int i = 0; i < n_ramp; i++) {
        float env = (1.0f - cosf(2.0f * (float)M_PI * i / (2 * n_ramp))) / 2.0f;
        signal[i] *= env;
        signal[n_wave - 1 - i] *= env;
    }

    heap_caps_free(dphi);
    heap_caps_free(pulse);
    return true;
}

// Runs on its own task (see ft8_arrl_fd_e2e_selftest() below) - the
// FFT/monitor machinery needs a large stack (same as ft8_task's 65536 bytes),
// far more than the "main" task's 8 KB (CONFIG_ESP_MAIN_TASK_STACK_SIZE).
// Running this directly in app_main's call stack caused an immediate stack
// protection fault.
static void ft8_arrl_fd_e2e_selftest_task(void *arg)
{
    (void)arg;
    const char *call_to = "K1ABC";
    const char *call_de = "OZ1LAV";
    const char *extra   = "R 16A EMA";
    const float tone_hz = 1500.0f;

    ftx_message_t msg;
    if (ftx_message_encode_arrl_fd(&msg, NULL, call_to, call_de, extra) != FTX_MESSAGE_RC_OK) {
        ESP_LOGE(TAG, "FD e2e selftest: FAIL (encode)");
        vTaskDelete(NULL);
        return;
    }

    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    int n_spsym = (int)(0.5f + SR_HZ * FT8_SYMBOL_PERIOD);  // 1920
    int n_wave  = FT8_NN * n_spsym;                          // 151680 (~12.64 s)

    float *signal = heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!signal) {
        ESP_LOGE(TAG, "FD e2e selftest: FAIL (signal alloc)");
        vTaskDelete(NULL);
        return;
    }
    memset(signal, 0, SLOT_SAMPLES * sizeof(float));

    // Start ~0.5s in, like a real capture's burst-within-slot framing.
    int start = (int)(0.5f * SR_HZ);
    if (start + n_wave > SLOT_SAMPLES) start = 0;
    bool synth_ok = synth_gfsk_heap(tones, FT8_NN, tone_hz, FD_E2E_GFSK_BT,
                                    FT8_SYMBOL_PERIOD, SR_HZ, signal + start);
    if (!synth_ok) {
        ESP_LOGE(TAG, "FD e2e selftest: FAIL (synth alloc)");
        heap_caps_free(signal);
        vTaskDelete(NULL);
        return;
    }

    const monitor_config_t cfg = {
        .f_min = 200.0f, .f_max = 3000.0f, .sample_rate = SR_HZ,
        .time_osr = 2, .freq_osr = 2, .protocol = FTX_PROTOCOL_FT8,
    };
    monitor_t *mon = heap_caps_malloc(sizeof(monitor_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mon) {
        ESP_LOGE(TAG, "FD e2e selftest: FAIL (monitor alloc)");
        heap_caps_free(signal);
        vTaskDelete(NULL);
        return;
    }
    monitor_init(mon, &cfg);

    int blk = mon->block_size;
    int n_blocks = SLOT_SAMPLES / blk;
    for (int i = 0; i < n_blocks; i++) {
        monitor_process(mon, &signal[i * blk]);
    }

    ftx_candidate_t cands[FT8_MAX_CANDIDATES];
    int n_cand = ftx_find_candidates(&mon->wf, FT8_MAX_CANDIDATES, cands, FT8_FIND_MIN_SCORE);

    bool found = false;
    for (int i = 0; i < n_cand; i++) {
        ftx_message_t dec_msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon->wf, &cands[i], FT8_LDPC_MAX_ITERS, &dec_msg, &st)) continue;
        if (ftx_message_get_type(&dec_msg) != FTX_MESSAGE_TYPE_ARRL_FD) continue;

        char dec_to[16], dec_de[16], dec_extra[20];
        ftx_field_t ft[FTX_MAX_MESSAGE_FIELDS];
        if (ftx_message_decode_arrl_fd(&dec_msg, NULL, dec_to, dec_de, dec_extra, ft) != FTX_MESSAGE_RC_OK) continue;

        bool ok = strcmp(dec_to, call_to) == 0 && strcmp(dec_de, call_de) == 0 && strcmp(dec_extra, extra) == 0;
        ESP_LOGI(TAG, "FD e2e selftest: candidate -> '%s' '%s' '%s' (score=%d, match=%d)",
                 dec_to, dec_de, dec_extra, cands[i].score, ok);
        if (ok) { found = true; break; }
    }

    if (found) {
        ESP_LOGI(TAG, "FD e2e selftest: PASS - full encode->GFSK-audio->STFT->LDPC->decode "
                       "pipeline round-tripped '%s' '%s' '%s' (%d candidate(s) total)",
                 call_to, call_de, extra, n_cand);
    } else {
        ESP_LOGE(TAG, "FD e2e selftest: FAIL - %d candidate(s) found, none matched", n_cand);
    }

    monitor_free(mon);
    heap_caps_free(mon);
    heap_caps_free(signal);
    vTaskDelete(NULL);
}

bool ft8_synth_and_decode(const ftx_message_t *msg, float tone_hz,
                          char *out_text, size_t out_len,
                          int *out_snr_db, int *out_score)
{
    if (!msg || !out_text || !out_len) return false;

    uint8_t tones[FT8_NN];
    ft8_encode(msg->payload, tones);

    int n_spsym = (int)(0.5f + SR_HZ * FT8_SYMBOL_PERIOD);
    int n_wave  = FT8_NN * n_spsym;

    float *signal = heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!signal) return false;
    memset(signal, 0, SLOT_SAMPLES * sizeof(float));

    int start = (int)(0.5f * SR_HZ);
    if (start + n_wave > SLOT_SAMPLES) start = 0;
    if (!synth_gfsk_heap(tones, FT8_NN, tone_hz, FD_E2E_GFSK_BT, FT8_SYMBOL_PERIOD, SR_HZ, signal + start)) {
        heap_caps_free(signal);
        return false;
    }

    const monitor_config_t cfg = {
        .f_min = 200.0f, .f_max = 3000.0f, .sample_rate = SR_HZ,
        .time_osr = 2, .freq_osr = 2, .protocol = FTX_PROTOCOL_FT8,
    };
    monitor_t *mon = heap_caps_malloc(sizeof(monitor_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mon) {
        heap_caps_free(signal);
        return false;
    }
    monitor_init(mon, &cfg);

    int blk = mon->block_size;
    int n_blocks = SLOT_SAMPLES / blk;
    for (int i = 0; i < n_blocks; i++) {
        monitor_process(mon, &signal[i * blk]);
    }

    ftx_candidate_t cands[FT8_MAX_CANDIDATES];
    int n_cand = ftx_find_candidates(&mon->wf, FT8_MAX_CANDIDATES, cands, FT8_FIND_MIN_SCORE);

    bool found = false;
    float noise_db = 0.0f;
    bool have_noise = false;
    for (int i = 0; i < n_cand; i++) {
        ftx_message_t dec_msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon->wf, &cands[i], FT8_LDPC_MAX_ITERS, &dec_msg, &st)) continue;
        ftx_message_offsets_t off;
        if (ftx_message_decode(&dec_msg, NULL, out_text, &off) != FTX_MESSAGE_RC_OK) continue;

        if (out_score) *out_score = cands[i].score;
        if (out_snr_db) {
            if (!have_noise) { noise_db = ft8_estimate_noise_db(mon); have_noise = true; }
            *out_snr_db = (int)lroundf(ft8_estimate_snr_db(mon, &cands[i], noise_db));
        }
        found = true;
        break;
    }

    monitor_free(mon);
    heap_caps_free(mon);
    heap_caps_free(signal);
    return found;
}

// Quick check that ft8_synth_and_decode() also round-trips a plain STANDARD
// message (not just ARRL FD) - the FT8 simulation mode's phantom CQ stations
// use this exact path with ordinary "CQ <call> <grid>" text, so this is the
// fastest way to confirm/rule out the synth+decode pipeline itself as the
// cause if sim mode silently does nothing.
static void ft8_sim_synth_selftest_task(void *arg)
{
    (void)arg;
    ftx_message_t msg;
    if (ftx_message_encode_std(&msg, NULL, "CQ", "W1AW", "FN31") != FTX_MESSAGE_RC_OK) {
        ESP_LOGE(TAG, "sim synth selftest: FAIL (encode)");
        vTaskDelete(NULL);
        return;
    }
    char text[FTX_MAX_MESSAGE_LENGTH];
    int snr = 0, score = 0;
    if (ft8_synth_and_decode(&msg, 800.0f, text, sizeof(text), &snr, &score)) {
        bool ok = (strcmp(text, "CQ W1AW FN31") == 0);
        ESP_LOGI(TAG, "sim synth selftest: %s decoded='%s' snr=%d score=%d",
                 ok ? "PASS" : "FAIL (mismatch)", text, snr, score);
    } else {
        ESP_LOGE(TAG, "sim synth selftest: FAIL (no candidate decoded)");
    }
    vTaskDelete(NULL);
}

void ft8_sim_synth_selftest(void)
{
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_sim_synth_selftest_task, "sim_synth_test", 65536, NULL,
        tskIDLE_PRIORITY + 1, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "sim synth selftest: failed to spawn task (rc=%d)", (int)rc);
    }
}

void ft8_arrl_fd_e2e_selftest(void)
{
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_arrl_fd_e2e_selftest_task, "fd_e2e_test", 65536, NULL,
        tskIDLE_PRIORITY + 1, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "FD e2e selftest: failed to spawn test task (rc=%d)", (int)rc);
    }
}

// ---------------------------------------------------------------------------
// ARRL Field Day encode/decode round-trip self-test
// ---------------------------------------------------------------------------

void ft8_arrl_fd_selftest(void)
{
    struct { const char *call_to, *call_de, *extra; } cases[] = {
        { "WA9XYZ", "KA1ABC", "R 16A EMA" },
        { "WA9XYZ", "KA1ABC", "R 32A EMA" },
        { "K1ABC",  "W2DEF",  "3A NNJ" },
        { "AA1AA",  "BB2BB",  "1A DX" },
        { "AA1AA",  "BB2BB",  "32F TER" },
        { "AA1AA",  "BB2BB",  "16A WCF" },
        { "AA1AA",  "BB2BB",  "17A WCF" },
    };
    int n_pass = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ftx_message_t msg;
        ftx_message_rc_t rc = ftx_message_encode_arrl_fd(&msg, NULL, cases[i].call_to, cases[i].call_de, cases[i].extra);
        if (rc != FTX_MESSAGE_RC_OK) {
            ESP_LOGE(TAG, "ARRL FD selftest FAIL (encode rc=%d): '%s' '%s' '%s'",
                     (int)rc, cases[i].call_to, cases[i].call_de, cases[i].extra);
            continue;
        }
        if (ftx_message_get_type(&msg) != FTX_MESSAGE_TYPE_ARRL_FD) {
            ESP_LOGE(TAG, "ARRL FD selftest FAIL (wrong type): '%s' '%s' '%s'",
                     cases[i].call_to, cases[i].call_de, cases[i].extra);
            continue;
        }
        char dec_to[16], dec_de[16], dec_extra[20];
        ftx_field_t field_types[FTX_MAX_MESSAGE_FIELDS];
        rc = ftx_message_decode_arrl_fd(&msg, NULL, dec_to, dec_de, dec_extra, field_types);
        bool ok = (rc == FTX_MESSAGE_RC_OK) &&
                  strcmp(dec_to, cases[i].call_to) == 0 &&
                  strcmp(dec_de, cases[i].call_de) == 0 &&
                  strcmp(dec_extra, cases[i].extra) == 0;
        if (ok) {
            n_pass++;
            ESP_LOGI(TAG, "ARRL FD selftest PASS: '%s' '%s' '%s'",
                     cases[i].call_to, cases[i].call_de, cases[i].extra);
        } else {
            ESP_LOGE(TAG, "ARRL FD selftest FAIL (roundtrip): sent '%s' '%s' '%s' -> got '%s' '%s' '%s' (rc=%d)",
                     cases[i].call_to, cases[i].call_de, cases[i].extra,
                     dec_to, dec_de, dec_extra, (int)rc);
        }
    }
    int n_total = (int)(sizeof(cases) / sizeof(cases[0]));
    if (n_pass == n_total) {
        ESP_LOGI(TAG, "ARRL FD selftest: ALL %d/%d PASSED", n_pass, n_total);
    } else {
        ESP_LOGE(TAG, "ARRL FD selftest: ONLY %d/%d PASSED", n_pass, n_total);
    }
}

// ---------------------------------------------------------------------------
// Public entry point: spawn the capture task
// ---------------------------------------------------------------------------

void ft8_self_test(void)
{
    // Single-instance guard: never spawn a second ft8_task while one is still
    // alive (e.g. a fast Panadapter<->FT8 toggle before the previous capture
    // task finished its slot/teardown). Two instances clobber the shared
    // s_decode_queue and crash one decode task on xQueueReceive(NULL). The
    // surviving task keeps serving FT8 (its loop re-checks ui_mode each slot).
    if (s_ft8_task_alive) {
        ESP_LOGW(TAG, "ft8_self_test: an FT8 task is still alive; not spawning a second");
        return;
    }
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_task, "ft8", 65536, NULL,
        tskIDLE_PRIORITY + 1, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn ft8_task (rc=%d)", (int)rc);
    }
}

bool ft8_task_is_alive(void)
{
    return s_ft8_task_alive;
}
