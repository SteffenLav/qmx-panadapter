#pragma once

#include "esp_err.h"

/**
 * @brief Initialize CAT subsystem.
 *
 * Phase 2.3: USB Host + CDC-ACM to QMX, polls FA; every 200ms,
 *            updates ui_update_frequency() on change.
 *
 * Phase 3.1: also dumps audio class descriptors on first connection.
 */
esp_err_t cat_init(void);

/**
 * @brief Send a frequency-set command (FA; in Kenwood/Elecraft protocol) to the QMX.
 *
 * Rate-limited internally — calls within ~200 ms of the previous one are dropped.
 * The QMX will retune; the next CAT poll cycle picks up the new freq and updates UI.
 *
 * @param freq_hz target frequency in Hz (will be sent as 11-digit padded ASCII)
 * @return ESP_OK if queued, ESP_ERR_INVALID_STATE if QMX not connected,
 *         ESP_ERR_TIMEOUT if rate-limited
 */
esp_err_t cat_set_frequency(uint32_t freq_hz);

/**
 * @brief Same as cat_set_frequency() but bypasses the 200 ms rate-limiter.
 *
 * Use for deliberate user actions (e.g. preset taps) where the write must
 * go through even if another write just happened (e.g. sticky-settings restore).
 */
esp_err_t cat_set_frequency_forced(uint32_t freq_hz);

/**
 * @brief Get the most recently observed VFO frequency.
 *
 * Updated by the CAT poll loop on every FA response. Returns 0 before
 * the first poll completes or if the QMX is disconnected.
 *
 * @return frequency in Hz
 */
uint32_t cat_get_frequency(void);
/* Returns current mode string (e.g. "USB", "CW", "DiGi").
 * Returns "" if CAT not ready or no MD response yet. */
const char *cat_get_mode_str(void);

/**
 * @brief Send a mode-set command (MD; in Kenwood/Elecraft protocol) to the QMX.
 *
 * Translates Hamlib mode strings to Kenwood digits:
 *   LSB=1, USB=2, CW=3, FM=4, AM=5, FSK/DiGi/PKTUSB/RTTY/FT8=6,
 *   CW-R/CWR=7, FSK-R/DIGI-R/RTTYR=9.
 * Anything unrecognised returns ESP_ERR_INVALID_ARG.
 *
 * Shares the 200 ms TX rate-limit with cat_set_frequency().
 *
 * @param mode  Hamlib mode string (case-insensitive)
 * @return ESP_OK on send, ESP_ERR_INVALID_ARG on unknown mode,
 *         ESP_ERR_INVALID_STATE if QMX not connected,
 *         ESP_ERR_TIMEOUT if rate-limited.
 */
esp_err_t cat_set_mode(const char *mode);

/**
 * @brief Send a passband-width command (FW; in Kenwood/Elecraft protocol).
 *
 * Width is rounded to nearest Hz and clipped to 4 digits. Shares the
 * 200 ms TX rate-limit.
 *
 * @param hz  passband width in Hz (50 .. 9999)
 * @return ESP_OK on send, ESP_ERR_INVALID_ARG if out of range,
 *         ESP_ERR_INVALID_STATE if QMX not connected,
 *         ESP_ERR_TIMEOUT if rate-limited.
 */
esp_err_t cat_set_passband_hz(uint32_t hz);

#include <stdbool.h>

/**
 * @brief Returns true once the QMX has fully booted on the CAT link.
 *
 * Set when the first FW (passband width) response is parsed, which is
 * the last leg of the CDC-open -> Q9 1; -> FA -> MD -> FW handshake.
 * Empirically this is the earliest moment the QMX is producing clean,
 * mode-correct I/Q on the USB sound card.
 * Cleared on QMX USB disconnect.
 */
bool cat_is_ready(void);
/**
 * @brief Get the CW offset (Hz) read from QMX at connect time.
 *
 * Read once via MMCW|CW offset; after Q9 1; on CDC open.
 * Returns 700 (QMX default) if not yet read or CAT not connected.
 */
int cat_get_cw_offset_hz(void);
/**
 * @brief QMX firmware version string from the VN; query (e.g. "1_03_002QMX").
 * Returns an empty string until the radio has answered VN; after link-up.
 */
const char *cat_get_qmx_fw(void);
/**
 * @brief Check whether the connected QMX's firmware is at least major.minor.patch.
 *
 * Parses cat_get_qmx_fw() (e.g. "1_04_002QMX"). Returns false if no firmware
 * string has been captured yet (not connected, or VN; hasn't answered) - the
 * safe default for gating a 1_04+-only feature. See
 * docs/qmx-1_04-cat-comparison.md.
 */
bool cat_qmx_fw_at_least(int major, int minor, int patch);
/**
 * @brief True once the QMX has confirmed IQ mode is ON (Q9; readback == 1)
 * for the current connection. False if not yet connected or if the
 * Q9 1; / Q9; handshake never confirmed after retrying at link-up -
 * in that state the spectrum will appear mirrored/shifted.
 */
bool cat_get_iq_mode_confirmed(void);
/**
 * @brief True once VOX has been confirmed OFF (Q3; readback == 0) for the
 * current connection. The panadapter keys the QMX via CAT (TX;/TA;/RX;), never
 * by transmit audio, so VOX is unnecessary and is disabled at link-up (Q3 0;,
 * session-only, reverts on QMX power-cycle) to remove any accidental-keying
 * risk from the USB-sound-card SSB TX path. Best-effort: unlike IQ mode, a
 * failure here is non-critical (no on-screen warning) - VOX-on simply can't do
 * harm as long as we never feed TX audio, which we don't.
 */
bool cat_get_vox_disabled(void);
/**
 * @brief Send a raw formatted CAT/MM command string to the QMX.
 * Uses printf-style format. Fire-and-forget, no response parsed.
 */
esp_err_t cat_send_raw_cmd(const char *fmt, ...);

/**
 * @brief Query the QMX's instantaneous power output and SWR (PC; and SW;).
 *
 * Both readings are only valid while the radio is keyed (transmitting) - SW;
 * returns no value in Receive mode. Blocks for the CAT round trip (up to
 * ~400ms total). On success, *power_w and *swr are set to -1.0f if that
 * particular reading was unavailable (e.g. SWR queried while not transmitting).
 *
 * @return ESP_OK if at least one reading was received, ESP_ERR_TIMEOUT if
 *         neither responded, ESP_ERR_INVALID_STATE if CAT is not connected.
 */
esp_err_t cat_query_power_swr(float *power_w, float *swr);

/**
 * @brief Non-blocking power/SWR query for live display during an FT8 TX burst.
 *
 * _send() fires "PC;SW;" without waiting (bounded ~50 ms CDC write); _read()
 * parses whatever response has since arrived (no wait). Used by ft8_tx.c to get
 * a current-burst reading mid-transmission without the ~600 ms blocking wait of
 * cat_query_power_swr(), which would overrun FT8 symbol timing. Call _send()
 * once after the PA settles, _read() several symbols later.
 */
esp_err_t cat_pwr_swr_async_send(void);
esp_err_t cat_pwr_swr_async_read(float *power_w, float *swr);

/**
 * @brief Enable/disable a live power/SWR poll step for QMX SWR Tune mode.
 *
 * When active, the background poll task adds a "PC;SW;" step to its FA/MD/FW
 * rotation so cat_pwr_swr_async_read() has a fresh reading while the radio is
 * transmitting a tune carrier (MD8;, 1_04+ firmware only). Disable as soon as
 * Tune mode is exited so the extra poll step stops.
 */
void cat_tune_poll_set_active(bool active);

/**
 * @brief Request a mode change (deferred to the poll task).
 *
 * Thread-safe to call from the LVGL/UI thread. The MD<digit>; command is
 * queued and sent by the poll task on its next cycle, avoiding a race with
 * the FA/MD/FW poll on the shared CDC pipe. Any subsequent call before the
 * poll drains it overwrites the previous request (last write wins).
 *
 * @param mode  Hamlib mode string (e.g. "USB", "LSB", "CW", "DiGi")
 */
void cat_request_mode(const char *mode);

/**
 * @brief Request the QMX SSB receive filter bandwidth (Hz: 2500/2700/2900/3200).
 *
 * Thread-safe to call from the LVGL/UI thread. The actual MMSSB|Bandwidth=
 * write is deferred to the poll task (which owns the CDC pipe), avoiding a
 * command-interleave race with the FA/MD/FW poll that made BW changes flaky.
 */
void cat_request_ssb_bandwidth(uint32_t hz);

/* QMX AF gain (volume), in the radio's own 0.25 dB steps. Kenwood "AG0nnn;".
 *
 * The QMX shows this on its OWN LCD in decibels - operation manual, "Volume
 * change" parameter: "the new volume is displayed momentarily on the bottom
 * left of the LCD. The volume is shown in decibels." So dB = value / 4, and
 * the drawer slider is in dB so the number on the Tab5 is the same number the
 * radio shows. Do not reintroduce a percentage here.
 *
 * Protocol range is 0-799 = 0-199.75 dB, matching the volume knob's own "0 to
 * 200dB gain" (operation manual, audio chain step 23).
 *
 * CAT_AF_GAIN_DB_MAX is the SLIDER's top, and it is deliberately below that
 * protocol maximum: a 0-199 dB slider put every setting an operator actually
 * wants inside the leftmost couple of centimetres.
 *
 * The number comes from Randy N4OPI, who has the only radio it has been measured
 * against, and it took two rounds to get right - keep both here, because the
 * first round is a lesson about acting on a range description:
 *   1. 2026-07-29, first report: "the usable portion is concentrated in the
 *      first 10% of the range - anything beyond that is way too loud". Read
 *      literally that is ~20 dB, so the cap went to 40.
 *   2. Same day, after using it: 40 dB "is not quite loud enough for weak
 *      signals down at the noise floor or in a noisy environment" on headphones
 *      or an added speaker. He suggested 70. So 70 it is.
 * The lesson: "too loud beyond X" described where the COMFORTABLE listening
 * range ended, not where the useful range did - weak-signal work and a noisy
 * shack both need headroom above comfortable. 70 dB still leaves ~7 px/dB on
 * the drawer's 488 px slider, so the resolution that made this worth capping
 * at all is intact. Do NOT turn the scale back into a percentage (the whole
 * point is that the number matches the radio's own LCD).
 *
 * Deferred to the poll task like the filter writes - a direct cross-thread
 * write races the FA/MD/FW poll and the QMX returns ?;.
 * Requesting 0 is a valid mute, not a no-op. */
#define CAT_AF_GAIN_MAX     799
#define CAT_AF_GAIN_DB_MAX  70    /* slider top in dB; 70 dB -> AG 280 */
void cat_request_af_gain(uint16_t ag);

/* Ask the radio for its current AF gain; the answer lands asynchronously and is
 * readable via cat_get_af_gain(). Used when the settings drawer opens so the
 * slider shows what the RADIO is actually set to (including changes made on its
 * own volume knob) rather than the last value this UI sent. */
void cat_query_af_gain(void);

/* Last AF gain read back from the radio in 0.25 dB steps, or -1 if never read.
 * Divide by 4 for the dB figure the QMX displays. */
int cat_get_af_gain(void);

/*
 * Request a CW filter width (Hz). Deferred to the poll task as
 * "MMCW|CW passband=<hz>;" - the QMX rejects a Kenwood FW<nnnn>; set with ?;,
 * so the menu-manager item is the only thing that works. Thread-safe.
 */
void cat_request_cw_passband(uint32_t hz);

/**
 * @brief Cooperatively pause/resume the background FA;/MD;/FW; poll loop.
 *
 * v0.12.0 (FT8 TX): while a TX burst owns the CDC-ACM link (sending
 * TX;/TA<freq>;/.../RX; at a precise 160 ms cadence), an interleaved poll
 * could desync the burst timing or garble the stream. The poll task checks
 * this flag cooperatively at the top of its loop (a plain vTaskDelay+continue
 * — never vTaskSuspend, which risks deadlocking on the driver's internal
 * mutex if the task is paused mid-transfer). Idempotent; safe to call from
 * any task.
 *
 * @param paused  true to pause polling, false to resume
 */
void cat_poll_set_paused(bool paused);

#define CAT_MAX_BANDS 16

typedef struct {
    char     name[8];      // e.g. "40"
    uint32_t center_hz;    // Frequency center from band config
} cat_band_entry_t;

/**
 * @brief Get the band list read from QMX at connect time.
 * @param out_count  number of valid entries populated
 * @return pointer to static array of cat_band_entry_t
 */
const cat_band_entry_t *cat_get_band_list(int *out_count);

/**
 * @brief Set the QMX's onboard real-time clock (time-of-day only, no date).
 *
 * Sends TM<hh><mm><ss>; (Kenwood/QMX-specific CAT command). Used to keep
 * the QMX RTC in sync with UTC whenever this device has a good time
 * source (SNTP), so the QMX RTC can later serve as a fallback time
 * source for FT8 when there's no WiFi (e.g. POTA).
 *
 * @return ESP_OK on send, ESP_ERR_INVALID_ARG if hour/min/sec out of range,
 *         ESP_ERR_INVALID_STATE if QMX not connected.
 */
esp_err_t cat_set_qmx_time(int hour, int min, int sec);

/**
 * @brief Query the QMX's onboard real-time clock (time-of-day only, no date).
 *
 * Sends "TM;" and blocks briefly (pausing the background poll loop) for
 * the "TMhhmmss;" response. Used as a fallback UTC time-of-day source for
 * FT8 slot alignment when SNTP/WiFi is unavailable.
 *
 * @return ESP_OK and out_hour/out_min/out_sec populated on success,
 *         ESP_ERR_INVALID_STATE if QMX not connected/ready, ESP_FAIL on
 *         a bad/missing response.
 */
esp_err_t cat_query_qmx_time(int *out_hour, int *out_min, int *out_sec);

/**
 * GPS second-tick sync (for a GPS-disciplined QMX+). Rapidly polls TM; and
 * catches the instant the seconds field ticks over - the true GPS second
 * boundary - so the caller can phase-lock the clock to the GPS beat (~+/-one TM
 * round-trip, drift-free) instead of the +/-1 s naive whole-second apply.
 *
 * @param out_flip_us esp_timer_get_time() at the moment the flipping TM
 *        response landed; out_hour/min/sec are the NEW second at that flip.
 * @return ESP_OK on a caught flip; ESP_ERR_INVALID_STATE if not ready or the
 *         CDC pipe is busy (e.g. FT8 TX); ESP_ERR_TIMEOUT if no flip in ~1.3 s.
 */
esp_err_t cat_gps_tick_sync(int *out_hour, int *out_min, int *out_sec, int64_t *out_flip_us);
