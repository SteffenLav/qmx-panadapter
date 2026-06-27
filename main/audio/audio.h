#pragma once

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

/**
 * @brief Discard everything currently buffered in the sample ring.
 *
 * MUST be called only from the single ring consumer (the dsp fft_task) — the
 * ring is single-producer/single-consumer and a second concurrent consumer
 * races it. Used at FT8 capture-arm so each slot starts from genuinely fresh
 * audio (boundary-aligned) instead of inheriting accumulated ring latency,
 * which time-shifts later captures off the FT8 symbol grid (sync still finds
 * candidates, LDPC fails). Returns the number of stereo pairs discarded.
 */
size_t audio_flush_ring(void);

/** @brief Stereo pairs currently buffered in the ring (latency indicator). */
size_t audio_ring_backlog_pairs(void);

/** @brief Running total of stereo pairs dropped on ring-full since boot. */
uint32_t audio_get_dropped_total(void);
