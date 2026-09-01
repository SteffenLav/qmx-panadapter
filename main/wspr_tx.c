// WSPR TX core - single burst, CAT-driven. See wspr_tx.h for the design
// overview and docs/wspr-scope.md for the CAT sequence rationale (same
// TA; "Transmit Audio" technique as ft8_tx.c).

#include "wspr_tx.h"
#include "wspr_proto.h"
#include "wspr_fano.h"

#include <string.h>
#include <math.h>
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
// reasoning). This module HAS keyed a real radio and been spotted worldwide;
// see the note on WSPR_TX_SEND_LIVE below for when that stopped being a
// local-only experiment.
/* ⭐ LIVE as of the 2026-08-28 launch. It was 0 - a dry run that logged a
 * perfect burst and sent zero bytes - which was right while WSPR shipped dark
 * and unreachable. It is the wrong default the moment the page is in the swipe
 * cycle with a TX switch on it: the operator would arm TX, watch the burst run,
 * and never be spotted by anybody. A control that reports success and does
 * nothing is the worst outcome available.
 *
 * The on-air proof came from commit 3db70a8 ("ON THE AIR - 50 spots worldwide,
 * zero drift, all fields correct"), which was built with this flipped locally
 * while the committed source stayed at 0 - so the evidence and the default
 * disagreed for weeks without anyone noticing.
 *
 * What still stands between this and an unexpected transmission, all of which
 * predate this change: wspr_tx_en defaults OFF, the callsign and grid must be
 * set or the slot loop refuses, simulation mode interlocks every byte, and
 * tx_cmd_critical() retries the stop-transmit rather than leaving a radio
 * keyed. */
#ifndef WSPR_TX_SEND_LIVE
#define WSPR_TX_SEND_LIVE 1
#endif

/* SIMULATION INTERLOCK. ft8_tx.c carries the identical check and for the
 * identical reason: with simulation on, a burst must not put one byte on the
 * CAT link, even though the radio may be connected and perfectly willing. The
 * check reads the SETTING on every call rather than a cached copy, so switching
 * simulation on takes effect immediately rather than at the next boot. */
static bool sim_active(void)
{
    qmx_settings_t s;
    settings_load_all(&s);
    return s.sim_mode_en;
}

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
/* Latched once at the top of run_burst(), not read per symbol: settings_load_all()
 * 162 times a burst would be absurd, and the answer cannot meaningfully change
 * inside 110 seconds. */
static bool              s_burst_sim = false;

/* Last MEASURED output of a WSPR burst - the radio's own PC;/SW; answer, not
 * the declared figure. The Tab5 cannot know what the radio delivers, so it
 * asks while keyed. -1 until a burst has reported one. */
static float             s_last_power_w = -1.0f;
static float             s_last_swr     = -1.0f;

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
    if (minute_is_even && sec_in_minute <= WSPR_TX_LATE_GRACE_S) {
        /* ⭐ A GRACE WINDOW, NOT AN EXACT SECOND (Dirk, 2026-09-01).
         *
         * This used to require `sec_in_minute == 0` - a ONE-SECOND window -
         * and wspr_rx.c's loop cannot reliably hit it: between the cycle
         * boundary and the arm it does settings_load_all(), the PA guard
         * (which can issue MM writes and MU; over CAT) and two status polls.
         * Miss by a single second and the burst was scheduled for the NEXT
         * even minute, 119 s away, with the receiver stood down for the whole
         * wait - so the waterfall died for TWO cycles instead of one.
         *
         * MEASURED on the dev bench, not reasoned: of seven real arms,
         * 2 x (0 s), 1 x (40 s) and FOUR x (119 s). More than half of all
         * transmissions cost an entire extra receive cycle.
         *
         * The grace is bounded at 2 s by the operator, and it is spent where
         * it is affordable: run_burst() delays only the REMAINDER of
         * WSPR_TX_START_OFFSET_MS, so a late arm still fires as close to +1 s
         * as it can and never later than the grace itself. The five reference
         * stations in WSJT's own capture start at 1.109-2.133 s, so a burst
         * inside this window is still within the population every receiver is
         * already searching around. */
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
    if (s_burst_sim) {   /* simulation: the radio must not hear a thing */
        ESP_LOGI(TAG, "[SIM t+%6lldus] %s", (long long)(esp_timer_get_time() - t0), fmt_freq);
        return;
    }
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
    if (s_burst_sim) {
        ESP_LOGI(TAG, "[SIM t+%6lldus] %s", (long long)(esp_timer_get_time() - t0), cmd);
        return true;
    }
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
/* WSPR declares power as one of a fixed set of dBm steps. This is the WHOLE
 * protocol set, not our shorter menu list - the advice has to be able to name
 * 37 dBm (5 W) if that is genuinely what went out, or it is not advice. */
static const int kWsprDbmSteps[] = { 0, 3, 7, 10, 13, 17, 20, 23, 27, 30,
                                     33, 37, 40, 43, 47, 50, 53, 57, 60 };

/* Nearest legal declaration for a measured power. -1 if nothing measured yet.
 *
 * ⭐ THIS IS THE POINT OF MEASURING. Declared power is published worldwide with
 * every spot and other operators reason from it, but the Tab5 cannot know what
 * the radio delivers - so it was a dropdown, i.e. a guess the operator had to
 * maintain by hand. It stayed wrong in both directions on the bench: 27 dBm
 * declared while transmitting 5.4 W (37 dBm), then 27 declared while the guard
 * held it to 1.6 W (32 dBm). PC; answers the question the dropdown was asking. */
int wspr_tx_advised_dbm(void)
{
    if (s_last_power_w <= 0.0f) return -1;
    float dbm = 10.0f * log10f(s_last_power_w * 1000.0f);
    int best = kWsprDbmSteps[0];
    float bestd = 1e9f;
    for (unsigned i = 0; i < sizeof(kWsprDbmSteps)/sizeof(kWsprDbmSteps[0]); i++) {
        float d = dbm - (float)kWsprDbmSteps[i];
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = kWsprDbmSteps[i]; }
    }
    return best;
}

/* Last measured burst output. Returns false until a burst has reported one. */
bool wspr_tx_get_last_power_swr(float *power_w, float *swr)
{
    if (s_last_power_w < 0.0f) return false;
    if (power_w) *power_w = s_last_power_w;
    if (swr)     *swr     = s_last_swr;
    return true;
}

static void run_burst(const wspr_tx_request_t *req)
{
    s_abort_requested = false;
    s_burst_sim = sim_active();
    /* ⭐ SELF-LABELLING. The PA voltage in force goes in the burst's OWN start
     * line, because attributing a burst to a voltage AFTERWARDS from separate
     * log lines is exactly what went wrong on 2026-08-29: three bursts, two
     * firmwares and one supply-current reading that got filed against the wrong
     * one. A measurement that cannot be attributed with certainty is not a
     * measurement. -1 means the radio has not told us yet. */
    int16_t pa_x10 = cat_get_pa_voltage_x10();
    ESP_LOGW(TAG, "WSPR TX burst starting: '%s' '%s' %d dBm declared, base=%d Hz, "
                  "PA=%d.%d V%s",
             req->callsign, req->grid, req->power_dbm, req->audio_freq_hz,
             pa_x10 > 0 ? pa_x10 / 10 : 0, pa_x10 > 0 ? pa_x10 % 10 : 0,
             s_burst_sim      ? "  [SIMULATION - radio not keyed]"
             : WSPR_TX_SEND_LIVE ? "" : "  [DRY RUN - logging only, radio not keyed]");
    if (pa_x10 <= 0) ESP_LOGW(TAG, "  ...PA voltage UNKNOWN - this burst cannot be attributed");

    // Exclusive use of the CDC-ACM link for the whole burst - see
    // ft8_tx.c's identical reasoning. Cooperative flag only, never
    // vTaskSuspend.
    if (!s_burst_sim) cat_poll_set_paused(true);

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

        /* MEASURE what actually goes out, mid-burst while the radio is keyed -
         * SW; reads nothing once back in Receive. The async pair is used, not
         * cat_query_power_swr(), whose ~600 ms blocking wait would overrun the
         * symbol clock; a WSPR symbol is 683 ms, so send and read are placed
         * several symbols apart with room to spare.
         *
         * This is the ground truth the declared-power dropdown can never be:
         * the Tab5 cannot know what the radio delivers, so it asks. */
        if (!s_burst_sim && WSPR_TX_SEND_LIVE) {
            if (i == 12) {
                cat_pwr_swr_async_send();
            } else if (i == 16) {
                float pw = -1.0f, sw = -1.0f;
                if (cat_pwr_swr_async_read(&pw, &sw) == ESP_OK &&
                    pw >= 0.0f && sw >= 0.0f) {
                    s_last_power_w = pw;
                    s_last_swr     = sw;
                    ESP_LOGW(TAG, "WSPR TX MEASURED: %.1f W, SWR %.2f  (PA=%d.%d V, "
                                  "declared %d dBm)",
                             (double)pw, (double)sw,
                             pa_x10 > 0 ? pa_x10 / 10 : 0, pa_x10 > 0 ? pa_x10 % 10 : 0,
                             req->power_dbm);
                    /* Say so when the claim and the measurement disagree by more
                     * than one step. ADVISORY only - the declared figure is a
                     * statement about the operator's station and stays theirs to
                     * make; this just stops it being a guess nobody can check. */
                    int adv = wspr_tx_advised_dbm();
                    if (adv >= 0 && (adv - req->power_dbm > 2 || req->power_dbm - adv > 2)) {
                        ESP_LOGW(TAG, "WSPR declared power looks wrong: declaring %d dBm, "
                                      "measured %.1f W = %d dBm. wsprnet publishes the "
                                      "declared figure worldwide.",
                                 req->power_dbm, (double)pw, adv);
                    }
                } else {
                    ESP_LOGW(TAG, "WSPR TX: no PC/SW reading this burst");
                }
            }
        }
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

    if (!s_burst_sim) cat_poll_set_paused(false);
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
            psram_task_park();
            return;
        }
        int secs = wspr_tx_seconds_until_next_slot();
        if (secs <= 0) break;
        vTaskDelay(pdMS_TO_TICKS(secs * 1000 > WSPR_TX_WAIT_POLL_MS ? WSPR_TX_WAIT_POLL_MS
                                                                     : (uint32_t)secs * 1000));
    }

    // A WSPR transmission does not begin AT the even minute - it begins one
    // second into it. 110.6 s of signal sits inside a 120 s window and the
    // convention puts the slack mostly at the end.
    //
    // MEASURED rather than recalled, because getting it wrong is invisible from
    // this side - we would transmit perfectly and simply be early. The five real
    // stations in WSJT's own reference capture
    // (test/wav_reference/wspr/150426_0918.wav, recorded from the even minute)
    // start at 1.109, 1.515, 1.621, 1.813 and 2.133 s: a clear floor at ~1.1 s
    // with each station's own clock error above it. Firing at :00 would put us
    // ~1.6 s ahead of the population every receiver is searching around, which
    // spends decode margin for nothing.
    /* Delay only the REMAINDER of the offset. The old fixed delay was correct
     * only when the wait above ended exactly on the even minute; with the grace
     * window it can end up to WSPR_TX_LATE_GRACE_S late, and adding a further
     * full second on top would put the burst at +3 s - past the whole reference
     * population instead of inside it. Already past +1 s means fire now. */
    {
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        int64_t into_min_ms = (int64_t)(tv_now.tv_sec % 60) * 1000
                            + tv_now.tv_usec / 1000;
        int64_t remain = (int64_t)WSPR_TX_START_OFFSET_MS - into_min_ms;
        if (remain > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)remain));
        } else if (into_min_ms > WSPR_TX_START_OFFSET_MS) {
            ESP_LOGW(TAG, "burst starting %lld ms into the minute (%lld ms later "
                          "than the +%d ms convention) - inside the %d s grace",
                     (long long)into_min_ms,
                     (long long)(into_min_ms - WSPR_TX_START_OFFSET_MS),
                     WSPR_TX_START_OFFSET_MS, WSPR_TX_LATE_GRACE_S);
        }
    }

    lock();
    s_state = WSPR_TX_ACTIVE;
    unlock();

    run_burst(&req);

    lock();
    s_state = WSPR_TX_IDLE;
    unlock();
    psram_task_park();
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

    // In a DRY-RUN build, a missing radio is not a reason to refuse. tx_cmd()
    // sends zero bytes when WSPR_TX_SEND_LIVE is 0, so the burst cannot key
    // anything, cannot mis-set a mode, and cannot reach the radio at all - the
    // Digi pre-flight below is guarding an action that will not happen. Left
    // strict, it made the engine untestable in exactly the situation where
    // bench time is cheapest: this Tab5 wedges its QMX on every reflash (#74),
    // so after any firmware change there is no radio until someone power-cycles
    // it by hand, and the timing work that needs neither radio nor antenna was
    // blocked behind that.
    //
    // A LIVE build keeps the check unconditionally. There the pre-flight is the
    // real thing - it is what stops a burst going out in the wrong mode.
    bool preflight_required = true;
    if (sim_active()) {
        ESP_LOGI(TAG, "arm: SIMULATION - skipping the Digi pre-flight, nothing will be sent");
        preflight_required = false;
    }
#if !WSPR_TX_SEND_LIVE
    if (!cat_is_ready()) {
        ESP_LOGW(TAG, "arm: no CAT link - allowing anyway, this is a DRY RUN "
                      "(nothing is sent to the radio; a live build would refuse)");
        preflight_required = false;
    }
#endif

    // Digi-mode pre-flight - see ft8_tx_arm()'s identical reasoning: check/
    // switch happens here, with up to ~110 s of lead time, never at burst
    // time where any delay would shift the start off the slot boundary.
    if (preflight_required) {
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

    // Priority 5, NOT tskIDLE_PRIORITY + 1. This task is asleep 99.85% of the
    // time - it wants ~1 ms of CPU once every 682 ms to send one TA; - but the
    // instant it wants it, it must have it: a late wake is a tone transition
    // landing inside the previous symbol, and nothing downstream can undo that.
    //
    // Measured, first hardware dry run (2026-08-23, 162 symbols, with FT8
    // capturing and decoding concurrently, i.e. worst case): at
    // tskIDLE_PRIORITY + 1 the worker sat below fft_task (4), cat_poll and
    // cat_link (5) and audio_task (6), and 15 of 162 symbols went out late -
    // 7 of them by more than 30 ms, worst 66 ms, each one coinciding with an
    // ordinary FT8-capture or USB-transport slice. That is the same
    // lowest-priority starvation CLAUDE.md's #199 note describes.
    //
    // 5 puts it with the CAT tasks, which is where it belongs - it IS a CAT
    // burst, and cat_poll is paused for its whole duration anyway. It stays
    // strictly BELOW audio_task (6) so the USB isochronous pump keeps its
    // margin: see #51, where losing that margin cost 170-350 ms of audio a
    // slot, silently, at the wire.
    /* The previous transmission's worker parked; free its stack before we ask
     * for another (#279). Small - 4 KB - but it is once per transmission, and
     * WSPR transmits all day. */
    psram_task_reap();
    TaskHandle_t h = psram_task_create_reapable(wspr_tx_worker_task, "wspr_tx", 4096, NULL,
                                        5, tskNO_AFFINITY);
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
