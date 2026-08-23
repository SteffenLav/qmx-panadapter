// WSPR TX core - single burst, CAT-driven. See wspr_tx.h for the design
// overview and docs/wspr-scope.md for the CAT sequence rationale (same
// TA; "Transmit Audio" technique as ft8_tx.c).

#include "wspr_tx.h"
#include "wspr_proto.h"
#include "wspr_fano.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "cat/cat.h"
#include "storage/settings.h"
#include "util/psram_task.h"

static const char *TAG = "wspr_tx";

// Compile-time safety switch, same purpose and same default-OFF posture as
// ft8_tx.c's FT8_TX_SEND_LIVE (see that file's comment for the full
// reasoning - the short version: this new module has not yet keyed a real
// radio even once, so it stays a dry run - full timing, full CAT-sequence
// logging, zero bytes actually sent - until several burst cycles have been
// visually verified in the logs on real hardware). Flip to 1 only
// deliberately, never as a side effect of another change.
#ifndef WSPR_TX_SEND_LIVE
#define WSPR_TX_SEND_LIVE 0
#endif

#define WSPR_SYMBOL_PERIOD_US   682667  // 8192/12000 s, in microseconds
#define WSPR_TONE_SPACING_HZ    1.46484375f
#define WSPR_TX_KEYUP_TONE_HZ   0.0f    // "any value < 10 Hz" keys up (CAT manual)
#define WSPR_TX_ENVELOPE_SETTLE_MS  5
#define WSPR_TX_STOP_RETRIES     8      // mirrors ft8_tx.c's tx_cmd_critical()
#define WSPR_TX_MODE_POLL_MS    100
#define WSPR_TX_MODE_POLL_TRIES  10     // ~1s worst case
#define WSPR_TX_WAIT_POLL_MS    500     // how often the ARMED wait loop checks for disarm

static SemaphoreHandle_t s_lock = NULL;
static wspr_tx_state_t   s_state = WSPR_TX_IDLE;
static wspr_tx_request_t s_armed;
static volatile bool     s_disarm_requested = false;
static volatile bool     s_abort_requested  = false;

void wspr_tx_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static inline void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

bool wspr_tx_build_request(const char *callsign, const char *grid,
                            int power_dbm, int audio_freq_hz,
                            wspr_tx_request_t *out_req,
                            char *out_err, size_t out_err_len)
{
    if (out_err && out_err_len) out_err[0] = '\0';
    if (!callsign || !grid || !out_req) {
        if (out_err) snprintf(out_err, out_err_len, "Internal error: missing argument");
        return false;
    }
    char grid4[5];
    snprintf(grid4, sizeof(grid4), "%.4s", grid); // WSPR's grid field is 4 chars; a
                                                    // longer (6-char) setting is truncated

    wspr_msg_bytes_t msg;
    if (!wspr_pack_message(callsign, grid4, power_dbm, &msg)) {
        if (out_err) snprintf(out_err, out_err_len,
                              "Can't encode '%s'/'%s'/%d dBm - check callsign/grid format",
                              callsign, grid4, power_dbm);
        return false;
    }

    uint8_t raw[WSPR_NSYM], channel[WSPR_NSYM];
    wspr_convolve_encode(&msg, raw);
    wspr_interleave(raw, channel);
    wspr_symbols_to_tones(channel, out_req->tones);

    strncpy(out_req->callsign, callsign, sizeof(out_req->callsign) - 1);
    out_req->callsign[sizeof(out_req->callsign) - 1] = '\0';
    strncpy(out_req->grid, grid4, sizeof(out_req->grid) - 1);
    out_req->grid[sizeof(out_req->grid) - 1] = '\0';
    out_req->power_dbm = power_dbm;
    out_req->audio_freq_hz = (audio_freq_hz >= WSPR_TX_TONE_MIN_HZ
                               && audio_freq_hz <= WSPR_TX_TONE_MAX_HZ)
                                  ? audio_freq_hz : WSPR_TX_DEFAULT_FREQ_HZ;
    return true;
}

int wspr_tx_seconds_until_next_slot(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    int sec_in_minute = (int)(now % 60);
    int minute_is_even = ((now / 60) % 2) == 0;
    int secs;
    if (minute_is_even && sec_in_minute == 0) {
        secs = 0;
    } else if (minute_is_even) {
        // this minute is even but already started - next even minute is 2 away
        secs = (120 - sec_in_minute);
    } else {
        // odd minute - next even minute starts at the top of the next minute
        secs = (60 - sec_in_minute);
    }
    return secs;
}

// ---- CAT send helpers - mirror ft8_tx.c's tx_cmd()/tx_cmd_critical() ----

static void tx_cmd(int64_t t0, const char *fmt_freq)
{
#if WSPR_TX_SEND_LIVE
    esp_err_t err = cat_send_raw_cmd("%s", fmt_freq);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send failed (0x%x): %s - continuing burst (radio may be disconnected)",
                 err, fmt_freq);
    }
#else
    ESP_LOGI(TAG, "[DRY RUN t+%6lldus] %s", (long long)(esp_timer_get_time() - t0), fmt_freq);
#endif
}

// The one CAT write whose failure has a physical consequence - see
// ft8_tx.c's tx_cmd_critical() for the full reasoning (Roy KI0ER, #? - a
// dropped stop-transmit command with no retry left a radio keyed until a
// manual power cycle). Retries hard and hands off to the CAT layer's own
// force-RX reassert if it never gets through.
static bool tx_cmd_critical(int64_t t0, const char *cmd)
{
#if WSPR_TX_SEND_LIVE
    for (int i = 0; i < WSPR_TX_STOP_RETRIES; i++) {
        esp_err_t err = cat_send_raw_cmd("%s", cmd);
        if (err == ESP_OK) {
            if (i) ESP_LOGW(TAG, "%s succeeded on attempt %d - radio is back in receive", cmd, i + 1);
            return true;
        }
        ESP_LOGW(TAG, "%s FAILED (0x%x), attempt %d/%d - retrying", cmd, err, i + 1, WSPR_TX_STOP_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "WSPR TX: %s NEVER GOT THROUGH - handing to CAT to re-assert", cmd);
    cat_request_force_rx();
    return false;
#else
    ESP_LOGI(TAG, "[DRY RUN t+%6lldus] %s", (long long)(esp_timer_get_time() - t0), cmd);
    return true;
#endif
}

static void sleep_until(int64_t t0, int64_t offset_us)
{
    int64_t target = t0 + offset_us;
    int64_t now = esp_timer_get_time();
    if (target > now) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((target - now) / 1000)));
    }
}

// Runs the actual ~110.6 s CAT burst. Called from the worker task once the
// even-minute boundary has arrived and the state is already ACTIVE.
static void run_burst(const wspr_tx_request_t *req)
{
    s_abort_requested = false;
    ESP_LOGI(TAG, "WSPR TX burst starting: '%s' '%s' %d dBm, base=%d Hz%s",
             req->callsign, req->grid, req->power_dbm, req->audio_freq_hz,
             WSPR_TX_SEND_LIVE ? "" : "  [DRY RUN - logging only, radio not keyed]");

    // Exclusive use of the CDC-ACM link for the whole burst - see
    // ft8_tx.c's identical reasoning. Cooperative flag only, never
    // vTaskSuspend.
    cat_poll_set_paused(true);

    int64_t t0 = esp_timer_get_time();
    sleep_until(t0, 0);
    tx_cmd(t0, "TX;");

    bool aborted = false;
    for (int i = 0; i < WSPR_NSYM; i++) {
        if (s_abort_requested) {
            ESP_LOGW(TAG, "WSPR TX abort requested at symbol %d/%d - keying up now", i, WSPR_NSYM);
            aborted = true;
            break;
        }
        if (i == 0 || i % 20 == 0) {
            ESP_LOGI(TAG, "WSPR TX [%d/%d]", i + 1, WSPR_NSYM);
        }
        float freq = (float)req->audio_freq_hz + (float)req->tones[i] * WSPR_TONE_SPACING_HZ;
        sleep_until(t0, (int64_t)i * WSPR_SYMBOL_PERIOD_US);
        char buf[32];
        snprintf(buf, sizeof(buf), "TA%.2f;", (double)freq);
        tx_cmd(t0, buf);
    }

    if (!aborted) {
        sleep_until(t0, (int64_t)WSPR_NSYM * WSPR_SYMBOL_PERIOD_US);
    }
    // Must ALWAYS run: the radio must never be left transmitting.
    {
        char keyup[24];
        snprintf(keyup, sizeof(keyup), "TA%.0f;", (double)WSPR_TX_KEYUP_TONE_HZ);
        tx_cmd_critical(t0, keyup);
    }
    vTaskDelay(pdMS_TO_TICKS(WSPR_TX_ENVELOPE_SETTLE_MS));
    tx_cmd_critical(t0, "RX;");

    cat_poll_set_paused(false);
    ESP_LOGI(TAG, "WSPR TX burst %s (%.1f s)", aborted ? "ABORTED" : "complete",
             (double)(esp_timer_get_time() - t0) / 1e6);
}

static void wspr_tx_worker_task(void *arg)
{
    (void)arg;
    wspr_tx_request_t req;
    lock();
    req = s_armed;
    unlock();

    // Wait for the next even-minute boundary, checking for a disarm every
    // WSPR_TX_WAIT_POLL_MS so cancelling doesn't take up to 120 s to notice.
    for (;;) {
        if (s_disarm_requested) {
            ESP_LOGI(TAG, "WSPR TX disarmed before the slot boundary");
            lock();
            s_state = WSPR_TX_IDLE;
            s_disarm_requested = false;
            unlock();
            vTaskDelete(NULL);
            return;
        }
        int secs = wspr_tx_seconds_until_next_slot();
        if (secs <= 0) break;
        vTaskDelay(pdMS_TO_TICKS(secs * 1000 > WSPR_TX_WAIT_POLL_MS ? WSPR_TX_WAIT_POLL_MS
                                                                     : (uint32_t)secs * 1000));
    }

    lock();
    s_state = WSPR_TX_ACTIVE;
    unlock();

    run_burst(&req);

    lock();
    s_state = WSPR_TX_IDLE;
    unlock();
    vTaskDelete(NULL);
}

bool wspr_tx_arm(const wspr_tx_request_t *req, char *out_err, size_t out_err_len)
{
    if (out_err && out_err_len) out_err[0] = '\0';
    if (!req) return false;

    lock();
    bool busy = (s_state != WSPR_TX_IDLE);
    unlock();
    if (busy) {
        if (out_err) snprintf(out_err, out_err_len, "A WSPR transmission is already armed/active");
        return false;
    }

    if (cat_user_pause_active()) {
        if (out_err) snprintf(out_err, out_err_len, "Radio released - take it back first");
        return false;
    }

    // Digi-mode pre-flight - see ft8_tx_arm()'s identical reasoning: check/
    // switch happens here, with up to ~110 s of lead time, never at burst
    // time where any delay would shift the start off the slot boundary.
    const char *mode = cat_get_mode_str();
    if (strcmp(mode, "DiGi") != 0) {
        ESP_LOGI(TAG, "arm: QMX mode is '%s' - switching to Digi...", mode);
        cat_set_mode("FT8"); // hamlib_mode_to_digit() maps this to digit '6' = DiGi
        bool confirmed = false;
        for (int i = 0; i < WSPR_TX_MODE_POLL_TRIES; i++) {
            vTaskDelay(pdMS_TO_TICKS(WSPR_TX_MODE_POLL_MS));
            if (strcmp(cat_get_mode_str(), "DiGi") == 0) { confirmed = true; break; }
        }
        if (!confirmed) {
            ESP_LOGW(TAG, "arm: QMX would not confirm Digi mode (still '%s')", cat_get_mode_str());
            if (out_err) snprintf(out_err, out_err_len, "QMX won't switch to Digi mode - check the radio");
            return false;
        }
        ESP_LOGI(TAG, "arm: QMX confirmed Digi mode");
    }

    lock();
    if (s_state != WSPR_TX_IDLE) {
        unlock();
        if (out_err) snprintf(out_err, out_err_len, "A WSPR transmission is already armed/active");
        return false;
    }
    s_armed = *req;
    s_state = WSPR_TX_ARMED;
    s_disarm_requested = false;
    unlock();

    TaskHandle_t h = psram_task_create(wspr_tx_worker_task, "wspr_tx", 4096, NULL,
                                        tskIDLE_PRIORITY + 1, tskNO_AFFINITY);
    if (!h) {
        lock();
        s_state = WSPR_TX_IDLE;
        unlock();
        if (out_err) snprintf(out_err, out_err_len, "Failed to start WSPR TX worker task");
        return false;
    }

    ESP_LOGI(TAG, "ARMED: '%s' '%s' %d dBm - fires at the next even UTC minute (%d s)",
             req->callsign, req->grid, req->power_dbm, wspr_tx_seconds_until_next_slot());
    return true;
}

bool wspr_tx_send_live_build(void)
{
    return WSPR_TX_SEND_LIVE ? true : false;
}

void wspr_tx_disarm(void)
{
    lock();
    if (s_state == WSPR_TX_ARMED) s_disarm_requested = true;
    unlock();
}

void wspr_tx_request_abort(void)
{
    s_abort_requested = true;
}

wspr_tx_state_t wspr_tx_get_status(char *text, size_t text_len, int *secs_until)
{
    lock();
    wspr_tx_state_t st = s_state;
    if (text && text_len) {
        if (st == WSPR_TX_IDLE) {
            text[0] = '\0';
        } else {
            snprintf(text, text_len, "%s %s dBm=%d", s_armed.callsign, s_armed.grid, s_armed.power_dbm);
        }
    }
    unlock();
    if (secs_until) *secs_until = (st == WSPR_TX_ARMED) ? wspr_tx_seconds_until_next_slot() : 0;
    return st;
}
