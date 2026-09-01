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
#include "ft8_slot_gate.h"

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
#include "ft8_pileup.h"
#include "ui/ft8_screen_view.h"
#include "ft8_tx.h"
#include "ft8_qso.h"
#include "ft8_robot.h"
#include "ft8_hound.h"
#include "net/pskreporter.h"
#include "ft8_hash.h"
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

// Below this much free internal RAM the per-slot line pays for a
// largest-free-block walk; above it, never. See the comment at the call site -
// that walk runs with interrupts off and is the documented cause of the
// full-screen cyan flash on this panel.
#define FT8_SLOT_LBLK_EMERGENCY_KB  24
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
#if FT4_MODE_DISABLED
    m = FT8_OP_MODE_FT8;   // see FT4_MODE_DISABLED's comment in ft8_test.h
#endif
    // Changing sub-mode invalidates everything already on screen: the two
    // protocols have different slot lengths, so rows decoded under the old
    // timing describe a band that no longer exists as far as the new engine is
    // concerned - and a stale row is tappable, i.e. a pounce at a station we
    // can no longer hear on this timing. The pileup goes with it for the same
    // reason. Done HERE rather than at the call site so a future way of
    // changing sub-mode can't forget it (apply_freq_preset() in
    // ft8_screen_view.c also clears, which additionally covers a plain band
    // change - the harmless overlap is deliberate).
    // Guarded on an ACTUAL change so re-selecting the current mode, or a
    // no-op coercion under FT4_MODE_DISABLED, doesn't wipe a live list.
    bool changed = (s_op_mode_loaded && s_op_mode != m);
    s_op_mode = m;
    s_op_mode_loaded = true;   // a deliberate set always wins over the lazy load
    settings_set_ft8_op_mode((uint8_t)m);   // sticky across reboot (debounced flush)
    ESP_LOGI(TAG, "operating sub-mode -> %s", m == FT8_OP_MODE_FT4 ? "FT4" : "FT8");
    if (changed) {
        ft8_screen_clear();
        ft8_pileup_clear();
        ESP_LOGI(TAG, "sub-mode changed - decode list and pileup cleared");
    }
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
#if FT4_MODE_DISABLED
        // A device that had FT4 selected before this change must not come
        // back up in FT4 - see FT4_MODE_DISABLED's comment in ft8_test.h.
        s_op_mode = FT8_OP_MODE_FT8;
#endif
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
//
// 2026-07-22: widened 2500 -> 2800 (Roy KI0ER field report - a hand-tapped
// pounce/Transmit often lands just past 2.5 s and slipped a full 30 s cycle).
// This is the hard ceiling: the decoder's own candidate time search is
// -10..+19 blocks = DT -1.6..+3.0 s (ft8_lib decode.c), and the burst's first
// tone fires ~TX-command latency after this window, so pushing past ~2.8 s
// would start transmitting outside the range the far end can decode - strictly
// worse than waiting for the next slot. Do NOT raise further without moving the
// decoder's +19-block search bound too.
/* ⛔ RETIRED - do not reinstate. 2800 ms was looser than EITHER protocol's
 * room (FT8 has 15000-12640 = 2360 ms, FT4 7500-5040 = 2460 ms), so a reply
 * fired at the window edge overran its own slot. It rarely bit only because
 * decodes usually land about a second in. The window is now derived per
 * protocol by ft8_gate_reply_window_ms() as slot - burst - margin: 2260 ms
 * for FT8, 2360 ms for FT4. Putting 2800 back is a caught mutation. */
/* #define FT8_REPLY_TX_WINDOW_MS 2800  - see ft8_slot_gate.h */

// Hold-for-decode gate (the "everything goes twice" fix). During an active
// QSO, the just-ended RX slot's decode is ALWAYS still running at the next
// boundary (capture deliberately runs TO the boundary; decode then takes
// ~1.5-1.9 s), so the partner's reply is found ~1.7 s AFTER our re-armed
// previous message has already gone on-air - every exchange step used to be
// transmitted twice. Instead of firing the ARMED request at the boundary, the
// slot loop now waits (mid-capture, via the FT8_REPLY_TX_WINDOW_MS machinery
// above) until ft8_qso_advance() has processed that decode and replaced the
// armed content with the fresh message, then fires it in THIS slot - a burst
// started ~2 s late is well inside every decoder's time-search range (see the
// reply-window comment above; validated on-air at +1032 ms). If the decode
// somehow hasn't landed by this deadline, fire whatever is armed - worst case
// is exactly the old resend behaviour, never a skipped slot. Must be < the
// FT8_REPLY_TX_WINDOW_MS poll cutoff or a late decode ends in NO TX at all.
/* #define FT8_TX_HOLD_DEADLINE_MS 2300  - now ft8_gate_hold_deadline_ms() */

// Early-decode (the "reply at dt~=0" path, WSJT-X-style). All the hold/reply
// machinery above ships the fresh reply ~1-2 s late for one reason: the
// partner's decode doesn't even START until the 15 s boundary (capture runs to
// the boundary, decode is then ~1.5-1.9 s). But the FT8 signal itself ends at
// ~12.64 s - the last 2.36 s of every slot is dead air. So mid-QSO, where the
// ONE station whose timing matters is our partner (who is dt~=0 and therefore
// fully inside that 12.64 s), we cut the capture a hair past the signal and
// queue the decode ~2 s early. The decode then finishes BEFORE the next
// boundary, ft8_qso_advance() arms the fresh reply in time, and the normal
// boundary TX path fires it at dt~=0 - no doubles, no cycle lost, and a low dt
// for every receiving station. If a busy-band decode still overruns the
// boundary the hold/reply machinery above catches it exactly as before (small
// positive dt, never a skipped slot). FT8-only, and ONLY while
// ft8_qso_is_busy() (an exchange or CQ-run) - plain monitoring keeps the
// full-slot capture so band decode yield is untouched. RESERVE = time left
// before the boundary for the decode to run; cut point = period - RESERVE
// (13.2 s), still 0.56 s past the 12.64 s signal end.
#define FT8_DECODE_RESERVE_MS 1800

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
// 2026-07-19 (#51): raised 15 -> 30. The 30->15 cut above was ONLY to shrink
// dec_ms so a QSO reply decoded before our next TX fired — a problem since made
// obsolete by the hold-for-decode TX gate (a4d8564), which holds the reply until
// the decode lands regardless of dec_ms. Meanwhile the decode budget runs ~85%
// idle (dec_ms ~1.8 s of an 11 s budget, dual-core split), so 15 iters was
// leaving weak-signal decodes on the table for no remaining benefit. bp_decode()
// only exits early on success, so marginal candidates that need 15-30 iterations
// to converge were being abandoned at 15. Yield-motivated; measure decodes/slot.
#define FT8_LDPC_MAX_ITERS    30
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
// Hold-for-decode gate bookkeeping (see FT8_TX_HOLD_DEADLINE_MS). queued is
// bumped by the capture task when it posts a decode_job_t; done is bumped by
// the decode task after decode_slot() (and therefore ft8_qso_advance() + the
// fresh re-arm inside it) has fully returned. queued != done means a decode
// whose result could supersede the ARMED request is still in flight. One
// writer each, single-word RV32 stores - volatile is sufficient.
static volatile uint32_t s_decode_jobs_queued = 0;
static volatile uint32_t s_decode_jobs_done   = 0;
// True for the whole lifetime of one ft8_task (capture) instance. A fast
// Panadapter<->FT8 toggle could otherwise spawn a SECOND ft8_task before the
// first finished tearing down; the two clobber the shared s_decode_queue and
// one decode task ends up calling xQueueReceive(NULL) -> assert/reboot.
/* Up to ~15 s: wspr_rx frees on its next loop pass, measured at ~2.6 s, and a
 * WSPR cycle is 120 s so a pathological case wants room. */
#define FT8_ALLOC_TRIES    16
#define FT8_ALLOC_WAIT_MS 1000

static volatile bool  s_ft8_task_alive = false;

// Timing error from the last decoded slot: positive = system clock is fast.
// Written by ft8_decode_task; read by the LVGL UI (ft8_time_modal).
// volatile int is sufficient — single writer, single reader, display hint only.
static volatile int      s_last_timing_ms    = 0;   // RAW measured offset (noisy on low-decode slots)
static volatile bool     s_last_timing_valid = false;
// The REAL correction actually pushed to the UTC clock this slot (0 = none:
// skipped because <MIN_SAMPLES decodes or <MIN_MS, or clamped to 0). This -
// not the raw measurement above - is what the time modal shows as the "nudge",
// so a +/-2 s single-station raw reading never looks like a 2 s clock jump.
static volatile int      s_last_applied_ms   = 0;
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

// The real per-slot correction actually applied to the clock (see s_last_applied_ms).
bool ft8_get_last_applied_ms(int *out_ms)
{
    if (!s_last_timing_valid) return false;
    if (out_ms) *out_ms = s_last_applied_ms;
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

// Slot-start UTC of the last slot we actually RECEIVED, per transmit window.
// Written only where a capture completed, read by the UI to tell "this window is
// free" from "we were transmitting into this window and have no idea".
static volatile int64_t s_last_rx_utc_even = 0;
static volatile int64_t s_last_rx_utc_odd  = 0;

// Slot-start UTC of the last slot we TRANSMITTED in, per window. The counterpart
// of the pair above, and it exists because their comment names a state nobody was
// recording: "we were transmitting into this window and have no idea".
//
// ⛔ Without this the auto-answer readiness gate could not tell "I have not heard
// this window" from "I was busy talking in it", and treated our own transmission
// as a failure to map the band. Roy KI0ER, v1.8.7: after every QSO the robot sat
// out a cycle or two before answering the next CQ, by which time a POTA activator
// had moved on - his "waits 30 seconds". See ft8_robot_occupancy_ready().
static volatile int64_t s_last_tx_utc_even = 0;
static volatile int64_t s_last_tx_utc_odd  = 0;

int64_t ft8_last_rx_utc_for_parity(bool even)
{
    return even ? s_last_rx_utc_even : s_last_rx_utc_odd;
}

int64_t ft8_last_tx_utc_for_parity(bool even)
{
    return even ? s_last_tx_utc_even : s_last_tx_utc_odd;
}

// Called from both TX paths in the slot loop. `slot_sec` is the slot's start UTC.
static void note_tx_slot(int64_t slot_sec, int slot_index)
{
    if ((slot_index % 2) == 0) s_last_tx_utc_even = slot_sec;
    else                       s_last_tx_utc_odd  = slot_sec;
}
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

/* ⛔ DROP OUR REFERENCES TO THE CAPTURE POOL WITHOUT FREEING ANY OF IT.
 *
 * For the ONE case where freeing is the crash: the decode task did not exit
 * within ft8_task's bounded wait, so its core-0 worker may still be reading a
 * monitor's waterfall. Hardware-captured 2026-09-01 on v1.10.5 - a Load access
 * fault in estimate_snr_db (ft8_lib decode.c:471) reading block[bin], MTVAL a
 * garbage pointer, 20 s AFTER `ft8_task exiting` had already logged.
 *
 * This is the rule the rest of the teardown already follows and this one path
 * ignored: reap_pending_tasks() deliberately reaps nothing on a join timeout,
 * and the worker context is deliberately LEAKED with the comment "a small leak
 * beats a crash". The pool is the same trade at a larger size - two waterfalls
 * (163 KB each in FT8, 82 KB in FT4) plus scratch, in PSRAM, where there are
 * ~15 MB free, and only on a pathological timeout.
 *
 * The next FT8 entry builds a fresh pool because every pointer here is NULL.
 *
 * ⚠ Do NOT "fix" the underlying starvation by widening the 15 s wait. The
 * worker is tskIDLE_PRIORITY+1 on core 0, and returning to the panadapter puts
 * taskLVGL back on that core at ~74 % - the log showed `idle0 0.0%` at the
 * moment of the fault. A longer bound changes the odds, not the race; this
 * file already records two fixes falsified for exactly that reasoning. */
static void forget_capture_pool(void)
{
    for (int i = 0; i < FT8_NUM_BUFFERS; i++) s_mon_pool[i] = NULL;
    s_shared_fft_work     = NULL;
    s_shared_fft_cfg      = NULL;
    s_shared_fft_internal = false;
    s_cap_scratch         = NULL;
    s_pool_proto          = -1;
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
    // ⛔ The shared buffer from a PREVIOUS build, captured before it is
    // overwritten below. On a rebuild that skipped the teardown, every monitor
    // still points at it - and the loop further down used to free
    // s_mon_pool[i]->fft_work unconditionally, so the SAME block was freed once
    // per monitor. That is a tlsf double-free and it reboots the device.
    //
    // Serial-captured 2026-08-19 switching into FT8, and it explains the
    // "one unreproduced tlsf_free double-free" that had been open since v1.3.0:
    //     W ft8_view: FT8 view visible but no ft8_task alive - respawning
    //     I ft8_test: monitor pool built for FT8
    //     assert failed: tlsf_free - block already marked as free
    // The respawn watchdog raced the normal FT8-entry path and the pool was
    // built twice with no teardown between.
    //
    // The two TEARDOWN paths already make exactly this check before freeing the
    // shared buffer once (see free_monitor_pool / reinit_pool_if_mode_changed);
    // only the build path was missing it.
    void *prev_shared = s_shared_fft_work;

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
        //
        // Free ONLY a buffer this monitor owns. A pointer equal to a shared
        // buffer belongs to the pool, and freeing it here frees it once per
        // monitor - see the prev_shared note above.
        void *w = s_mon_pool[i]->fft_work;
        if (w && w != prev_shared && w != s_shared_fft_work) free(w);
        s_mon_pool[i]->fft_work = s_shared_fft_work;
        s_mon_pool[i]->fft_cfg  = s_shared_fft_cfg;
    }

    if (prev_shared) {
        // The pool was rebuilt without a teardown, which should not happen. The
        // old shared buffer is now unreferenced, but a decode may still be
        // running against it on the other core - so it is deliberately LEAKED
        // once rather than risk a use-after-free, which would be harder to
        // diagnose than the leak and just as fatal.
        //
        // LOUD on purpose: this is the double-build itself, and #199 is about
        // stopping it happening rather than surviving it. A silent survival
        // would leave the root cause invisible, which is how it stayed open for
        // months in the first place.
        ESP_LOGW(TAG, "monitor pool rebuilt with no teardown - old shared FFT "
                      "scratch leaked (see #199). Was the ft8_task respawned?");
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

// Core-0 worker comms, ONE HEAP CONTEXT PER DECODE-TASK INSTANCE - deliberately
// NOT statics. These were three shared static handles, and Dennis WN4FLA's
// field crash (serial-captured 2026-08-03, "assert failed: xQueueGenericSend
// queue.c:936 (pxQueue)" in ft8_decode_worker_task) was two instances crossing
// them: ft8_task's teardown waits a bounded time for the decode task, whose own
// worker join can outlast it - ft8_task then declared itself dead, a new FT8
// session spawned, its decode task RECREATED the statics, and the OLD decode
// task's teardown deleted/NULLed the NEW instance's handles out from under its
// live worker. With the handles owned per-instance (worker gets its context as
// its task arg; decode_slot gets it as a parameter), overlapping instances
// cannot touch each other's comms. exited: worker gives it just before
// parks; the owning decode task JOINS on it, then reaps it and deletes the handles
// (and, on join timeout, deliberately LEAKS the context - a zombie worker may
// still hold it, and a small leak beats a crash).
typedef struct {
    QueueHandle_t     jobs;    // carries worker_job_t*
    SemaphoreHandle_t done;
    SemaphoreHandle_t exited;
} worker_ctx_t;

// True from ft8_decode_task entry until just before it parks. The
// ft8_task single-instance guard must cover the DECODE task too - ft8_task's
// bounded teardown wait can expire with the decode task still finishing, and
// spawning a new session in that window is what crossed the worker handles.
static volatile bool s_decode_task_alive = false;

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
//
// EXCEPTION: FT8 Simulation Mode needs no radio at all (phantom stations,
// ft8_tx.c's interlock never keys anything), yet this wait sat ahead of the
// entire slot loop - so with no QMX the sim could inject CQs (its own task)
// but an armed pounce/CQ TX never fired: the loop that fires it was parked
// right here showing "Waiting for QMX - check USB/power". Poll sim_mode_en
// INSIDE the loop (not just once) so enabling sim while already waiting
// unblocks immediately.
static void wait_for_cat_ready(void)
{
    int64_t t0 = esp_timer_get_time();
    int64_t last_update = t0;
    while (!cat_is_ready()) {
        qmx_settings_t s;
        settings_load_all(&s);
        if (s.sim_mode_en) {
            ESP_LOGI(TAG, "sim mode ON - not waiting for QMX (no radio needed)");
            return;
        }
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

// (The former per-slot rate clamp FT8_AUTOSYNC_MAX_STEP_MS was removed 2026-07-18:
// it was a RATE cap, which never prevents reaching an offset - only slows the
// approach - and it fought legitimate convergence to the band consensus. Runaway
// protection is now a POSITION leash in time_sync (FT8_LEASH_MS), bounding the
// cumulative pull from the SNTP/GPS anchor instead of the per-slot step. NOTE:
// FT4's noisier raw (±800-950 ms field data) × the 0.3 gain still allows a
// ~250 ms single-slot step; that's within the 500 ms leash and self-corrects,
// but if FT4 ever looks jumpy on screen, the fix is a LOWER FT4 gain (more
// damping) - not a return to the arbitrary rate cap.)

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
    (void)start_off_ms;   // no longer added to the timing (backfill anchors the
                          // buffer to the boundary); kept for the future robust
                          // incomplete-backfill fix. See the timing calc below.
    int max_iters = (s_pool_proto == (int)FTX_PROTOCOL_FT4) ? FT4_LDPC_MAX_ITERS : FT8_LDPC_MAX_ITERS;
    // Real decodes are suppressed from the shared decode list while sim mode
    // is on - otherwise a real QMX still attached and receiving keeps
    // re-populating the list with genuine stations moments after ft8_sim.c
    // clears it on entry, which read as "stations flickering back and forth
    // before finally fading" (they were real, still being decoded, and only
    // stopped once they aged out or the band moved on). Sim mode's whole
    // point is a view isolated from whatever is actually on the air right
    // now, same principle the TX interlock already applies and pskreporter.c
    // already applies to spot reporting - this closes the matching gap on
    // the RX/display side. Loaded once per call (both decode-task cores),
    // not per candidate - settings_load_all() is a cheap RAM-cached read.
    qmx_settings_t sim_qs;
    settings_load_all(&sim_qs);
    bool sim_suppresses_real = sim_qs.sim_mode_en;
    for (int i = start; i < n_cand; i += step) {
        /* ⭐ ABANDON THE SLOT THE MOMENT WE ARE TEARING DOWN (#312).
         *
         * ft8_task clears s_ft8_running BEFORE it sends the stop sentinel, so
         * by the time a teardown is under way this loop is doing work whose
         * results nobody will ever see - the decode list is about to be torn
         * down with the pool.
         *
         * Finishing it anyway is what made the teardown overrun its bound:
         * measured 2026-09-01, ~19.2 s against 15 s, because the worker is
         * tskIDLE_PRIORITY+2 on core 0 and leaving FT8 puts taskLVGL (priority
         * 4) back on that core - `idle0 0.0%` at the moment of the fault. The
         * timeout then leaked the pool (see forget_capture_pool), and at
         * ~905 KB a time that is not something a station switching modes can
         * afford to pay repeatedly.
         *
         * ⛔ Deliberately NOT a priority raise, which was the obvious fix and
         * is worse here: the handle belongs to the decode task, which is
         * itself BLOCKED waiting on this worker at that moment, and reaching
         * it from ft8_task means republishing a handle across instances -
         * exactly the cross-instance sharing that caused Dennis WN4FLA's
         * crash and that worker_ctx_t exists to prevent.
         *
         * Costs the final slot's decodes on the way out, which is precisely
         * what the operator asked for by leaving. */
        if (!s_ft8_running) break;
        if ((int)((esp_timer_get_time() - t_start_us) / 1000) >= FT8_DECODE_BUDGET_MS) {
            break;
        }
        out->n_attempted++;
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon->wf, &cands[i], max_iters, &msg, &st)) continue;
        char text[FTX_MAX_MESSAGE_LENGTH];
        ftx_message_offsets_t off;
        if (ftx_message_decode(&msg, ft8_hash_if(), text, &off) == FTX_MESSAGE_RC_OK) {
            out->n_decoded++;
            // SR=12000. block_size/subblock_size come from mon itself rather than
            // being hardcoded to FT8's 1920/960 - FT4 uses 576/288, and using the
            // FT8 constants for FT4 candidates silently produced a ~3.3x-too-large
            // (and wrongly-scaled) timing offset.
            //
            // NOTE (2026-07-18): this candidate position is ALREADY relative to
            // the UTC slot boundary, because dsp_ft8_capture_begin() backfills
            // the boundary->arm gap from the pre-ring so buffer[0] == the
            // boundary (the v0.20.0 boundary-anchoring change). It must therefore
            // NOT be re-anchored with start_off_ms. It used to add start_off_ms -
            // correct in the OLD no-backfill design where buffer[0] == arm time
            // (a boundary-synced station then decoded at -start_off_ms, and the
            // two cancelled to 0). With backfill that add became a DOUBLE-COUNT:
            // since start_off_ms is always >= 0 (capture never arms before the
            // boundary), every measurement gained a persistent + bias (mean
            // ~+49 ms in field data) that the auto-sync integrated straight to
            // the +500 ms leash - holding the clock, and our absolute TX timing,
            // that far off true. Removing the term: on-air data shows the real
            // per-slot boundary offset (timing - off) already averages ~0, i.e.
            // the band is well synced, so the raw candidate position IS the
            // correct measurement. (Edge case left unhandled: an incomplete
            // backfill on a badly stalled slot with huge start_off_ms - rare, and
            // those slots decode poorly anyway.)
            // This candidate's boundary-relative slot timing (ms). Feeds both
            // the slot's robust-mean auto-sync AND the per-station DT recorded
            // below (for the DT-follow-partner feature - ft8_qso).
            float cand_dt_ms = (cands[i].time_offset * mon->block_size +
                                cands[i].time_sub * mon->subblock_size) / 12.0f;
            if (out->n_timing < FT8_MAX_CANDIDATES) {
                out->timing[out->n_timing++] = cand_dt_ms;
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
            ESP_LOGI(TAG, "decoded: '%s' (score=%d freq=%dHz snr=%d dt=%d)",
                     text, cands[i].score, freq_hz, snr_db, (int)lroundf(cand_dt_ms));
            if (!sim_suppresses_real) {
                ft8_screen_record_decode(text, cands[i].score, snr_db, freq_hz, slot_sec,
                                         (int)lroundf(cand_dt_ms));
            }
            // PSK Reporter spot (REAL decodes only - this path never runs on
            // simulator injections, which bypass the audio pipeline entirely;
            // pskreporter.c additionally refuses spots while sim mode is on).
            {
                char sp_call[16], sp_grid[8];
                if (ft8_screen_extract_call(text, sp_call, sizeof(sp_call))) {
                    ft8_screen_extract_grid(text, sp_grid, sizeof(sp_grid));
                    pskreporter_spot(sp_call, sp_grid,
                                     cat_get_frequency() + (uint32_t)freq_hz, snr_db,
                                     s_pool_proto == (int)FTX_PROTOCOL_FT4 ? "FT4" : "FT8",
                                     slot_sec);
                }
            }
        }
    }
}

// Persistent core-0 decode helper: waits for one sub-range job, decodes it,
// signals completion. Only one job is ever in flight (the decode task blocks on
// ctx->done before issuing the next).
/* ---------------------------------------------------------------------------
 * Reaping a WithCaps task (#279)
 *
 * Every task in this file is created with xTaskCreatePinnedToCoreWithCaps() so
 * its 32-64 KB stack comes from PSRAM. Such a task MUST be freed with
 * vTaskDeleteWithCaps(); a plain vTaskDelete() leaves the stack and TCB
 * allocated for ever, which IDF says outright in idf_additions.c: "the idle
 * task will not free the task TCB and stack memories we created statically
 * during xTaskCreateWithCaps() ... Therefore, it will leak memory."
 *
 * That was measured here, not inferred: three Panadapter/WSPR round trips took
 * PSRAM 15153 -> 14946 -> 14540 -> 14498 KB, about 218 KB per FT8 exit - the
 * 160 KB of stacks plus the shared FFT scratch. Roughly 70 mode toggles would
 * exhaust free PSRAM, and the symptom would be FT8 quietly refusing to start.
 *
 * ⛔ THE OBVIOUS FIX IS WORSE THAN THE BUG. Calling vTaskDeleteWithCaps(NULL)
 * on yourself makes IDF spawn a temporary cleanup task to do the freeing, from
 * INTERNAL RAM, and abort() if it cannot get it. A task-create failure from
 * internal RAM is not hypothetical on this board - it is what we had just
 * finished watching happen 390 times - and an abort() is a warm reset, which
 * with the radio attached is the documented #74 QMX wedge. That trades a slow
 * leak for a reboot that kills the operator's radio.
 *
 * So a finished task PARKS itself and registers its handle here, and an owner
 * reaps it from an ordinary context. vTaskDeleteWithCaps() called on ANOTHER
 * task suspends the target, spins until it is provably not eRunning, deletes
 * it and frees the buffers - no temporary task, no abort, nothing to fail.
 * ------------------------------------------------------------------------- */
#define REAP_MAX 4
static TaskHandle_t  s_reap[REAP_MAX];
static portMUX_TYPE  s_reap_lock = portMUX_INITIALIZER_UNLOCKED;

/* Register, then stop. Registering first is deliberate: if we are preempted
 * between the two, an owner may reap us here rather than after the suspend -
 * which is equally safe, because vTaskDeleteWithCaps waits for us to stop
 * running and we hold nothing at this point. */
static void task_park_and_reap(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_reap_lock);
    for (int i = 0; i < REAP_MAX; i++) {
        if (!s_reap[i]) { s_reap[i] = me; me = NULL; break; }
    }
    portEXIT_CRITICAL(&s_reap_lock);
    /* Full list: fall back to the old behaviour rather than run on. A leak is
     * survivable; returning from a task function is not. */
    if (me) vTaskDelete(NULL);

    vTaskSuspend(NULL);
    for (;;) vTaskDelay(portMAX_DELAY);   /* never return, even if resumed */
}

/* Safe from any ordinary task context. Never call this from a parked task. */
static void reap_pending_tasks(void)
{
    for (;;) {
        TaskHandle_t h = NULL;
        portENTER_CRITICAL(&s_reap_lock);
        for (int i = 0; i < REAP_MAX; i++) {
            if (s_reap[i]) { h = s_reap[i]; s_reap[i] = NULL; break; }
        }
        portEXIT_CRITICAL(&s_reap_lock);
        if (!h) break;
        /* Outside the critical section - this suspends, spins and frees. */
        size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        vTaskDeleteWithCaps(h);
        /* SAID OUT LOUD on purpose. A silent leak fix is indistinguishable
         * from no fix at all - the trap #189 records for the two USB patches -
         * and the number is the whole claim being made here. */
        ESP_LOGI(TAG, "reaped a parked task: PSRAM %u -> %u KB (+%u KB)",
                 (unsigned)(before / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)((heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - before) / 1024));
    }
}

static void ft8_decode_worker_task(void *arg)
{
    // Comms context owned by OUR decode-task instance (task arg, never a
    // static) - see worker_ctx_t for the cross-instance crash this prevents.
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    ESP_LOGI(TAG, "decode worker ready (core %d)", xPortGetCoreID());
    while (true) {
        worker_job_t *job = NULL;
        if (xQueueReceive(ctx->jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        if (!job) break;   // termination sentinel
        decode_candidate_range(job->mon, job->cands, job->n_cand, job->start, job->step,
                               job->noise_db, job->slot_sec, job->t_start_us,
                               job->start_off_ms, job->result);
        xSemaphoreGive(ctx->done);
    }
    ESP_LOGI(TAG, "decode worker exiting");
    // Signal the join BEFORE self-deleting: after this the worker touches
    // nothing shared (context/job), so the decode task can safely free them
    // once it sees this.
    xSemaphoreGive(ctx->exited);
    task_park_and_reap();
}

// Decode one slot's PRE-BUILT waterfall (the STFT was streamed in during
// capture). Candidate search, then dual-core LDPC fan-out, merge, record,
// advance the QSO state machine.
static void decode_slot(worker_ctx_t *wctx, monitor_t *mon, int64_t slot_sec,
                        int slot_idx, int cap_ms, int stft_ms, int start_off_ms,
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

    // #51 instrumentation: per-slot waterfall + candidate-score statistics to
    // discriminate the alternating-slot decode collapse (level vs alignment vs
    // spectral). Byte-walk of the uint8 waterfall (~167 KB PSRAM, ~2 ms, plain
    // task context - not an ints-off walk) + a small score histogram over the
    // full candidate list. Remove once #51 is closed.
    {
        const ftx_waterfall_t *wf = &mon->wf;
        size_t n_mag = (size_t)wf->num_blocks * wf->block_stride;
        uint32_t msum = 0, n_sat = 0, n_zero = 0;
        for (size_t k = 0; k < n_mag; k++) {
            uint8_t v = wf->mag[k];
            msum += v;
            if (v >= 250) n_sat++;
            else if (v == 0) n_zero++;
        }
        int smax = 0; long ssum = 0; int n10 = 0, n15 = 0, n20 = 0;
        for (int i = 0; i < n_cand; i++) {
            int sc = cands[i].score;
            if (sc > smax) smax = sc;
            ssum += sc;
            if (sc >= 10) n10++;
            if (sc >= 15) n15++;
            if (sc >= 20) n20++;
        }
        ESP_LOGI(TAG, "slotdiag %d: noise=%.1f wf_mean=%.1f sat=%u zero=%u blocks=%d "
                      "score max=%d mean=%.1f n10=%d n15=%d n20=%d",
                 slot_idx, noise_db,
                 n_mag ? (float)msum / (float)n_mag : 0.0f,
                 (unsigned)n_sat, (unsigned)n_zero, wf->num_blocks,
                 smax, n_cand ? (float)ssum / (float)n_cand : 0.0f, n10, n15, n20);
    }

    // Fan out across both cores: the helper (core 0) takes odd candidates, we
    // take even. Both share the same const waterfall (read-only) and the
    // mutex-protected decode list. r_worker is static (one helper, one slot at a
    // time); r_main is a stack local. We block on the done semaphore before reading
    // r_worker, so neither result struct is ever touched concurrently.
    static decode_result_t r_worker;
    decode_result_t r_main;

    worker_job_t job = {
        .mon = mon, .cands = cands, .n_cand = n_cand, .noise_db = noise_db,
        .slot_sec = slot_sec, .t_start_us = t_start, .start_off_ms = start_off_ms,
        .start = 1, .step = 2, .result = &r_worker,
    };
    bool dispatched = false;
    if (wctx && n_cand > 1) {
        worker_job_t *jp = &job;   // job stays valid: we block on wctx->done below
        if (xQueueSend(wctx->jobs, &jp, 0) == pdTRUE) dispatched = true;
    }

    // Our half (even indices).
    decode_candidate_range(mon, cands, n_cand, 0, 2, noise_db, slot_sec,
                           t_start, start_off_ms, &r_main);

    if (dispatched) {
        xSemaphoreTake(wctx->done, portMAX_DELAY);
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
        int applied_ms      = 0;   // real correction pushed to the clock this slot

        // Auto-sync: nudge the system clock to the on-air population average
        // every slot, not just when the operator opens the time-sync modal
        // and taps Apply. See FT8_AUTOSYNC_MIN_SAMPLES/_MS above for why both
        // guards exist. Previously FT8-only - the timing offset was
        // protocol-scaled wrong for FT4 (see the mon->block_size fix above);
        // now that it's computed from the real per-protocol block sizes, FT4
        // is included too.
        if (n_timing >= FT8_AUTOSYNC_MIN_SAMPLES &&
            abs(s_last_timing_ms) >= FT8_AUTOSYNC_MIN_MS) {
            // Damped fraction of the raw measurement (FT8_AUTOSYNC_GAIN) - the
            // running mean that tracks the on-air consensus. NO per-slot rate
            // cap: the loop follows the band freely. The safety is the POSITION
            // leash inside time_sync (FT8_LEASH_MS - bounded cumulative pull
            // from the SNTP/GPS anchor), not an arbitrary ms/slot limit.
            // _quiet skips the QMX CAT push (blocking CDC write, unsafe from
            // this hot path) and RETURNS the delta actually applied after
            // leashing - that (not the requested damped_ms) is the real nudge.
            int damped_ms = (int)roundf(s_last_timing_ms * FT8_AUTOSYNC_GAIN);
            if (damped_ms != 0) {
                applied_ms = time_sync_apply_correction_ms_quiet(damped_ms);
            }
        }
        s_last_applied_ms = applied_ms;   // real applied nudge (post-leash) - shown by the modal
        s_timing_seq++;                   // bump after BOTH raw + applied are set
    }

    int dec_ms = (int)((esp_timer_get_time() - t_start) / 1000);

    size_t heap_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    size_t heap_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024;
    // Low-water mark + largest contiguous block: the SDIO TX path needs an
    // internal DMA buffer, so what matters for the transport_drv crash is the
    // worst-case internal free and whether a single contiguous chunk survives.
    //
    // ⛔ BUT largest_free_block WALKS THE HEAP WITH INTERRUPTS OFF, and this is
    // a PERIODIC path - once per slot, so every 15 s in FT8 and every 7.5 s in
    // FT4. That is exactly what the 2026-07-13 investigation removed from
    // cpu_stats.c and audio.c to stop the full-screen cyan flash: the ints-off
    // window delays the core-0 MIPI-DSI frame-restart ISR past the vertical
    // blanking window and the panel blanks for a frame. audio.c's heap watchdog
    // even carries the rule in a comment - "must NEVER go on this periodic path
    // (that caused the FT4 cyan flash)" - while this call sat two files away
    // doing it unconditionally, at TWICE the rate in FT4. It was simply missed
    // when the others were fixed.
    //
    // Observed by the operator on 2026-08-28 during an FT4 simulation run, with
    // internal free at 17-18 KB: below audio.c's 24 KB emergency threshold, so
    // BOTH walkers were running - its 10 s one and this 7.5 s one.
    //
    // Same treatment as audio.c: the walk is fragmentation forensics, worth a
    // one-frame blink only when the heap is nearly exhausted. Above the
    // threshold report the O(1) counters and print lblk as 0, which the log
    // format already distinguishes by the LOW! marker on the emergency line.
    size_t heap_i_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;
    size_t heap_i_lblk = 0;
    if (heap_i < FT8_SLOT_LBLK_EMERGENCY_KB)
        heap_i_lblk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024;

    ft8_status_set("RX: %d decoded", n_decoded);
    ESP_LOGI(TAG,
        "slot %d UTC %lld: off=%+dms cap=%dms stft=%dms dec=%dms cand=%d dec=%d skip=%d "
        "backlog=%dpr drop=%dpr timing=%+dms applied=%+dms heap_i=%uKB(min=%uKB,lblk=%uKB) heap_p=%uKB",
        slot_idx, (long long)slot_sec, start_off_ms,
        cap_ms, stft_ms, dec_ms, n_cand, n_decoded, n_skipped,
        arm_backlog, drop_delta,
        s_last_timing_valid ? s_last_timing_ms : 0,
        s_last_timing_valid ? s_last_applied_ms : 0,
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
        if (tx_or_qso || cat_user_pause_active()) {
            // Sparse RX is expected during an exchange, and GUARANTEED while
            // the operator has paused CAT to use the radio's own menu - the
            // radio stops streaming, so every slot decodes zero by design.
            s_stuck_slots = 0;   // never reset mid-exchange or mid-pause
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
    // Fox/Hound automatic mode goes BEFORE the robot: both self-gate to IDLE, so
    // whichever runs first wins the slot, and a Fox is the rarer opportunity as
    // well as the one the operator opted into explicitly. The robot separately
    // refuses to pounce a Fox while Hound is enabled - an ordinary pounce at one
    // cannot complete, since it never QSYs.
    ft8_hound_tick(slot_sec);
    ft8_robot_tick(slot_sec);
    ft8_screen_view_request_refresh();
}

// ---------------------------------------------------------------------------
// Decode task
// ---------------------------------------------------------------------------

static void ft8_decode_task(void *arg)
{
    TaskHandle_t notify_target = (TaskHandle_t)arg;
    // Already claimed by the spawner before xTaskCreate - see there. Kept
    // (idempotent) so the flag is right whatever route started this task.
    s_decode_task_alive = true;

    // Dual-core decode helper: a 1-deep job queue + completion semaphore + a
    // worker pinned to core 0. In FT8 mode core 0 runs only the (light) UAC
    // producer / audio task (pri 5/3) and the FT8 list LVGL render, so a pri-2
    // worker uses its spare cycles without ever starving audio. If the spawn
    // fails we drop the queue and decode_slot falls back to single-core.
    TaskHandle_t worker = NULL;
    worker_ctx_t *wctx = calloc(1, sizeof(worker_ctx_t));
    if (wctx) {
        wctx->jobs   = xQueueCreate(1, sizeof(worker_job_t *));
        wctx->done   = xSemaphoreCreateBinary();
        wctx->exited = xSemaphoreCreateBinary();
        if (wctx->jobs && wctx->done && wctx->exited) {
            BaseType_t wrc = xTaskCreatePinnedToCoreWithCaps(
                ft8_decode_worker_task, "ft8_dec0", 32768, wctx,
                tskIDLE_PRIORITY + 2, &worker, 0,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (wrc != pdPASS) {
                ESP_LOGW(TAG, "core-0 decode worker spawn failed (rc=%d); single-core decode", (int)wrc);
                worker = NULL;
            }
        }
        if (!worker) {
            if (wctx->jobs)   vQueueDelete(wctx->jobs);
            if (wctx->done)   vSemaphoreDelete(wctx->done);
            if (wctx->exited) vSemaphoreDelete(wctx->exited);
            free(wctx);
            wctx = NULL;
        }
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
        decode_slot(wctx, s_mon_pool[job.mon_idx], job.slot_sec, job.slot_idx,
                    job.cap_ms, job.stft_ms, job.start_off_ms,
                    job.arm_backlog, job.drop_delta);
        s_buf_busy[job.mon_idx] = false;   // hand the monitor back to the pool
        // Open the hold-for-decode gate ONLY here, after decode_slot() has
        // fully returned - by then ft8_qso_advance() has run and (on progress)
        // replaced the ARMED request with the fresh message.
        s_decode_jobs_done++;
    }

    // Stop the core-0 worker (blocked on its queue): send a NULL sentinel, then
    // JOIN — wait for the worker to actually reach its park (it gives
    // ctx->exited first) before deleting the queue/semaphore and returning.
    // The old code waited a FIXED 50 ms, which was not enough if the worker was
    // still holding a job pointer into this task's stack: freeing the stack out
    // from under it caused a "Load address misaligned" panic on FT8 exit. The
    // worker is idle here (decode_slot always joins on the done semaphore before
    // returning), so this normally returns in well under a millisecond; the
    // generous bound only guards a pathological in-flight decode.
    if (wctx) {
        worker_job_t *sentinel = NULL;
        xQueueSend(wctx->jobs, &sentinel, pdMS_TO_TICKS(1000));
        if (xSemaphoreTake(wctx->exited, pdMS_TO_TICKS(12000)) == pdTRUE) {
            /* The worker has finished and parked, so free its PSRAM stack
             * before the context it was reading from (#279). On a TIMEOUT we
             * deliberately do NOT reap - the same reasoning as the ctx leak
             * below, and vTaskDeleteWithCaps on a task that is still working
             * would be the use-after-free we are avoiding. */
            reap_pending_tasks();
            vQueueDelete(wctx->jobs);
            vSemaphoreDelete(wctx->done);
            vSemaphoreDelete(wctx->exited);
            free(wctx);
        } else {
            // The worker may still be alive holding this context - deleting
            // it now is the use-after-free/assert path (Dennis WN4FLA's
            // crash class). Leak it deliberately: ~few hundred bytes, only
            // on a pathological join timeout, and a leak beats a reboot.
            ESP_LOGW(TAG, "worker join timed out - LEAKING worker ctx (crash-safe)");
        }
        wctx = NULL;
    }

    ESP_LOGI(TAG, "decode task exiting");
    s_decode_task_alive = false;
    if (notify_target) xTaskNotify(notify_target, 1, eSetBits);
    task_park_and_reap();
}

// ---------------------------------------------------------------------------
// Capture task
// ---------------------------------------------------------------------------

static void ft8_task(void *arg)
{
    (void)arg;
    // Already set by ft8_self_test() BEFORE xTaskCreate - see the comment there.
    // Kept (idempotent) so the flag is still correct if this task is ever
    // started by some other route.
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
            /* Every other exit clears this; this one did not, so a boot with no
             * time source left the flag true for ever with no task behind it -
             * FT8 dead for the session AND the respawn watchdog refusing to
             * retry, which is precisely what the spawner's comment warns of. */
            s_ft8_task_alive = false;
            task_park_and_reap();
            return;
        }
    }

    // Capture scratch (decimated 12 kHz audio, reused every slot) + monitor
    // pool. The scratch is sized for the larger FT8 slot (180000) and shared by
    // FT4 (90000, fits inside). The pool is built for the CURRENT sub-mode; the
    // slot loop rebuilds it on an FT8<->FT4 toggle (reinit_pool_if_mode_changed).
    // s_mon_pool/s_cap_scratch are zeroed statics, so a partial-alloc frees clean.
    /* ⛔ RETRIED, BECAUSE THE PAGE WE JUST LEFT MAY STILL HOLD THE MEMORY.
     *
     * Switching WSPR -> FT8 asks for this scratch while wspr_rx is still
     * holding 11.25 MB: wspr_rx_stop() only sets a flag, and its loop frees on
     * its next pass - about 2.6 s later, measured. FT8 asked 147 ms after the
     * mode changed, failed, and deleted itself.
     *
     * ⛔ AND THE RECOVERY WAS WORSE THAN THE FAULT. The respawn watchdog then
     * retried once a SECOND for ever, and by then FT8's own entry had taken
     * ~34 KB of internal heap it never gave back - so every retry failed on
     * the TCB (`failed to spawn ft8_task (rc=0)`) with internal free at 4 KB
     * and a 0 KB largest block. The device was left degraded, which on this
     * board is the state that also breaks TLS, USB endpoint allocation and SD.
     *
     * The mirror of this exists in wspr_rx_task and was fixed a day earlier;
     * fixing one direction and not asking about the other is what let this
     * through. The memory genuinely arrives - wait for it. */
    for (int attempt = 0; attempt < FT8_ALLOC_TRIES && !s_cap_scratch; attempt++) {
        s_cap_scratch = heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
        if (s_cap_scratch) break;
        if (attempt == 0)
            ESP_LOGW(TAG, "capture scratch not available yet (%u KB PSRAM free) - "
                          "waiting for the previous page to release",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        if (ui_mode_get() != UI_MODE_FT8) break;   /* left again while waiting */
        vTaskDelay(pdMS_TO_TICKS(FT8_ALLOC_WAIT_MS));
    }
    if (!s_cap_scratch) {
        ESP_LOGE(TAG, "PSRAM alloc for capture scratch failed");
        s_ft8_task_alive = false;
        task_park_and_reap();
        return;
    }
    if (!build_monitor_pool(proto_for_mode())) {
        ESP_LOGE(TAG, "initial monitor pool build failed");
        free_capture_pool();
        s_ft8_task_alive = false;
        task_park_and_reap();
        return;
    }

    s_decode_queue = xQueueCreate(DECODE_QUEUE_DEPTH, sizeof(decode_job_t));
    if (!s_decode_queue) {
        ESP_LOGE(TAG, "failed to create decode queue");
        free_capture_pool();
        s_ft8_task_alive = false;
        task_park_and_reap();
        return;
    }

    // Spawn decode task, passing our handle so it can notify us on exit.
    s_ft8_running = true;
    // Same defect as #199, second instance: this flag used to be set as
    // ft8_decode_task's first statement, so it meant "the decode task has begun
    // RUNNING" while ft8_self_test()'s guard needs "a decode task EXISTS". Both
    // tasks are tskIDLE_PRIORITY + 1 - the lowest on the board - so the gap
    // between creating one and its first slice is routinely long.
    //
    // The dangerous ordering is real: with the flag still false, ft8_task could
    // exit and clear s_ft8_task_alive, the 1 Hz respawn watchdog would spawn a
    // fresh ft8_task, BOTH guards would pass, and the new instance would run
    // alongside an orphaned decode task - which is precisely the overlap the
    // guard exists to refuse (Dennis WN4FLA's crash). Claim it before creating.
    s_decode_task_alive = true;
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_decode_task, "ft8_dec", 65536,
        xTaskGetCurrentTaskHandle(),
        tskIDLE_PRIORITY + 1, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn ft8_decode_task (rc=%d)", (int)rc);
        s_decode_task_alive = false;   // release, or FT8 never starts again
        s_ft8_running = false;
        vQueueDelete(s_decode_queue);
        s_decode_queue = NULL;
        free_capture_pool();
        s_ft8_task_alive = false;
        task_park_and_reap();
        return;
    }

    ESP_LOGI(TAG, "entering continuous slot loop (pooled, time-budgeted decode)");

    // Callsign hash table: create (first FT8 entry only) and seed our own
    // call. Nonstandard-call messages (i3=4) carry OUR call as a 12-bit hash
    // - without our own call in the table, a special-call station's answer
    // decodes as "<...> THEIRCALL" and is never recognised as addressed to us.
    ft8_hash_init();
    {
        qmx_settings_t id_cfg;
        settings_load_all(&id_cfg);
        if (id_cfg.my_callsign[0]) ft8_hash_seed(id_cfg.my_callsign);
    }

    // Fresh gate state for this ft8_task instance (the counters are static and
    // a previous instance may have torn down mid-flight).
    s_decode_jobs_queued = 0;
    s_decode_jobs_done   = 0;

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
        // FT4 TX is implemented in ft8_tx.c and DOES key a connected radio.
        // ⚠ This comment used to say FT4 was "force-routed through the
        // simulation interlock ... never let an FT4 burst reach a connected
        // QMX". That interlock does not exist: ft8_tx_run()'s `sim` is
        // settings' sim_mode_en and nothing else, and CLAUDE.md records real
        // FT4 QSOs completed on air. The claim was left behind when FT4 TX was
        // enabled. Corrected 2026-08-09 - do not rely on a protocol-based TX
        // safety boundary here or in ft8_tx.c, because there isn't one.

        ESP_LOGI(TAG, "slot %d: waiting for next %s boundary...", slot_idx, is_ft4 ? "FT4" : "FT8");
        int64_t boundary_ms = wait_for_slot_boundary_ms(last_boundary_ms, period_ms);
        last_boundary_ms = boundary_ms;
        int64_t slot_sec = boundary_ms / 1000;   // whole-second slot id (record/aging)

        ft8_tx_request_t txreq;
        // Hold-for-decode gate (see FT8_TX_HOLD_DEADLINE_MS): a TX is due this
        // slot, but the previous RX slot's decode is still in flight and the
        // QSO machine is mid-exchange - the decode's result (the partner's
        // reply) will replace the armed message ~1.7 s from now. Don't fire
        // the stale one at the boundary; fall into the RX branch below and let
        // the reply-on-immediate-slot poll fire the FRESH message once
        // ft8_qso_advance() has run (or the deadline passes). FT8-only, same
        // as the late-TX path itself.
        /* ⭐ FT4 TOO, and the exclusion was over-cautious (Gyula HA3HZ).
         *
         * His report is precisely what this gate exists to prevent: "with FT4,
         * when the decoded response appeared, my message had already been
         * transmitted, so it was sent out repeatedly" - and the partner gives up
         * and answers someone else. The fix was written for exactly that in
         * v1.0.0 and then switched off for the mode he uses, on the assumption
         * that a 7.5 s slot has no room to wait.
         *
         * The arithmetic says otherwise:
         *     FT8  15.0 s slot - 12.64 s burst (79 x 160 ms) = 2.36 s of room
         *     FT4   7.5 s slot -  5.04 s burst (105 x 48 ms) = 2.46 s of room
         * FT4 has MORE room, not less, because its burst shrinks faster than its
         * slot does. The existing 2300 ms deadline is that room minus a margin,
         * and it is comfortably safe for both.
         *
         * Safe by construction either way: if the decode has not landed by the
         * deadline the armed message fires anyway, so the worst case is exactly
         * today's behaviour - a repeat - and never a skipped slot.
         *
         * ⚠ HONEST LIMIT: an FT4 decode measures ~2.3 s, which is right at the
         * deadline, so this will help on the faster slots and do nothing on the
         * slower ones. It is not a promise that the repeat disappears. */
        /* ⛔ THE HOLD AND THE FIRE MUST AGREE ON WHICH PROTOCOLS THEY COVER.
         * v1.10.2 shipped with the hold enabled for FT4 (the #295 fix) while the
         * late-fire path below still carried its own `!is_ft4`. So an FT4 burst
         * was held at the boundary and then NEVER fired: FT4 stopped
         * transmitting altogether, on CQ and on a reply alike, with the slot
         * countdown running normally - reported within hours by Gyula HA3HZ.
         *
         * `ft8_qso_is_busy()` is true throughout a CQ run, which is why calling
         * CQ was hit and not just replies.
         *
         * One flag now drives BOTH, so the gate cannot be half-removed again.
         * Holding a burst that nothing is allowed to fire is the whole bug. */
        /* Both gates come from ft8_slot_gate.c, which is portable and
         * host-tested (test/ft8_slot_gate_harness.c, 9/9 mutations caught).
         * They share ONE ft8_gate_late_fire_enabled(), so the pair can no
         * longer be half-removed - which is exactly what broke FT4 transmit in
         * v1.10.2 and what the harness's first test now forbids.
         *
         * The three inputs are evaluated in the same short-circuit ORDER as
         * before: the two accessors are side-effect-free peeks, but reading
         * them only when needed keeps this off the hot path. */
        ft8_gate_boundary_t bgate = {
            .is_ft4           = is_ft4,
            .decode_in_flight = (s_decode_jobs_done != s_decode_jobs_queued),
            .qso_busy         = false,
            .tx_would_run     = false,
        };
        if (bgate.decode_in_flight) {
            bgate.qso_busy = ft8_qso_is_busy(NULL, 0);
            if (bgate.qso_busy) bgate.tx_would_run = ft8_tx_slot_would_run(boundary_ms);
        }
        bool hold_for_fresh = ft8_gate_should_hold(&bgate);
        if (hold_for_fresh)
            ESP_LOGI(TAG, "slot %d: TX due but previous slot still decoding - holding for fresh reply",
                     slot_idx);
        // Pass boundary_ms (exact, undistorted) not slot_sec - see the
        // ft8_tx_should_run_this_slot doc comment for why the whole-second
        // truncation breaks FT4 parity.
        // Set when this slot was spent transmitting. An ABORTED burst hands the
        // rest of its slot back, and we fall through to the normal RX capture
        // below rather than sitting idle until the next boundary (#136).
        bool slot_used_by_tx = false;
        if (!hold_for_fresh && ft8_tx_should_run_this_slot(boundary_ms, &txreq)) {
            ft8_status_set("TX: %s", txreq.display_text);
            note_tx_slot(slot_sec, (int)(boundary_ms / period_ms));
            ft8_tx_run(&txreq);   // blocks ~12.7 s; always restores RX before returning
            ft8_qso_on_tx_complete();  // re-arm the current outgoing message
            ft8_screen_view_request_refresh();
            slot_used_by_tx = true;

            // Cancelled early? Listen to what is left. Roy KI0ER's point: what
            // we hear in the remainder is perfectly valid data for the tone
            // occupancy map, even though it is only part of a slot - and the
            // alternative is a window we transmitted into for half a second and
            // then learned nothing from.
            //
            // No second capture path is needed: the FT8 pre-ring runs
            // CONTINUOUSLY in FT8 mode (dsp.c fills it every window regardless
            // of what this task is doing), so the ordinary RX branch below,
            // which already backfills from the boundary, reconstructs the slot -
            // including the fraction we were keyed for. That is the same
            // machinery the late-arm case uses; it is not a new one.
            //
            // Only worth it if enough of the slot remains for a signal to still
            // decode: FT8's payload is ~12.6 s of a 15 s slot, so a late abort
            // leaves nothing but the tail. The threshold is deliberately
            // generous rather than clever - a wasted capture costs one monitor
            // from the pool and nothing else.
            int ab_ms = ft8_tx_last_abort_ms();
            if (ab_ms >= 0 && ab_ms <= (period_ms / 3)) {
                slot_used_by_tx = false;
                ESP_LOGI(TAG, "slot %d: TX aborted %d ms in - listening to the rest of the slot",
                         slot_idx, ab_ms);
                ft8_status_set("TX cancelled - listening");
            } else {
                ft8_status_set("TX done - waiting for next slot");
            }
        }
        if (!slot_used_by_tx) {
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

            // Early-decode cut point (see FT8_DECODE_RESERVE_MS). Mid-QSO, stop
            // capturing ~2 s before the boundary so the decode runs and arms the
            // fresh reply in time to fire at dt~=0. The capture buffer stays
            // full-size (begin() got slot_samples); finish() zero-pads the tail
            // we skipped, so the decoder still sees a normal 93-block waterfall.
            // FT8-only; full-slot when just monitoring, for max band yield.
            int cap_target = slot_samples;
            // Early-cut whenever a QSO is running OR a reply is merely ARMED
            // (a hand-tapped Transmit/pounce that hasn't fired yet): both mean
            // the partner's next message must decode BEFORE the boundary so the
            // fresh reply can arm and fire at DT~0. The armed case is what lets
            // a manual exchange land on-beat instead of a cycle late.
            //
            // With ft8_early_decode ON (default) the cut ALSO runs during plain
            // monitoring, so EVERY decode surfaces before the boundary the way
            // WSJT-X does - this is what makes a COLD pounce (first reply to a
            // fresh CQ) able to fire in its own slot instead of a cycle later.
            // The trade-off is weak/late-station yield: capture stops at
            // period-RESERVE (~13.2 s), so a station transmitting late enough
            // that its tail lands past that point (our ~560 ms RX latency eats
            // into the margin) can be clipped and miss decoding. Operator-
            // toggleable so it can be turned off if a given band's yield suffers.
            qmx_settings_t cut_cfg;
            settings_load_all(&cut_cfg);
            bool want_early_cut = ft8_qso_is_busy(NULL, 0) ||
                                  (ft8_tx_get_status(NULL, 0, NULL) == FT8_TX_ARMED) ||
                                  cut_cfg.ft8_early_decode;
            if (!is_ft4 && want_early_cut) {
                int cut = (period_ms - FT8_DECODE_RESERVE_MS) * (SR_HZ / 1000);
                if (cut > slot_samples) cut = slot_samples;   // reserve too small
                if (cut < slot_samples) cap_target = cut;     // else: no early cut
            }

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
            // Backfill the boundary->arm gap from the continuous pre-ring: the
            // slot's opening audio (incl. the Costas sync array) that arrived
            // while this low-priority task was still getting scheduled. Anchors
            // the capture window to the UTC boundary, not to wake time — the fix
            // for the "great decodes only on the first slot" collapse.
            uint32_t backfill = (start_off_ms > 0)
                              ? (uint32_t)start_off_ms * (SR_HZ / 1000) : 0;
            esp_err_t e = dsp_ft8_capture_begin(s_cap_scratch, slot_samples, backfill);
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
                    if (avail >= cap_target) break;             // slot in (or early-decode cut)
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
                    //
                    // hold_for_fresh (set at the boundary): the ARMED request was
                    // deliberately NOT fired at the boundary because the previous
                    // slot's decode could supersede it. Keep holding until that
                    // decode has fully landed (jobs_done catches up - by then
                    // ft8_qso_advance() has replaced the armed content) or the
                    // deadline passes (fire whatever is armed = old behaviour).
                    bool decode_landed = (s_decode_jobs_done == s_decode_jobs_queued);
                    /* tx_should_run is set true here and the REAL check is
                     * left as the last term below, deliberately: unlike the
                     * peeks above, ft8_tx_should_run_this_slot() fills
                     * late_txreq and is the commit path, so it must stay behind
                     * the short circuit and must not be called speculatively. */
                    ft8_gate_late_t lgate = {
                        .is_ft4        = is_ft4,
                        .held          = hold_for_fresh,
                        .decode_landed = decode_landed,
                        .tx_should_run = true,
                        .into_slot_ms  = into_slot_ms,
                    };
                    if (ft8_gate_should_late_fire(&lgate) &&
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
                note_tx_slot(slot_sec, (int)(boundary_ms / period_ms));
                ft8_tx_run(&late_txreq);          // blocks ~12.7 s; always restores RX
                ft8_qso_on_tx_complete();
                ft8_status_set("TX done - waiting for next slot");
                ft8_screen_view_request_refresh();
            } else if (e == ESP_OK) {
                // We genuinely LISTENED to this slot, so the occupancy picture
                // for its parity is current as of now. This is the only honest
                // source for "do we know what is in that window": a station
                // count cannot tell an EMPTY window from one we transmitted
                // into, and both otherwise render as "free" (#135).
                {
                    int64_t sidx = boundary_ms / period_ms;
                    if ((sidx % 2) == 0) s_last_rx_utc_even = slot_sec;
                    else                 s_last_rx_utc_odd  = slot_sec;
                }
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
                } else {
                    s_decode_jobs_queued++;   // hold-for-decode gate bookkeeping
                }
            } else {
                s_buf_busy[bi] = false;   // capture failed: release the monitor
                qmx_settings_t sim_chk;
                settings_load_all(&sim_chk);
                if (sim_chk.sim_mode_en) {
                    // Sim mode with no QMX audio: capture timing out is the
                    // NORMAL case, not an error. The phantom stations inject
                    // decodes directly (ft8_sim.c -> ft8_screen_record_decode),
                    // bypassing audio entirely - but ft8_qso_advance() normally
                    // only runs from the decode task after a successful capture,
                    // so without this the injected replies sat unread and the
                    // QSO machine re-sent the same message forever. Run the
                    // same end-of-slot bookkeeping decode_slot() would have.
                    // Safe here: with no audio no decode job was queued for
                    // this slot, so no decode-task advance() can race this one.
                    ft8_status_set("SIM RX slot (no audio)");
                    // Honor the Fast-pounce toggle's timing in sim: with the
                    // early-decode cut OFF a real decode pass lands ~1.5-3.5 s
                    // AFTER the boundary, and ft8_sim delays its phantom-reply
                    // injection to match - so wait the same before consuming,
                    // or the advance would scan before the reply exists. With
                    // it ON both the injection and this advance run before /
                    // at the boundary (the whole point of the toggle).
                    if (!sim_chk.ft8_early_decode) vTaskDelay(pdMS_TO_TICKS(3500));
                    ESP_LOGI(TAG, "slot %d UTC %lld: no audio (sim) - running QSO advance%s",
                             slot_idx, (long long)slot_sec,
                             sim_chk.ft8_early_decode ? "" : " (late, early-decode OFF)");
                    ft8_qso_advance(slot_sec);
                    // Robot auto-answer must tick here too - it normally runs
                    // from decode_slot() after a successful capture, which a
                    // no-audio sim slot never reaches, so "Auto-answer CQ"
                    // silently did nothing in QMX-less simulation. Same
                    // ordering as decode_slot(): advance first, robot second.
                    //
                    // ⚠ ANY per-slot tick added to decode_slot() needs adding
                    // HERE as well, or it does nothing in simulation - which is
                    // the one place the feature can be tested with the radio off.
                    // Fox/Hound learned this the hard way on 2026-08-10: the
                    // phantom Fox was decoded perfectly and ft8_hound_tick() was
                    // never called, so it looked like detection was broken.
                    ft8_hound_tick(slot_sec);
                    ft8_robot_tick(slot_sec);
                    ft8_screen_view_request_refresh();
                } else {
                    ft8_status_set("RX: capture error");
                    ESP_LOGW(TAG, "slot %d UTC %lld: capture failed (%d)",
                             slot_idx, (long long)slot_sec, e);
                }
            }
        }

        slot_idx++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Signal decode task to stop, then wait for it to drain and exit.
    s_ft8_running = false;
    decode_job_t sentinel = { .mon_idx = -1, .slot_sec = -1LL };
    xQueueSend(s_decode_queue, &sentinel, pdMS_TO_TICKS(1000));
    // Clear any stale notification, then wait for decode task exit (which
    // also tears down its core-0 worker before notifying us). 15 s: must
    // OUTLAST the decode task's own 12 s worker join, or ft8_task declares
    // itself dead while the decode task still runs - the overlap window
    // behind Dennis WN4FLA's crash (the 10 s it used to be was shorter than
    // the join bound it was waiting on).
    bool joined = (xTaskNotifyWait(0x01, 0x01, NULL, pdMS_TO_TICKS(15000)) == pdTRUE);

    /* The decode task has notified us and parked; free its 64 KB stack (#279).
     * If the wait TIMED OUT it has not parked and is not on the list, so this
     * reaps nothing - which is the safe outcome, not a missed one. */
    reap_pending_tasks();

    if (joined) {
        vQueueDelete(s_decode_queue);
        s_decode_queue = NULL;
        free_capture_pool();
    } else {
        /* ⛔ THE TIMEOUT PATH MUST FREE NOTHING. Hardware-captured 2026-09-01:
         * this branch logged `ft8_task exiting` at 457.114 s and the still-live
         * core-0 worker took a Load access fault at 477.268 s in
         * estimate_snr_db, reading a waterfall this function had freed.
         *
         * BOTH objects are unsafe here, not just the pool. The decode task
         * blocks on a LOCAL COPY of the queue handle (`QueueHandle_t q =
         * s_decode_queue`), so vQueueDelete() under it is the same
         * use-after-free one layer down; its NULL check only guards the NEXT
         * iteration. Clearing the global is what makes that iteration exit.
         *
         * So: clear the references, free neither, and say so loudly. The
         * decode task finishes on its own, joins its worker and parks; what it
         * was reading stays valid the whole time. */
        s_decode_queue = NULL;
        forget_capture_pool();
        ESP_LOGW(TAG, "decode task did not exit in 15 s - LEAKING its queue and "
                      "the capture pool rather than freeing memory a live worker "
                      "may still be reading (crash-safe; see forget_capture_pool)");
    }

    ESP_LOGI(TAG, "ft8_task exiting; processed %d slots", slot_idx);
    s_ft8_task_alive = false;
    task_park_and_reap();
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
#define FT4_GFSK_BT        1.0f          // FT4's is narrower (ft8_lib's own demo)
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
        task_park_and_reap();
        return;
    }

    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    int n_spsym = (int)(0.5f + SR_HZ * FT8_SYMBOL_PERIOD);  // 1920
    int n_wave  = FT8_NN * n_spsym;                          // 151680 (~12.64 s)

    float *signal = heap_caps_malloc(SLOT_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!signal) {
        ESP_LOGE(TAG, "FD e2e selftest: FAIL (signal alloc)");
        task_park_and_reap();
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
        task_park_and_reap();
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
        task_park_and_reap();
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
    task_park_and_reap();
}

bool ft8_synth_and_decode(const ftx_message_t *msg, float tone_hz,
                          char *out_text, size_t out_len,
                          int *out_snr_db, int *out_score)
{
    // The original, noiseless behaviour. Every boot self-test comes through
    // here and must keep reading what it always read.
    return ft8_synth_and_decode_at(msg, tone_hz, FT8_SIM_SNR_CLEAN,
                                   out_text, out_len, out_snr_db, out_score);
}

bool ft8_synth_and_decode_at(const ftx_message_t *msg, float tone_hz,
                             int want_snr_db,
                             char *out_text, size_t out_len,
                             int *out_snr_db, int *out_score)
{
    if (!msg || !out_text || !out_len) return false;

    // Whichever protocol is running. The simulator used to be FT8-only and said
    // so - "no concept of the FT8/FT4 sub-mode" - which is why sim mode switched
    // itself off in FT4 and no FT4 change could be exercised without a partner
    // on the air (#256). FT4 is 105 symbols of 48 ms with narrower shaping;
    // everything else about this function is the same.
    bool  ft4       = (ft8_op_mode_get() == FT8_OP_MODE_FT4);
    int   n_sym     = ft4 ? FT4_NN : FT8_NN;
    float sym_per   = ft4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    float gfsk_bt   = ft4 ? FT4_GFSK_BT : FD_E2E_GFSK_BT;
    int   slot_samp = ft4 ? (int)(SR_HZ * 7.5f) : SLOT_SAMPLES;

    uint8_t tones[FT4_NN];              // FT4_NN (105) > FT8_NN (79)
    if (ft4) ft4_encode(msg->payload, tones);
    else     ft8_encode(msg->payload, tones);

    int n_spsym = (int)(0.5f + SR_HZ * sym_per);
    int n_wave  = n_sym * n_spsym;

    float *signal = heap_caps_malloc((size_t)slot_samp * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!signal) return false;
    memset(signal, 0, (size_t)slot_samp * sizeof(float));

    // A NOISE FLOOR TO MEASURE AGAINST (#265). Without it the buffer is silent
    // and the estimator has nothing but the signal's own leakage to compare to,
    // which scales with the signal - so every phantom came out at the same
    // +9/+10 dB however loud it was meant to be.
    //
    // Uniform noise is enough here: the estimator averages power in bins, and
    // what matters is that the floor is flat and independent of the signal.
    // xorshift32 rather than rand(), to keep this off the C library's global
    // state - this runs on the FT8 tasks.
    float amp = 1.0f;
    if (want_snr_db != FT8_SIM_SNR_CLEAN) {
        const float sigma = 0.10f;                  // noise RMS
        uint32_t x = 0x1234567u ^ (uint32_t)(tone_hz * 7.0f) ^ (uint32_t)esp_timer_get_time();
        if (!x) x = 1;
        const float scale = sigma * 1.7320508f;     // uniform[-s,s] has RMS s/sqrt(3)
        for (int i = 0; i < slot_samp; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            signal[i] = scale * ((float)(int32_t)x / 2147483648.0f);
        }
        // Amplitude for the wanted ratio, in FT8's 2500 Hz reference bandwidth:
        //   SNR = (a^2 / 2) / (sigma^2 * 2500 / (SR/2))
        float nb = sigma * sigma * (2500.0f / (SR_HZ / 2.0f));
        amp = sqrtf(2.0f * nb * powf(10.0f, (float)want_snr_db / 10.0f));
    }

    int start = (int)(0.5f * SR_HZ);
    if (start + n_wave > slot_samp) start = 0;
    float *tone_buf = signal + start;
    if (want_snr_db != FT8_SIM_SNR_CLEAN) {
        // Synthesise into scratch, then ADD it to the noise at the chosen level.
        tone_buf = heap_caps_malloc((size_t)n_wave * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!tone_buf) { heap_caps_free(signal); return false; }
        memset(tone_buf, 0, (size_t)n_wave * sizeof(float));
    }
    if (!synth_gfsk_heap(tones, n_sym, tone_hz, gfsk_bt, sym_per, SR_HZ, tone_buf)) {
        if (tone_buf != signal + start) heap_caps_free(tone_buf);
        heap_caps_free(signal);
        return false;
    }
    if (tone_buf != signal + start) {
        for (int i = 0; i < n_wave; i++) signal[start + i] += amp * tone_buf[i];
        heap_caps_free(tone_buf);
    }

    const monitor_config_t cfg = {
        .f_min = 200.0f, .f_max = 3000.0f, .sample_rate = SR_HZ,
        .time_osr = 2, .freq_osr = 2,
        .protocol = ft4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8,
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
        if (ftx_message_decode(&dec_msg, ft8_hash_if(), out_text, &off) != FTX_MESSAGE_RC_OK) continue;

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
        task_park_and_reap();
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
    task_park_and_reap();
}

void ft8_sim_synth_selftest(void)
{
    reap_pending_tasks();   /* the previous one-shot, if it has finished (#279) */
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
    reap_pending_tasks();   /* the previous one-shot, if it has finished (#279) */
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
        /* INFO: this is the ORDINARY case on an FT8 re-entry, not a fault.
         * Teardown takes ~10 s (measured), so any toggle quicker than that
         * lands here, and the 1 Hz watchdog in ft8_screen_view.c starts the
         * new session about a second later. Refusing IS the correct answer -
         * see the double-spawn history below (#199). It was a WARN, which
         * made every normal toggle look like a defect (#280). */
        ESP_LOGI(TAG, "ft8_self_test: previous FT8 session is still tearing "
                      "down; the watchdog will start the new one");
        return;
    }
    // The decode task can outlive ft8_task (ft8_task's teardown wait is
    // bounded, and the decode task's own worker join can outlast it). A new
    // session starting in that window is what crossed the old shared worker
    // handles (Dennis WN4FLA's crash) - now the handles are per-instance,
    // but still refuse the overlap: the old instance also still owns monitor
    // pool buffers and the decode list mutex path.
    if (s_decode_task_alive) {
        /* Same: routine, and the watchdog retries. See above (#280). */
        ESP_LOGI(TAG, "ft8_self_test: previous decode task is still exiting; "
                      "the watchdog will start the new one");
        return;
    }
    // ⛔ CLAIM THE SLOT BEFORE CREATING THE TASK, NOT INSIDE IT (#199).
    //
    // s_ft8_task_alive used to be set as ft8_task's first statement, so the
    // flag meant "an ft8_task has begun RUNNING" - and the guard above, and the
    // 1 Hz respawn watchdog in ft8_screen_view.c, both need it to mean "an
    // ft8_task EXISTS". Between those two readings sits the whole scheduling
    // delay, and ft8_task is created at tskIDLE_PRIORITY + 1: the LOWEST
    // priority on the board. On a busy core - capture, fft_task (4), taskLVGL,
    // the feeds (2) and httpd (5) all above it - taking more than a second to
    // get its first slice is ordinary, not exceptional. The same starvation is
    // measured directly in ft8_screen.c's record_decode comment.
    //
    // So the FT8-entry path spawned the task, the watchdog looked one second
    // later, saw a flag still false, logged "no ft8_task alive - respawning"
    // and spawned a SECOND one. Both then built the monitor pool - four
    // "Block size" lines where one build emits two - and the second build
    // double-freed the shared FFT scratch: `assert failed: tlsf_free ... block
    // already marked as free`, ft8_task -> build_monitor_pool -> free(). That
    // is almost certainly the unreproduced tlsf_free double-free reboot open
    // since v1.3.0.
    //
    // Setting the flag here closes the window by construction rather than by
    // timing: a grace period in the watchdog would only have made the race
    // rarer, and this file's history is emphatic that timing changes the odds
    // of a race, not the race.
    /* NOTHING JOINS ft8_task, so its spawner is the owner. The previous
     * instance parked on its way out and is still holding 64 KB of PSRAM;
     * free it here, before asking for another 64 KB (#279). Doing it at the
     * spawn rather than at the exit is what makes it safe - we are on the
     * LVGL task, an ordinary context, and the target is provably parked. */
    reap_pending_tasks();

    s_ft8_task_alive = true;
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        ft8_task, "ft8", 65536, NULL,
        tskIDLE_PRIORITY + 1, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        // Nothing will ever clear it, so release the claim here or FT8 is dead
        // for the rest of the session with the watchdog refusing to retry.
        s_ft8_task_alive = false;
        ESP_LOGE(TAG, "failed to spawn ft8_task (rc=%d)", (int)rc);
    }
}

bool ft8_task_is_alive(void)
{
    return s_ft8_task_alive;
}
