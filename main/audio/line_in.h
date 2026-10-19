#pragma once
/* line_in.h - the 3.5 mm jack as a receive source.
 *
 * WHAT THE HARDWARE GIVES US
 *   The Tab5's 3.5 mm socket is a 4-pole headset jack. Its MIC contact reaches
 *   the ES7210 four-channel ADC on TDM slot 3 - measured 2026-10-10 by Tony A
 *   (tony1tf), who fed his 80 m DF receiver's headphone output in through a
 *   6:1 lead and watched channel 3 go to nearly full scale while 0 and 2
 *   followed the internal microphones and 1 showed nothing. M5Stack publish
 *   no schematic for the socket, so that measurement is the only source for
 *   the mapping.
 *
 * ⛔ ONE CHANNEL. THERE IS NO QUADRATURE.
 *   A single mic contact carries one real signal. The panadapter chain is
 *   complex IQ end to end, and feeding a real signal into it unchanged mirrors
 *   every carrier about the tuned centre - that is missing information, not a
 *   software choice. So this file synthesises the quadrature channel with a
 *   Hilbert transform and hands audio.c an analytic signal. The spectrum is
 *   then one-sided and correct: audio 0..24 kHz lands on the POSITIVE half of
 *   the span and the negative half is empty, which is an honest picture of
 *   what one channel can and cannot tell you.
 *
 * ⛔ NOT A PANADAPTER, AND IT CANNOT BECOME ONE.
 *   A Hilbert transform recovers a one-sided spectrum; it does not recover the
 *   sideband information a second channel would have carried. This is an
 *   AUDIO input - an audio spectrum, and the FT8/FT4/JS8/WSPR decoders, which
 *   want exactly a real 0-6 kHz baseband. Do not wire it to anything that
 *   assumes an RF dial.
 *
 * ⛔ MUTUALLY EXCLUSIVE WITH THE QMX, decided at boot. See rx_source_t.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Bring the ES7210 up and start the sampler. Call INSTEAD of
 * bsp_usb_host_start(), and only when rx_source_is_real() says so.
 * Returns ESP_OK once samples are flowing into audio.c's ring. */
esp_err_t line_in_start(void);

/* True once line_in_start() has succeeded. */
bool line_in_running(void);

/* Apply the analogue gain live, in dB. Quantised by the part to 3 dB steps -
 * line_in_gain_db_normalize() in settings.h is the same rule. Unlike the
 * source itself this needs no restart: it is one I2C register write. */
esp_err_t line_in_set_gain_db(uint8_t db);

/* Peak level of the last ~100 ms, in dBFS (<= 0). Returns -99.0f when nothing
 * has been sampled yet. This is what the level meter beside the gain control
 * reads, and it is the ONLY honest way to set the gain - the right value
 * depends on whatever the operator has plugged in. */
float line_in_peak_dbfs(void);

/* Samples that reached the converter's rail during the last ~100 ms window.
 *
 * ⭐ THE PEAK METER ALONE CANNOT SHOW CLIPPING. Past full scale the samples
 * stop growing, so the meter parks at 0 dBFS and reads like a strong healthy
 * signal while the waveform is being flattened. Non-zero here means back the
 * gain off, whatever the peak says. */
uint32_t line_in_clip_count(void);
