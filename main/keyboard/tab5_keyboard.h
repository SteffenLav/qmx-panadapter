#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Driver for the M5Stack Tab5 snap-on keyboard (SKU A164, 70 keys).
 *
 * The keyboard is an STM32F030C8T6 acting as an I2C slave at address 0x6D on
 * its own bus (SDA=GPIO0, SCL=GPIO1) — a C port of M5Stack's official
 * M5Tab5-Keyboard-UserDemo (see docs/tab5-keyboard-ref/). We drive it in
 * "String mode": the STM32 does the keymap + shift/symbol-layer handling and
 * returns ready ASCII, so the host never needs a row/col → char table.
 *
 * v1 uses 50 ms polling (no GPIO50 interrupt) for simplicity.
 */

/* Called from the keyboard poll task whenever the keyboard delivers typed
 * characters. `text` is a NUL-terminated run of bytes; it may include control
 * bytes (0x08 backspace, 0x0D/0x0A enter) which the consumer interprets.
 * Runs OUTSIDE the LVGL thread — the consumer must take display_lock() before
 * touching any LVGL object. */
// `mods` is the STM32's modifier byte, delivered with every String-mode event
// and previously read and thrown away. It is what makes shortcuts possible:
// CLAUDE.md's note that "modifier keys emit NO event" is about pressing Ctrl or
// Fn ON ITS OWN - a modifier COMBINED with a key arrives here, in this byte.
//
// ⚠ THE BIT LAYOUT IS NOT KNOWN YET and must not be guessed. Every distinct
// value is logged once by the UI bridge (see kbd_text_cb in ui.c) so the real
// mapping can be read off the hardware rather than assumed - this keyboard has
// already cost this project one confident-wrong byte assumption, when Backspace
// was implemented as 0x08 because a report said "Backspace" and the radio
// actually wanted 0x7F.
typedef void (*tab5_kbd_text_cb_t)(const char *text, uint8_t mods, void *arg);

/* Probe for the keyboard on GPIO0/1 and, if present, switch it to String mode
 * and start the poll task. Returns:
 *   ESP_OK            keyboard found and running
 *   ESP_ERR_NOT_FOUND no device answered at 0x6D (the bus is scanned and the
 *                     result logged; the feature is silently disabled)
 *   other             I2C bus could not be created
 * Safe to call even if no keyboard is attached. */
esp_err_t tab5_keyboard_init(void);

/* Register the text callback. May be called before or after tab5_keyboard_init(). */
void tab5_keyboard_set_text_cb(tab5_kbd_text_cb_t cb, void *arg);

/* True once a keyboard has been detected and the poll task is running. */
bool tab5_keyboard_present(void);

#ifdef __cplusplus
}
#endif
