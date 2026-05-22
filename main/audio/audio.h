#pragma once

#include "esp_err.h"

/**
 * @brief Initialize the audio subsystem.
 *
 * Phase 3.2: install UAC host driver, wait for QMX as UAC microphone,
 *            open stream IN at 48 kHz / 2 ch / 16-bit, log sample stats
 *            once per second (count + peak L/R).
 *
 * Must be called AFTER cat_init() has installed the USB host stack.
 */
esp_err_t audio_init(void);
