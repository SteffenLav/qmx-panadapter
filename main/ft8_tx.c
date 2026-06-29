// v0.12.0: Manual FT8 TX core (Reply + Call CQ). See ft8_tx.h for the
// design overview and docs/qmx-reference/SOURCES.md for the underlying
// QMX CAT sequence (TA; "Transmit Audio" - radio does its own DDS
// synthesis + envelope shaping; we just feed it tone frequencies).

#include "ft8_tx.h"
#include "ft8_status.h"
#include "ft8_test.h"   // ft8_op_mode_get() - FT8/FT4 sub-mode

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "ft8/message.h"
#include "ft8/encode.h"

#include "cat/cat.h"
#include "storage/settings.h"

static const char *TAG = "ft8_tx";

// Compile-time safety switch for bring-up (plan v0.12.0 §10 "verification
// plan", step 1). While 0, ft8_tx_run() runs the *entire* sequence -
// pre-flight, poll pause/resume, scheduling, the 79-symbol timing loop,
// abort handling, state transitions - but logs each TX;/TA<freq>;/TA0;/RX;
// via ESP_LOGI with precise timestamps instead of writing to the radio.
// This validates everything (slot-parity scheduling, message -> tones ->
// frequency math, the 160 ms cadence, abort paths, the full UI flow) without
// ever keying up the transmitter. Flip to 1 only once several real slot
// cycles look correct in the dry-run logs.
#ifndef FT8_TX_SEND_LIVE
#define FT8_TX_SEND_LIVE 1
#endif

#define FT8_TONE_SPACING_HZ      6.25f      // FT8 tone spacing (8-FSK, 0..7)
#define FT8_SYMBOL_PERIOD_US     160000     // 160 ms/symbol, in microseconds
// FT4 4-FSK (tones 0..3), 48 ms/symbol -> spacing = 1/symbol_period.
// FT4_SYMBOL_PERIOD (0.048f) comes from ft8/constants.h, kept as the single
// source of truth rather than duplicating the raw number here.
#define FT4_SYMBOL_PERIOD_US     ((int)(FT4_SYMBOL_PERIOD * 1000000.0f))   // 48000
#define FT4_TONE_SPACING_HZ      (1.0f / FT4_SYMBOL_PERIOD)                // ~20.833 Hz
#define FT8_TX_KEYUP_TONE_HZ     0.0f       // "any value < 10 Hz" keys up (CAT manual)
#define FT8_TX_ENVELOPE_SETTLE_MS  5        // wait after TA0; before RX; (CAT manual sequence)
#define FT8_TX_MODE_POLL_MS      100
#define FT8_TX_MODE_POLL_TRIES   10         // ~1s worst case; MD; refreshes ~every 150ms

// Placeholder PWR/SWR shown for any SIMULATED burst (general sim mode or
// FT4's forced-sim) - there is no real transmitter output to query when sim
// is on, but the UI's live PWR/SWR line should still appear (same code path,
// same layout) rather than silently differ from a real FT8 burst. Fixed,
// plausible QRP values; tagged in the log as a placeholder, never claimed to
// be a measurement.
#define FT8_TX_SIM_POWER_W       5.0f
#define FT8_TX_SIM_SWR           1.2f

// Protocol of the currently-selected FT8/FT4 sub-mode, for build-time use
// (the request itself then carries this in req->protocol - see ft8_tx.h).
static inline ftx_protocol_t cur_proto(void)
{
    return (ft8_op_mode_get() == FT8_OP_MODE_FT4) ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
}

// Encode to the tone alphabet matching `proto` - ft8_lib exposes separate
// encoders (8-FSK FT8_NN=79 symbols vs 4-FSK FT4_NN=105) rather than one
// protocol-switched function.
static inline void encode_tones(const uint8_t *payload, uint8_t *tones, ftx_protocol_t proto)
{
    if (proto == FTX_PROTOCOL_FT4) ft4_encode(payload, tones);
    else                           ft8_encode(payload, tones);
}

// CQ audio-frequency auto-selection scan parameters.
// The usable FT8 passband on the QMX is roughly 200–2800 Hz (signals right
// at the edge often have degraded decode rates on narrow receivers, and the
// QMX audio path attenuates below ~200 Hz).  Each FT8 signal occupies
// 7 × 6.25 = 43.75 Hz; we snap to 50-Hz increments, giving 52 slots that
// fit exactly in a uint64_t bitmask.
#define FT8_AUDIO_SCAN_MIN_HZ    200
#define FT8_AUDIO_SCAN_MAX_HZ   2800
#define FT8_AUDIO_SLOT_HZ         50
// (2800 - 200) / 50 = 52 — must be ≤ 63 for the uint64_t bitmask.

// ---------------------------------------------------------------------------
// State. Guarded by s_lock; ft8_tx_run() itself runs lock-free because it is
// only ever invoked from ft8_task's slot-loop thread, serialized by the loop
// itself (ft8_tx_should_run_this_slot() already transitioned us to ACTIVE
// under the lock before returning true).
// ---------------------------------------------------------------------------

static SemaphoreHandle_t s_lock = NULL;
static ft8_tx_state_t    s_state = FT8_TX_IDLE;
static ft8_tx_request_t  s_armed;                 // valid when s_state != IDLE
static volatile bool     s_abort_requested = false;

// Last PC;/SW; reading taken at the tail of a TX burst (see ft8_tx_run).
static float    s_last_power_w = -1.0f;
static float    s_last_swr     = -1.0f;
static int64_t  s_last_pwr_swr_us = -1;  // esp_timer_get_time() at capture, -1 if never

float ft8_tx_get_last_power_swr(float *power_w, float *swr)
{
    if (power_w) *power_w = s_last_power_w;
    if (swr) *swr = s_last_swr;
    if (s_last_pwr_swr_us < 0) return -1.0f;
    return (float)(esp_timer_get_time() - s_last_pwr_swr_us) / 1e6f;
}

void ft8_tx_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    memset(&s_armed, 0, sizeof(s_armed));
    s_state = FT8_TX_IDLE;
    ESP_LOGI(TAG, "FT8 TX core ready (mutex=%p, SEND_LIVE=%d)", s_lock, FT8_TX_SEND_LIVE);
}

static inline void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---------------------------------------------------------------------------
// Slot parity. FT8 slots start every 15 UTC seconds; by FT8 convention the
// two halves of each minute alternate "first" (even, :00/:30) and "second"
// (odd, :15/:45) sequences. wait_for_slot_boundary_ms() in ft8_test.c returns
// the exact UTC millisecond the slot started; ft8_screen_record_decode()
// stores that truncated to whole seconds as ft8_call_t.last_utc.
//
// FT4 classification of an already-truncated-to-seconds value (e.g. a heard
// station's last_utc) is NOT simply "/15 on a different number" - it needs a
// closed-form derivation, not a re-use of the FT8 formula:
//   FT4 slots are 7.5 s apart; real boundary k (k=0,1,2,...) sits at
//   k*7500 ms, truncated to whole seconds = floor(k*7.5). Since 7.5*2 = 15
//   exactly, every PAIR of FT4 slots advances the truncated value by exactly
//   15 - so floor(k*7.5) mod 15 is deterministically 0 when k is even, and 7
//   when k is odd (floor(7.5) = 7), regardless of which pair you're in. So
//   "is this last_utc an EVEN-k slot" reduces to a single mod-15 test, with
//   no information lost despite the truncation. (Contrast with computing a
//   future boundary in seconds, e.g. ft8_qso.c's next_slot_sec() - that's a
//   different problem and needs millisecond-precision math instead, since
//   you're choosing where the boundary lands, not classifying one you
//   already have.)
static inline bool slot_is_even(int64_t slot_start_unix_sec, ftx_protocol_t proto)
{
    if (proto == FTX_PROTOCOL_FT4) return (slot_start_unix_sec % 15) == 0;
    return ((slot_start_unix_sec / 15) % 2) == 0;
}

// Seconds until the next slot boundary matching the given parity preference,
// for the UI's ARMED countdown. If `match_parity` is true, keeps searching
// forward until the parity equals `want_even` (used for a parity-restricted
// CQ or a REPLY; a plain CQ requests fire on the very next boundary so pass
// match_parity=false).
//
// Works in milliseconds internally, using `proto`'s own slot period (15000 ms
// FT8 / 7500 ms FT4), then rounds UP to whole seconds only for the display
// value - same fix as ft8_tx_should_run_this_slot()'s parity bug: computing
// this in whole seconds and dividing by a hardcoded 15 silently breaks FT4
// (its 7.5 s grid doesn't divide evenly into seconds), which is exactly what
// made an armed FT4 CQ's on-screen countdown still read like an FT8 cadence
// even after the actual TX-firing parity was fixed.
static int seconds_until_slot(bool match_parity, bool want_even, ftx_protocol_t proto)
{
    int period_ms = (proto == FTX_PROTOCOL_FT4) ? 7500 : 15000;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_ms  = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int64_t next_ms = (now_ms / period_ms) * period_ms + period_ms;
    if (match_parity) {
        while ((((next_ms / period_ms) % 2) == 0) != want_even) next_ms += period_ms;
    }
    int64_t delta_ms = next_ms - now_ms;
    if (delta_ms < 0) delta_ms = 0;
    return (int)((delta_ms + 999) / 1000);   // round up to whole seconds for display
}

// ---------------------------------------------------------------------------
// CQ slot auto-selection
// ---------------------------------------------------------------------------

// Scan the current heard-station table and return the audio frequency (Hz)
// of the nearest unoccupied 50-Hz slot to FT8_TX_CQ_DEFAULT_FREQ_HZ (1500 Hz).
// Each heard station's slot is marked occupied together with one guard slot on
// each side, giving 150 Hz of clearance around active signals.
//
// If nothing has been decoded yet (empty table), all bins appear free and
// 1500 Hz is returned immediately.  If every bin is occupied (extremely packed
// band), falls back to FT8_TX_CQ_DEFAULT_FREQ_HZ.
//
// Heap-allocated internally to avoid ~11 KB of stack pressure in the LVGL
// event-handler context where this is called (cq_btn_cb). PSRAM, not plain
// malloc: this fires every RX slot during CQ-tone-clash checking, and a
// transient ~11 KB internal-RAM bite that often was a real contributor to
// the WiFi co-processor link dying under load (see CLAUDE.md "config import
// /export buffers" fix - same bug class).
int ft8_find_clear_tone_hz_near(int center_hz)
{
    ft8_call_t *calls = heap_caps_malloc(FT8_CALL_TABLE_SIZE * sizeof(ft8_call_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!calls) return center_hz;

    int n = 0;
    ft8_screen_get_all(calls, FT8_CALL_TABLE_SIZE, &n);

    // Number of 50-Hz slots across [SCAN_MIN, SCAN_MAX). = 52
    const int n_slots = (FT8_AUDIO_SCAN_MAX_HZ - FT8_AUDIO_SCAN_MIN_HZ)
                        / FT8_AUDIO_SLOT_HZ;

    // Build occupancy bitmask.  Guard bands: mark the signal's own slot
    // plus one slot on each side as occupied.
    uint64_t occupied = 0;
    for (int i = 0; i < n; i++) {
        int bin = ((int)calls[i].last_freq - FT8_AUDIO_SCAN_MIN_HZ)
                  / FT8_AUDIO_SLOT_HZ;
        for (int g = bin - 1; g <= bin + 1; g++) {
            if (g >= 0 && g < n_slots)
                occupied |= (1ULL << g);
        }
    }
    free(calls);

    // Walk outward from the centre bin for the nearest clear slot. Prefer the
    // lower bin when both equidistant (-r first).
    int centre = (center_hz - FT8_AUDIO_SCAN_MIN_HZ) / FT8_AUDIO_SLOT_HZ;
    if (centre < 0) centre = 0;
    if (centre >= n_slots) centre = n_slots - 1;
    for (int r = 0; r <= n_slots / 2; r++) {
        int b1 = centre - r;
        if (b1 >= 0 && b1 < n_slots && !(occupied & (1ULL << b1))) {
            int freq = FT8_AUDIO_SCAN_MIN_HZ + b1 * FT8_AUDIO_SLOT_HZ;
            ESP_LOGI(TAG, "clear-tone scan: clear at %d Hz (r=%d, %d stations heard)", freq, r, n);
            return freq;
        }
        if (r > 0) {
            int b2 = centre + r;
            if (b2 < n_slots && !(occupied & (1ULL << b2))) {
                int freq = FT8_AUDIO_SCAN_MIN_HZ + b2 * FT8_AUDIO_SLOT_HZ;
                ESP_LOGI(TAG, "clear-tone scan: clear at %d Hz (r=%d, %d stations heard)", freq, r, n);
                return freq;
            }
        }
    }

    ESP_LOGW(TAG, "clear-tone scan: all %d slots occupied - staying at %d Hz",
             n_slots, center_hz);
    return center_hz;
}

int ft8_find_clear_tone_hz(void)
{
    return ft8_find_clear_tone_hz_near(FT8_TX_CQ_DEFAULT_FREQ_HZ);
}

bool ft8_tx_is_clashing(void)
{
    // Grab the armed freq and target call under the lock, then release before
    // calling ft8_screen_get_all() (which takes its own mutex).
    lock();
    if (s_state == FT8_TX_IDLE) { unlock(); return false; }
    int our_hz      = s_armed.audio_freq_hz;
    ft8_tx_kind_t kind = s_armed.kind;
    char target[FT8_CALL_MAX_LEN];
    strncpy(target, s_armed.target_call, sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';
    unlock();

    // PSRAM: called every 1 s from the FT8 screen's clock timer while a TX is
    // armed/active, far too frequent for an ~11 KB internal-RAM allocation.
    ft8_call_t *calls = heap_caps_malloc(FT8_CALL_TABLE_SIZE * sizeof(ft8_call_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!calls) return false;
    int n = 0;
    ft8_screen_get_all(calls, FT8_CALL_TABLE_SIZE, &n);

    const int n_slots = (FT8_AUDIO_SCAN_MAX_HZ - FT8_AUDIO_SCAN_MIN_HZ) / FT8_AUDIO_SLOT_HZ;
    int our_bin = (our_hz - FT8_AUDIO_SCAN_MIN_HZ) / FT8_AUDIO_SLOT_HZ;

    bool clash = false;
    if (our_bin >= 0 && our_bin < n_slots) {
        for (int i = 0; i < n; i++) {
            // For a REPLY, the target station IS expected at this frequency —
            // skip them. A second station at the same bin (pile-up) is still a clash.
            if (kind == FT8_TX_KIND_REPLY && target[0] &&
                strncmp(calls[i].call, target, sizeof(calls[i].call)) == 0)
                continue;
            int their_bin = ((int)calls[i].last_freq - FT8_AUDIO_SCAN_MIN_HZ) / FT8_AUDIO_SLOT_HZ;
            if (abs(their_bin - our_bin) <= 1) {
                clash = true;
                break;
            }
        }
    }
    free(calls);
    return clash;
}

// ---------------------------------------------------------------------------
// Message building
// ---------------------------------------------------------------------------

bool ft8_tx_build_request(ft8_tx_kind_t kind,
                          const char *target_call,
                          int target_audio_freq_hz,
                          int64_t target_last_utc,
                          const char *extra,
                          ft8_tx_request_t *out_req,
                          char *out_err, size_t out_err_len)
{
    if (out_err && out_err_len) out_err[0] = '\0';
    if (!out_req) return false;
    memset(out_req, 0, sizeof(*out_req));

    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.my_callsign[0] || !s.my_grid[0]) {
        if (out_err) snprintf(out_err, out_err_len,
                              "Set your callsign and grid first (Settings)");
        return false;
    }

    const char *call_to;
    if (kind == FT8_TX_KIND_CQ) {
        call_to = "CQ";
    } else {
        if (!target_call || !target_call[0]) {
            if (out_err) snprintf(out_err, out_err_len, "No target callsign");
            return false;
        }
        call_to = target_call;
        strncpy(out_req->target_call, target_call, sizeof(out_req->target_call) - 1);
    }

    // Third field: explicit extra overrides my_grid for QSO exchange messages.
    const char *third = extra ? extra : s.my_grid;
    if (!third[0]) {
        if (out_err) snprintf(out_err, out_err_len, "No grid set (Settings)");
        return false;
    }

    // Encode now — never at burst time. Errors surface here in the UI, not
    // mid-burst where there's nothing we can do about them.
    ftx_message_t msg;
    ftx_message_rc_t rc = ftx_message_encode_std(&msg, NULL, call_to, s.my_callsign, third);
    if (rc != FTX_MESSAGE_RC_OK) {
        if (out_err) snprintf(out_err, out_err_len,
                              "Can't encode message (rc=%d)", (int)rc);
        return false;
    }

    // Slot parity: REPLY/ROGER_RPT/73 all fire on the slot opposite to when
    // the target last transmitted (target_last_utc). CQ fires on any slot
    // unless the caller overrides use_parity+want_even_slot afterwards.
    bool needs_parity = (kind == FT8_TX_KIND_REPLY    ||
                         kind == FT8_TX_KIND_ROGER_RPT ||
                         kind == FT8_TX_KIND_73);

    out_req->kind           = kind;
    out_req->audio_freq_hz  = target_audio_freq_hz;
    out_req->protocol       = cur_proto();
    out_req->want_even_slot = needs_parity ? !slot_is_even(target_last_utc, out_req->protocol) : false;
    out_req->use_parity     = needs_parity && (target_last_utc != 0);
    encode_tones(msg.payload, out_req->tones, out_req->protocol);
    snprintf(out_req->display_text, sizeof(out_req->display_text),
             "%s %s %s", call_to, s.my_callsign, third);
    if (extra) strncpy(out_req->extra_field, extra, sizeof(out_req->extra_field) - 1);

    static const char * const kind_names[] = { "reply", "CQ", "roger-rpt", "73" };
    ESP_LOGI(TAG, "built %s: '%s' @ %d Hz%s",
             kind_names[kind], out_req->display_text, out_req->audio_freq_hz,
             out_req->use_parity
                 ? (out_req->want_even_slot ? " (EVEN)" : " (ODD)")
                 : " (any slot)");
    return true;
}

bool ft8_tx_build_request_fd(ft8_tx_kind_t kind,
                             const char *target_call,
                             int target_audio_freq_hz,
                             int64_t target_last_utc,
                             const char *class_section,
                             ft8_tx_request_t *out_req,
                             char *out_err, size_t out_err_len)
{
    if (out_err && out_err_len) out_err[0] = '\0';
    if (!out_req) return false;
    memset(out_req, 0, sizeof(*out_req));

    if (kind != FT8_TX_KIND_REPLY && kind != FT8_TX_KIND_ROGER_RPT) {
        if (out_err) snprintf(out_err, out_err_len, "Bad kind for Field Day message");
        return false;
    }
    if (!target_call || !target_call[0]) {
        if (out_err) snprintf(out_err, out_err_len, "No target callsign");
        return false;
    }
    if (!class_section || !class_section[0]) {
        if (out_err) snprintf(out_err, out_err_len, "No Field Day class/section");
        return false;
    }

    qmx_settings_t s;
    settings_load_all(&s);
    if (!s.my_callsign[0]) {
        if (out_err) snprintf(out_err, out_err_len, "Set your callsign first (Settings)");
        return false;
    }

    strncpy(out_req->target_call, target_call, sizeof(out_req->target_call) - 1);

    ftx_message_t msg;
    ftx_message_rc_t rc = ftx_message_encode_arrl_fd(&msg, NULL, target_call, s.my_callsign, class_section);
    if (rc != FTX_MESSAGE_RC_OK) {
        if (out_err) snprintf(out_err, out_err_len,
                              "Can't encode Field Day message (rc=%d)", (int)rc);
        return false;
    }

    out_req->kind           = kind;
    out_req->audio_freq_hz  = target_audio_freq_hz;
    out_req->want_even_slot = !slot_is_even(target_last_utc, FTX_PROTOCOL_FT8);
    out_req->use_parity     = (target_last_utc != 0);
    // ARRL Field Day exchange has no FT4 wire format - always FT8, regardless
    // of the operator's current FT8/FT4 sub-mode selection.
    out_req->protocol       = FTX_PROTOCOL_FT8;
    ft8_encode(msg.payload, out_req->tones);
    snprintf(out_req->display_text, sizeof(out_req->display_text),
             "%s %s %s", target_call, s.my_callsign, class_section);
    strncpy(out_req->extra_field, class_section, sizeof(out_req->extra_field) - 1);

    ESP_LOGI(TAG, "built FD %s: '%s' @ %d Hz%s",
             kind == FT8_TX_KIND_REPLY ? "reply" : "roger-rpt",
             out_req->display_text, out_req->audio_freq_hz,
             out_req->use_parity ? (out_req->want_even_slot ? " (EVEN)" : " (ODD)") : " (any slot)");
    return true;
}

bool ft8_tx_build_request_text(const char *message_text,
                               int audio_freq_hz,
                               ft8_tx_request_t *out_req,
                               char *out_err, size_t out_err_len)
{
    if (out_err && out_err_len) out_err[0] = '\0';
    if (!out_req) return false;
    memset(out_req, 0, sizeof(*out_req));
    if (!message_text || !message_text[0]) {
        if (out_err) snprintf(out_err, out_err_len, "Empty message");
        return false;
    }

    ftx_message_t msg;
    ftx_message_rc_t rc = ftx_message_encode(&msg, NULL, message_text);
    if (rc != FTX_MESSAGE_RC_OK) {
        if (out_err) snprintf(out_err, out_err_len,
                              "Can't encode '%s' (rc=%d)", message_text, (int)rc);
        return false;
    }

    out_req->kind          = FT8_TX_KIND_CQ;
    out_req->audio_freq_hz = audio_freq_hz;
    out_req->use_parity    = false;
    out_req->want_even_slot = false;
    out_req->protocol      = cur_proto();
    encode_tones(msg.payload, out_req->tones, out_req->protocol);
    strncpy(out_req->display_text, message_text, sizeof(out_req->display_text) - 1);

    ESP_LOGI(TAG, "built text CQ: '%s' @ %d Hz", out_req->display_text, audio_freq_hz);
    return true;
}

// ---------------------------------------------------------------------------
// Arm / disarm / abort / status
// ---------------------------------------------------------------------------

bool ft8_tx_arm(const ft8_tx_request_t *req, char *out_err, size_t out_err_len)
{
    if (out_err && out_err_len) out_err[0] = '\0';
    if (!req) return false;

    lock();
    bool already_active = (s_state == FT8_TX_ACTIVE);
    unlock();
    if (already_active) {
        if (out_err) snprintf(out_err, out_err_len, "Transmission already in progress");
        return false;
    }

    // ---- Digi-mode pre-flight (slow path - runs OUTSIDE the lock, so the
    // status getter / UI indicator stay responsive while this blocks the
    // calling task for up to ~1s). See ft8_tx.h doc comment + plan §5 for
    // why this happens here (seconds of lead time) and not at burst time
    // (where any settle delay would shift the slot-synchronised TX start).
    // Skipped entirely under the FT8 simulation-mode hard interlock (see
    // ft8_sim.h) - cat_set_mode() below is a real CAT write, and sim mode's
    // whole point is that NOTHING here touches a possibly-connected QMX.
    qmx_settings_t arm_sim_s;
    settings_load_all(&arm_sim_s);
    bool sim = arm_sim_s.sim_mode_en;
    const char *mode = sim ? "DiGi" : cat_get_mode_str();
    if (strcmp(mode, "DiGi") != 0) {
        ESP_LOGI(TAG, "arm: QMX mode is '%s' - switching to Digi...", mode);
        cat_set_mode("FT8");   // hamlib_mode_to_digit() maps this to digit '6' = DiGi
        bool confirmed = false;
        for (int i = 0; i < FT8_TX_MODE_POLL_TRIES; i++) {
            vTaskDelay(pdMS_TO_TICKS(FT8_TX_MODE_POLL_MS));
            if (strcmp(cat_get_mode_str(), "DiGi") == 0) { confirmed = true; break; }
        }
        if (!confirmed) {
            ESP_LOGW(TAG, "arm: QMX would not confirm Digi mode (still '%s')", cat_get_mode_str());
            if (out_err) snprintf(out_err, out_err_len,
                                  "QMX won't switch to Digi mode - check the radio");
            return false;
        }
        ESP_LOGI(TAG, "arm: QMX confirmed Digi mode");
    }

    lock();
    if (s_state == FT8_TX_ACTIVE) {
        // A burst could have started while we were blocked in pre-flight
        // above (e.g. a previously-armed request fired). Don't clobber it.
        unlock();
        if (out_err) snprintf(out_err, out_err_len, "Transmission already in progress");
        return false;
    }
    s_armed = *req;
    s_state = FT8_TX_ARMED;
    unlock();

    const char *parity_desc;
    if (req->kind == FT8_TX_KIND_CQ) {
        parity_desc = req->use_parity
                    ? (req->want_even_slot ? "CQ - EVEN slots only" : "CQ - ODD slots only")
                    : "CQ - next slot (any parity)";
    } else {
        parity_desc = req->want_even_slot ? "reply - needs EVEN slot" : "reply - needs ODD slot";
    }
    ESP_LOGI(TAG, "ARMED: '%s' (%s)", req->display_text, parity_desc);
    return true;
}

void ft8_tx_disarm(void)
{
    lock();
    if (s_state == FT8_TX_ARMED) {
        ESP_LOGI(TAG, "disarmed: '%s'", s_armed.display_text);
        s_state = FT8_TX_IDLE;
        memset(&s_armed, 0, sizeof(s_armed));
    }
    unlock();
}

void ft8_tx_request_abort(void)
{
    // Plain volatile flag, checked only between symbol sends inside
    // ft8_tx_run() - same cooperative pattern as cat.c's s_poll_paused.
    // No lock needed: at worst a stale request (state already IDLE) sets a
    // flag that ft8_tx_run() clears on its next entry without ever reading it.
    ESP_LOGI(TAG, "abort requested");
    s_abort_requested = true;
}

ft8_tx_state_t ft8_tx_get_status(char *text, size_t text_len, int *secs_until)
{
    lock();
    ft8_tx_state_t st = s_state;
    int secs = 0;

    if (text && text_len) {
        if (st == FT8_TX_IDLE) {
            text[0] = '\0';
        } else {
            strncpy(text, s_armed.display_text, text_len - 1);
            text[text_len - 1] = '\0';
        }
    }
    if (st == FT8_TX_ARMED) {
        secs = seconds_until_slot(s_armed.use_parity,
                                  s_armed.want_even_slot,
                                  s_armed.protocol);
    }
    unlock();

    if (secs_until) *secs_until = secs;
    return st;
}

// ---------------------------------------------------------------------------
// Slot-loop integration
// ---------------------------------------------------------------------------

bool ft8_tx_should_run_this_slot(int64_t slot_start_ms, ft8_tx_request_t *out)
{
    if (!out) return false;

    lock();
    bool fire = false;
    // Fire when: no parity requirement, OR parity matches.
    // use_parity is always true for REPLY; for CQ it's true only when the
    // operator has set an explicit EVEN/ODD TX preference.
    //
    // Parity is computed from the ARMED request's own protocol period
    // (s_armed.protocol), not a hardcoded /15. slot_start_ms is always an
    // exact multiple of that period (wait_for_slot_boundary_ms in ft8_test.c
    // quantizes it), so slot_start_ms/period_ms is an exact integer slot
    // index whose parity flips on EVERY real slot. This matters for FT4
    // (7.5 s period): the old version took whole-second slot_sec and divided
    // by the FT8-only constant 15, which for a 7.5 s grid truncates the
    // half-second and produces a broken even-even-odd-odd PAIRED pattern
    // instead of alternating every slot (seen on-air as CQ firing on two
    // consecutive slots, then silent for two, repeating) - this is what fixed
    // that. For FT8 (period exactly 15000 ms) the result is numerically
    // identical to the old formula, so FT8 behaviour is unchanged.
    int period_ms = (s_armed.protocol == FTX_PROTOCOL_FT4) ? 7500 : 15000;
    bool is_even = ((slot_start_ms / period_ms) % 2) == 0;
    if (s_state == FT8_TX_ARMED &&
        (!s_armed.use_parity || is_even == s_armed.want_even_slot)) {
        *out = s_armed;
        s_state = FT8_TX_ACTIVE;
        fire = true;
    }
    unlock();

    if (fire) {
        ESP_LOGI(TAG, "slot @%lldms: armed request '%s' matches - going ACTIVE",
                 (long long)slot_start_ms, out->display_text);
    }
    return fire;
}

// ---------------------------------------------------------------------------
// Burst executor
// ---------------------------------------------------------------------------

// Sends one CAT command - or, in dry-run/simulation mode, just logs it with
// a microsecond timestamp relative to t0. Centralising this keeps the burst
// sequencing identical between dry-run/sim and live modes; only the actual
// wire write differs. `buf` is a fully-formatted literal (no '%' survives
// from e.g. "TA1234.56;"), so passing it through cat_send_raw_cmd's
// printf-style interface as "%s" is safe.
//
// `sim` is the FT8 simulation-mode hard interlock (see ft8_sim.h): when
// true, NOT ONE byte reaches the CDC-ACM link, regardless of FT8_TX_SEND_LIVE
// - this is the only thing standing between "practice mode" and actually
// keying up a real, possibly-connected QMX.
static void tx_cmd(int64_t t0, bool sim, const char *fmt, ...)
{
    char buf[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (sim) {
        ESP_LOGI(TAG, "[SIM t+%6lldus] %s", (long long)(esp_timer_get_time() - t0), buf);
        return;
    }
#if FT8_TX_SEND_LIVE
    esp_err_t err = cat_send_raw_cmd("%s", buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send failed (0x%x): %s - continuing burst (radio may be disconnected)", err, buf);
    }
#else
    ESP_LOGI(TAG, "[DRY RUN t+%6lldus] %s", (long long)(esp_timer_get_time() - t0), buf);
#endif
}

// Sleep until t0 + offset_us, if that's still in the future. Anchoring every
// target to the fixed t0 (rather than chaining vTaskDelay calls) keeps 79
// sends from drifting cumulatively over the ~12.6s burst.
static void sleep_until(int64_t t0, int64_t offset_us)
{
    int64_t target = t0 + offset_us;
    int64_t now = esp_timer_get_time();
    if (target > now) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((target - now) / 1000)));
    }
}

void ft8_tx_run(const ft8_tx_request_t *req)
{
    if (!req) return;
    s_abort_requested = false;

    // FT8 simulation-mode hard interlock (see ft8_sim.h): when on, this
    // function still runs its full real-time-accurate sequence (so
    // ft8_qso.c's slot timing and state transitions behave identically to a
    // real burst), but every cat_* call is replaced with a log line - no
    // byte ever reaches the CDC-ACM link, so a real, connected QMX can never
    // be keyed while practicing against phantom stations. Checked once per
    // burst (not cached) so toggling the drawer switch mid-session takes
    // effect on the very next TX.
    //
    qmx_settings_t sim_s;
    settings_load_all(&sim_s);
    bool sim = sim_s.sim_mode_en;

    // Per-protocol timing/encoding, captured once from req->protocol (never
    // re-read from the live ft8_op_mode_get() mid-burst - see ft8_tx.h).
    const bool    is_ft4         = (req->protocol == FTX_PROTOCOL_FT4);
    const int     nn             = is_ft4 ? FT4_NN : FT8_NN;
    const int64_t symbol_period_us = is_ft4 ? FT4_SYMBOL_PERIOD_US : FT8_SYMBOL_PERIOD_US;
    const float   tone_spacing_hz  = is_ft4 ? FT4_TONE_SPACING_HZ : FT8_TONE_SPACING_HZ;

    // Final pre-flight: a cheap *cached-string* read (cat_get_mode_str()
    // just returns the digit from the last MD; poll response - no CAT round
    // trip), not a re-check-and-fix. If the operator changed modes after
    // arming, abort cleanly *before* TX; - a corrective cat_set_mode() here
    // would shift the burst start off the slot boundary and desync every
    // receiving decoder. Invariant: start exactly on time, or not at all.
    // Skipped entirely in sim mode - there's no real radio mode to drift.
    const char *mode = sim ? "DiGi" : cat_get_mode_str();
    if (strcmp(mode, "DiGi") != 0) {
        ESP_LOGW(TAG, "TX aborted before key-up: mode drifted to '%s' (need DiGi)", mode);
    } else {
        ESP_LOGI(TAG, "TX burst starting (%s): '%s' base=%d Hz%s",
                 is_ft4 ? "FT4" : "FT8", req->display_text, req->audio_freq_hz,
                 sim ? (is_ft4 ? "  [FT4 - simulation mode]"
                               : "  [SIMULATION - radio not keyed]")
                     : (FT8_TX_SEND_LIVE ? "" : "  [DRY RUN - logging only, radio not keyed]"));

        // Exclusive use of the CDC-ACM link for the whole burst - an
        // interleaved FA;/MD;/FW; poll mid-sequence could desync our timing
        // or garble the stream. Cooperative flag only (see cat.c) - never
        // vTaskSuspend, which risks deadlocking on the driver's internal
        // mutex if the poll task is suspended mid-transfer. Not needed in
        // sim mode - nothing here touches the CDC link.
        if (!sim) cat_poll_set_paused(true);

        int64_t t0 = esp_timer_get_time();
        tx_cmd(t0, sim, "TX;");   // key down - radio's own envelope shaping

        // Live power/SWR: fire ONE non-blocking PC;SW; once the PA has settled
        // (symbol 6 ≈ 1 s in), then read the async response a few symbols later
        // (symbol 14). Both steps fit inside the 160 ms inter-symbol slack (the
        // send is a ~ms CDC write bounded to 50 ms; the read just parses
        // buffers), so symbol timing is undisturbed - unlike cat_query_power_swr()'s
        // ~600 ms blocking wait, which is why that one only runs at burst end.
        // Result populates s_last_* so the "TRANSMITTING:" line shows the CURRENT
        // burst's reading from ~2 s in. Skipped in sim mode (no real link).
        bool ps_sent = false, ps_have = false;

        bool aborted = false;
        for (int i = 0; i < nn; i++) {
            if (s_abort_requested) {
                ESP_LOGW(TAG, "TX abort requested at symbol %d/%d - keying up now", i, nn);
                aborted = true;
                break;
            }
            // Update status every ~10 symbols so the UI shows TX progress.
            if (i == 0 || i % 10 == 0) {
                ft8_status_set("%s[%d/%d] %s", sim ? "SIM TX " : "TX ", i + 1, nn, req->display_text);
            }
            float freq = (float)req->audio_freq_hz + (float)req->tones[i] * tone_spacing_hz;
            sleep_until(t0, (int64_t)i * symbol_period_us);
            tx_cmd(t0, sim, "TA%.2f;", (double)freq);
            // Live power/SWR mid-burst query is FT8-only timing (tuned to FT8's
            // 160 ms symbol slack at symbols 6/14); skipped for FT4 (forced sim
            // anyway - !sim is always false here when is_ft4).
#if FT8_TX_SEND_LIVE
            if (!sim) {
                if (!ps_sent && i == 6) {
                    cat_pwr_swr_async_send();
                    ps_sent = true;
                } else if (ps_sent && !ps_have && i >= 14) {
                    float pw = -1.0f, sw = -1.0f;
                    if (cat_pwr_swr_async_read(&pw, &sw) == ESP_OK && pw >= 0.0f && sw >= 0.0f) {
                        s_last_power_w   = pw;
                        s_last_swr       = sw;
                        s_last_pwr_swr_us = esp_timer_get_time();
                        ps_have = true;
                        ESP_LOGI(TAG, "live TX power=%.1fW SWR=%.2f", (double)pw, (double)sw);
                    }
                }
            } else if (!ps_have && i == nn / 4) {
                // Simulated burst (general sim mode, or FT4's always-forced
                // sim): no real PA to query, so populate the same s_last_*
                // fields with a fixed placeholder reading at roughly the same
                // point in the burst a real reading would land, so the UI's
                // live PWR/SWR line behaves identically either way.
                s_last_power_w    = FT8_TX_SIM_POWER_W;
                s_last_swr        = FT8_TX_SIM_SWR;
                s_last_pwr_swr_us = esp_timer_get_time();
                ps_have = true;
                ESP_LOGI(TAG, "sim TX power=%.1fW SWR=%.2f (placeholder, not measured)",
                         (double)FT8_TX_SIM_POWER_W, (double)FT8_TX_SIM_SWR);
            }
#endif
        }

        if (!aborted) {
            // Let the final symbol play out its full period before keying
            // up - otherwise we'd truncate the last tone for receivers.
            sleep_until(t0, (int64_t)nn * symbol_period_us);
        }
        // Either way - whether all symbols played or we broke out early
        // on an abort request - key up immediately now. This is the part
        // that must ALWAYS run: the radio must never be left transmitting.
        tx_cmd(t0, sim, "TA%.0f;", (double)FT8_TX_KEYUP_TONE_HZ);
        vTaskDelay(pdMS_TO_TICKS(FT8_TX_ENVELOPE_SETTLE_MS));

        // Query power/SWR while still keyed - SW; returns no reading once
        // back in Receive mode. Do this for both normal and aborted bursts.
        // Skipped entirely in sim mode - there's nothing to query.
        float power_w = -1.0f, swr = -1.0f;
#if FT8_TX_SEND_LIVE
        if (!sim) {
            esp_err_t pswr_err = cat_query_power_swr(&power_w, &swr);
            ESP_LOGI(TAG, "post-burst PC/SW query: err=0x%x power=%.1f swr=%.2f",
                     pswr_err, (double)power_w, (double)swr);
            if (power_w >= 0.0f && swr >= 0.0f) {
                ESP_LOGI(TAG, "TX power=%.1fW SWR=%.2f", (double)power_w, (double)swr);
                s_last_power_w = power_w;
                s_last_swr = swr;
                s_last_pwr_swr_us = esp_timer_get_time();
            }
        }
#endif

        tx_cmd(t0, sim, "RX;");

#if FT8_TX_SEND_LIVE
        if (!sim && power_w >= 0.0f && swr > 4.0f) {
            ESP_LOGW(TAG, "SWR protection trip (SWR=%.2f) — cycling TX/RX to clear latch",
                     (double)swr);
            cat_send_raw_cmd("TX;");
            vTaskDelay(pdMS_TO_TICKS(150));
            cat_send_raw_cmd("RX;");
            ESP_LOGI(TAG, "SWR latch clear cycle done");
        }
#endif

        if (!sim) cat_poll_set_paused(false);

        ESP_LOGI(TAG, "TX burst %s%s: '%s' (%lld ms on-air)",
                 sim ? "[SIM] " : "", aborted ? "ABORTED" : "complete", req->display_text,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }

    lock();
    s_state = FT8_TX_IDLE;
    memset(&s_armed, 0, sizeof(s_armed));
    unlock();
    s_abort_requested = false;
}
