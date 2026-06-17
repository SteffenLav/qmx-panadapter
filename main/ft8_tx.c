// v0.12.0: Manual FT8 TX core (Reply + Call CQ). See ft8_tx.h for the
// design overview and docs/qmx-reference/SOURCES.md for the underlying
// QMX CAT sequence (TA; "Transmit Audio" - radio does its own DDS
// synthesis + envelope shaping; we just feed it tone frequencies).

#include "ft8_tx.h"
#include "ft8_status.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
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

#define FT8_TONE_SPACING_HZ      6.25f      // FT8 tone spacing
#define FT8_SYMBOL_PERIOD_US     160000     // 160 ms/symbol, in microseconds
#define FT8_TX_KEYUP_TONE_HZ     0.0f       // "any value < 10 Hz" keys up (CAT manual)
#define FT8_TX_ENVELOPE_SETTLE_MS  5        // wait after TA0; before RX; (CAT manual sequence)
#define FT8_TX_MODE_POLL_MS      100
#define FT8_TX_MODE_POLL_TRIES   10         // ~1s worst case; MD; refreshes ~every 150ms

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
// Slot parity. Slots start every 15 UTC seconds; by FT8 convention the two
// halves of each minute alternate "first" (even, :00/:30) and "second" (odd,
// :15/:45) sequences. wait_for_slot_boundary() in ft8_test.c returns exactly
// the value this expects (the UTC second the slot started), and
// ft8_screen_record_decode() stores that same value as ft8_call_t.last_utc.
// ---------------------------------------------------------------------------

static inline bool slot_is_even(int64_t slot_start_unix_sec)
{
    return ((slot_start_unix_sec / 15) % 2) == 0;
}

// Seconds from `now` to the next slot boundary. If `match_parity` is true,
// keeps searching forward until the parity equals `want_even` (used for the
// UI countdown on an armed REPLY; CQ requests fire on the very next boundary
// so pass match_parity=false).
static int seconds_until_slot(time_t now, bool match_parity, bool want_even)
{
    int64_t next = ((int64_t)now / 15) * 15 + 15;
    if (match_parity) {
        while (slot_is_even(next) != want_even) next += 15;
    }
    int64_t delta = next - (int64_t)now;
    return (delta < 0) ? 0 : (int)delta;
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
// event-handler context where this is called (cq_btn_cb).
int ft8_find_clear_tone_hz(void)
{
    ft8_call_t *calls = malloc(FT8_CALL_TABLE_SIZE * sizeof(ft8_call_t));
    if (!calls) return FT8_TX_CQ_DEFAULT_FREQ_HZ;

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

    // Walk outward from the centre bin (1500 Hz = bin 26) for the nearest
    // clear slot.  Prefer the lower bin when both equidistant (—r first).
    const int centre = (FT8_TX_CQ_DEFAULT_FREQ_HZ - FT8_AUDIO_SCAN_MIN_HZ)
                       / FT8_AUDIO_SLOT_HZ;   // = (1500-200)/50 = 26
    for (int r = 0; r <= n_slots / 2; r++) {
        int b1 = centre - r;
        if (b1 >= 0 && b1 < n_slots && !(occupied & (1ULL << b1))) {
            int freq = FT8_AUDIO_SCAN_MIN_HZ + b1 * FT8_AUDIO_SLOT_HZ;
            ESP_LOGI(TAG, "CQ scan: clear at %d Hz (r=%d, %d stations heard)", freq, r, n);
            return freq;
        }
        if (r > 0) {
            int b2 = centre + r;
            if (b2 < n_slots && !(occupied & (1ULL << b2))) {
                int freq = FT8_AUDIO_SCAN_MIN_HZ + b2 * FT8_AUDIO_SLOT_HZ;
                ESP_LOGI(TAG, "CQ scan: clear at %d Hz (r=%d, %d stations heard)", freq, r, n);
                return freq;
            }
        }
    }

    ESP_LOGW(TAG, "CQ scan: all %d slots occupied - using default %d Hz",
             n_slots, FT8_TX_CQ_DEFAULT_FREQ_HZ);
    return FT8_TX_CQ_DEFAULT_FREQ_HZ;
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
    out_req->want_even_slot = needs_parity ? !slot_is_even(target_last_utc) : false;
    out_req->use_parity     = needs_parity && (target_last_utc != 0);
    ft8_encode(msg.payload, out_req->tones);
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
    ft8_encode(msg.payload, out_req->tones);
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
    const char *mode = cat_get_mode_str();
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
        secs = seconds_until_slot(time(NULL),
                                  s_armed.use_parity,
                                  s_armed.want_even_slot);
    }
    unlock();

    if (secs_until) *secs_until = secs;
    return st;
}

// ---------------------------------------------------------------------------
// Slot-loop integration
// ---------------------------------------------------------------------------

bool ft8_tx_should_run_this_slot(int64_t slot_start_unix_sec, ft8_tx_request_t *out)
{
    if (!out) return false;

    lock();
    bool fire = false;
    // Fire when: no parity requirement, OR parity matches.
    // use_parity is always true for REPLY; for CQ it's true only when the
    // operator has set an explicit EVEN/ODD TX preference.
    if (s_state == FT8_TX_ARMED &&
        (!s_armed.use_parity ||
         slot_is_even(slot_start_unix_sec) == s_armed.want_even_slot)) {
        *out = s_armed;
        s_state = FT8_TX_ACTIVE;
        fire = true;
    }
    unlock();

    if (fire) {
        ESP_LOGI(TAG, "slot %lld: armed request '%s' matches - going ACTIVE",
                 (long long)slot_start_unix_sec, out->display_text);
    }
    return fire;
}

// ---------------------------------------------------------------------------
// Burst executor
// ---------------------------------------------------------------------------

// Sends one CAT command - or, in dry-run mode, just logs it with a
// microsecond timestamp relative to t0. Centralising this keeps the burst
// sequencing identical between dry-run and live modes; only the actual
// wire write differs. `buf` is a fully-formatted literal (no '%' survives
// from e.g. "TA1234.56;"), so passing it through cat_send_raw_cmd's
// printf-style interface as "%s" is safe.
static void tx_cmd(int64_t t0, const char *fmt, ...)
{
    char buf[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

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

    // Final pre-flight: a cheap *cached-string* read (cat_get_mode_str()
    // just returns the digit from the last MD; poll response - no CAT round
    // trip), not a re-check-and-fix. If the operator changed modes after
    // arming, abort cleanly *before* TX; - a corrective cat_set_mode() here
    // would shift the burst start off the slot boundary and desync every
    // receiving decoder. Invariant: start exactly on time, or not at all.
    const char *mode = cat_get_mode_str();
    if (strcmp(mode, "DiGi") != 0) {
        ESP_LOGW(TAG, "TX aborted before key-up: mode drifted to '%s' (need DiGi)", mode);
    } else {
        ESP_LOGI(TAG, "TX burst starting: '%s' base=%d Hz%s",
                 req->display_text, req->audio_freq_hz,
                 FT8_TX_SEND_LIVE ? "" : "  [DRY RUN - logging only, radio not keyed]");

        // Exclusive use of the CDC-ACM link for the whole burst - an
        // interleaved FA;/MD;/FW; poll mid-sequence could desync our timing
        // or garble the stream. Cooperative flag only (see cat.c) - never
        // vTaskSuspend, which risks deadlocking on the driver's internal
        // mutex if the poll task is suspended mid-transfer.
        cat_poll_set_paused(true);

        int64_t t0 = esp_timer_get_time();
        tx_cmd(t0, "TX;");   // key down - radio's own envelope shaping

        bool aborted = false;
        for (int i = 0; i < FT8_NN; i++) {
            if (s_abort_requested) {
                ESP_LOGW(TAG, "TX abort requested at symbol %d/%d - keying up now", i, FT8_NN);
                aborted = true;
                break;
            }
            // Update status every ~10 symbols so the UI shows TX progress.
            if (i == 0 || i % 10 == 0) {
                ft8_status_set("TX [%d/%d] %s", i + 1, FT8_NN, req->display_text);
            }
            float freq = (float)req->audio_freq_hz + (float)req->tones[i] * FT8_TONE_SPACING_HZ;
            sleep_until(t0, (int64_t)i * FT8_SYMBOL_PERIOD_US);
            tx_cmd(t0, "TA%.2f;", (double)freq);
        }

        if (!aborted) {
            // Let the final symbol play out its full 160 ms before keying
            // up - otherwise we'd truncate the last tone for receivers.
            sleep_until(t0, (int64_t)FT8_NN * FT8_SYMBOL_PERIOD_US);
        }
        // Either way - whether all 79 symbols played or we broke out early
        // on an abort request - key up immediately now. This is the part
        // that must ALWAYS run: the radio must never be left transmitting.
        tx_cmd(t0, "TA%.0f;", (double)FT8_TX_KEYUP_TONE_HZ);
        vTaskDelay(pdMS_TO_TICKS(FT8_TX_ENVELOPE_SETTLE_MS));

        // Query power/SWR while still keyed - SW; returns no reading once
        // back in Receive mode. Do this for both normal and aborted bursts.
        float power_w = -1.0f, swr = -1.0f;
#if FT8_TX_SEND_LIVE
        esp_err_t pswr_err = cat_query_power_swr(&power_w, &swr);
        ESP_LOGI(TAG, "post-burst PC/SW query: err=0x%x power=%.1f swr=%.2f",
                 pswr_err, (double)power_w, (double)swr);
        if (power_w >= 0.0f && swr >= 0.0f) {
            ESP_LOGI(TAG, "TX power=%.1fW SWR=%.2f", (double)power_w, (double)swr);
            s_last_power_w = power_w;
            s_last_swr = swr;
            s_last_pwr_swr_us = esp_timer_get_time();
        }
#endif

        tx_cmd(t0, "RX;");

#if FT8_TX_SEND_LIVE
        if (power_w >= 0.0f && swr > 4.0f) {
            ESP_LOGW(TAG, "SWR protection trip (SWR=%.2f) — cycling TX/RX to clear latch",
                     (double)swr);
            cat_send_raw_cmd("TX;");
            vTaskDelay(pdMS_TO_TICKS(150));
            cat_send_raw_cmd("RX;");
            ESP_LOGI(TAG, "SWR latch clear cycle done");
        }
#endif

        cat_poll_set_paused(false);

        ESP_LOGI(TAG, "TX burst %s: '%s' (%lld ms on-air)",
                 aborted ? "ABORTED" : "complete", req->display_text,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }

    lock();
    s_state = FT8_TX_IDLE;
    memset(&s_armed, 0, sizeof(s_armed));
    unlock();
    s_abort_requested = false;
}
