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

/* #298: tell the audio layer the dial moved, so each sample can be attributed
 * to the frequency it was actually captured at. Called from ui_update_frequency
 * - every tune path already goes through there. */
void     audio_note_dial_hz(uint32_t hz);
/* The dial the most recently READ samples were captured under. */
uint32_t audio_dial_for_last_read(void);

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

// Drop any partial 6-byte I/Q frame held from a previous UAC session. Called
// when a session starts or ends - a new stream begins on a frame boundary, and
// carrying a fragment across would misalign the interleave from the first
// sample. See the s_carry notes in audio.c.
void audio_reset_frame_alignment(void);

// Stop and close the USB audio stream deliberately, on our way out - see
// util/usb_shutdown.h. Sets the streaming interface back to alt 0, which is how
// the radio is told to stop producing isochronous audio. Safe with nothing open.
void audio_usb_shutdown(void);
