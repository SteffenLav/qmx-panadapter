#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Initialize the audio subsystem.
 *
 * Phase 3.2: UAC host driver + stream open at 48 kHz / 2 ch / 24-bit
 *            (auto-discovered from device descriptors).
 * Phase 3.3: ring-buffered int16 stereo samples for downstream consumption.
 *
 * Must be called AFTER bsp_usb_host_start() has been invoked.
 */
esp_err_t audio_init(void);

/**
 * @brief Read decoded audio samples (interleaved int16 L/R) from the ring buffer.
 *
 * @param[out] dst              Destination buffer (host-allocated)
 * @param[in]  max_pairs        Max stereo pairs the caller can accept
 * @param[in]  timeout_ms       How long to wait if buffer is empty (0 = no wait)
 * @return Number of stereo pairs actually written into dst (may be 0)
 */
size_t audio_read_samples(int16_t *dst, size_t max_pairs, uint32_t timeout_ms);

/** @brief Stereo pairs currently buffered in the ring (latency indicator). */
size_t audio_ring_backlog_pairs(void);

/** @brief Running total of stereo pairs dropped on ring-full since boot. */
uint32_t audio_get_dropped_total(void);

// True while a UAC device is open (streaming or mid-setup). Read by the
// stale-QMX detector so it never replugs a connecting/connected device.
bool audio_uac_active(void);

/**
 * @brief Request a soft audio reset on the next live sample batch.
 *
 * Sets the same flag the UAC stream-restart path sets, causing
 * ui_flat_mode_reset() to fire when the next non-empty audio poll
 * arrives.  Safe to call from any task.
 */
void audio_request_reset(void);
