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

// True while WE are holding the radio in split for the CW transmit offset.
// RIT is refused while this is true - the two are mutually exclusive, since the
// offset is implemented as split (the QMX has no XIT) and RIT would move the
// receiver as well. Clearing RIT to zero is always allowed.
bool cat_cw_tx_offset_engaged(void);

// How many CDC (virtual COM) interfaces this QMX exposes: 1, 2 or 3, or -1 if no
// radio is open. Read-only - it opens and immediately closes the extra ones and
// writes nothing to the radio.
//
// Matters because the QMX can be configured for three ports so a terminal
// session can run alongside CAT. If there is a second interface, a Tab5 terminal
// can own it and never disturb the CAT poll.
int cat_probe_extra_cdc_ports(void);

// Open the QMX's SECOND serial port (interface 5), send Enter, and hex-dump what
// comes back to the log. Returns the byte count, or -1 if the port would not
// open. CAT on port 1 is untouched throughout.
//
// Answers the one question left before a Tab5 terminal can be designed: is the
// stream ANSI/VT100 escape sequences or plain re-sent lines?
int cat_probe_terminal(void);
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
 *      or an added speaker. He suggested 70. So 70 it was, briefly.
 *   3. 2026-07-30: Randy retracted round 2 - he had been listening with the
 *      antenna switched OFF, so the "not loud enough" was the missing band
 *      noise, not a missing gain range. With the antenna on, "40 seems plenty
 *      loud now. Maybe 50?" - so 50, splitting his two with-antenna reports.
 * The lesson from round 1 stands: "too loud beyond X" described where the
 * COMFORTABLE listening range ended, not where the useful range did. Round 3
 * adds its own: ask what the antenna was doing before acting on a loudness
 * report. 50 dB gives ~10 px/dB on the drawer's 488 px slider. Do NOT turn
 * the scale back into a percentage (the whole point is that the number
 * matches the radio's own LCD).
 *
 * Deferred to the poll task like the filter writes - a direct cross-thread
 * write races the FA/MD/FW poll and the QMX returns ?;.
 * Requesting 0 is a valid mute, not a no-op. */
#define CAT_AF_GAIN_MAX     799
#define CAT_AF_GAIN_DB_MAX  50    /* slider top in dB; 50 dB -> AG 200 */
void cat_request_af_gain(uint16_t ag);

/* Ask the radio for its current AF gain; the answer lands asynchronously and is
 * readable via cat_get_af_gain(). Used when the settings drawer opens so the
 * slider shows what the RADIO is actually set to (including changes made on its
 * own volume knob) rather than the last value this UI sent. */
void cat_query_af_gain(void);

/* Last AF gain read back from the radio in 0.25 dB steps, or -1 if never read.
 * Divide by 4 for the dB figure the QMX displays. */
int cat_get_af_gain(void);

/* ---- RF gain (RG), Stan's suggestion via Samuel W7STF, 2026-08-07 ----------
 *
 * The QMX's per-band "RF gain (dB)" from its Band Configuration screen, exposed
 * over CAT as RG in BOTH 1_03 and 1_04 - no firmware gate needed. Range 0-99,
 * default 54 (operation manual: "RF gain (dB): 54 is the default. Valid values
 * for the parameter are 0 to 99"). Get answers "RG063" for 63 dB.
 *
 * Two things this is NOT:
 *  - not AF gain: AG is the volume in 0.25 dB steps, RG is plain dB. The
 *    commands look alike and are scaled differently.
 *  - not session-only: unlike Q9/Q3, the CAT manual does not mark RG as
 *    "current operating session only", and the same value is editable in the
 *    Band Configuration terminal app - so a write here changes the operator's
 *    stored per-band configuration. That is why the drawer commits on slider
 *    RELEASE rather than on every tick of a drag.
 *
 * It also moves the noise floor the panadapter is calibrated against, so a
 * change re-seeds flat mode's floor (see the drawer callback). */
#define CAT_RF_GAIN_DB_MAX  99
void cat_request_rf_gain(uint8_t db);

// ---- RIT (receiver incremental tuning) -------------------------------------
//
// Move the RECEIVE frequency without moving transmit - the thing you want when a
// caller is answering you slightly off your frequency (Roy KI0ER: "a Sasquatch
// tone below my centre frequency or a mosquito above me"). Positive = receive
// HIGHER. Real RIT, present in both 1_03 and 1_04, so unlike the CW transmit
// offset this needs no split trickery and no firmware gate.
//
// Writes are deferred to the poll task like every other CAT write, and the
// sequence is deliberately RC; then RU/RD - see cat.c, because RU/RD alone are
// absolute or relative depending on a QMX menu setting we cannot read.
//
// cat_get_rit_hz() returns what we last commanded, not a reading from the radio:
// the display needs it every frame (see ui_get_if_offset_hz), and it is our own
// value, so polling for it would be slower and would add a fifth competitor for
// this pipe.
#define CAT_RIT_MAX_HZ  500   /* plenty for pulling in an off-frequency caller */
void cat_request_rit_hz(int hz);
int  cat_get_rit_hz(void);

/* Ask the radio for the active band's RF gain; answer lands asynchronously in
 * cat_get_rf_gain(). Same drawer-open read-back reasoning as AF gain, with an
 * extra reason: RF gain is PER BAND, so the stored value goes stale the moment
 * the operator changes band. */
void cat_query_rf_gain(void);

/* Last RF gain read back from the radio in dB, or -1 if never read. */
int cat_get_rf_gain(void);

/* ---- Operator pause: release the radio -------------------------------------
 *
 * Stops all CAT traffic to the QMX so the operator can use the radio's own menu
 * or its Terminal Applications (Band Configuration), which speak over this same
 * CDC pipe - our 50 ms FA/MD/FW poll otherwise lands in the middle of whatever
 * they are doing. Also stands down the dead-stream and stuck-decode watchdogs,
 * which would otherwise read a deliberate menu visit as a fault and eventually
 * power-cycle the USB port under the operator's hands.
 *
 * Deliberately separate from cat_poll_set_paused(): that flag belongs to the
 * FT8 TX burst and is cleared at the end of every burst, which would silently
 * cancel the operator's pause.
 *
 * Resuming re-runs the IQ-mode handshake, because leaving the QMX's menu can
 * drop IQ mode (Q9 is session state) and stop the audio stream. */
void cat_user_pause_set(bool paused);
bool cat_user_pause_active(void);

/* Queue a re-run of the Q9 IQ-mode enable+confirm handshake on the live link.
 * Cheap, invisible when IQ was already on, and the first thing worth trying
 * when the radio has stopped streaming audio while still answering CAT. */
void cat_request_iq_reassert(void);

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

// Close the CAT link deliberately, on our way out - see util/usb_shutdown.h.
// Sends TA0;RX; first so the radio is never left keyed, then tears the CDC
// handle down using the same poll-task-aware sequence the disconnect path uses.
// Safe to call with nothing open.
void cat_usb_shutdown(void);

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
