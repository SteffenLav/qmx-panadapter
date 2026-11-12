#pragma once
/* RTTY receive on the device - R005 (TODO-RTTY.md).
 *
 * Taps rx_audio's stage-1 complex baseband (fs/D, 6 kHz in USB/LSB/CW) and
 * runs rtty_demod on it inside rx_audio_task (core 1). No task of its own.
 *
 * The signal is given as an RF offset from the dial, Hz: the tone CENTRE as
 * it would sound in USB audio (e.g. 1423 for tones at 1338/1508). Mark is the
 * higher RF tone unless `reverse`. This does not depend on the radio's mode -
 * the QMX streams the same I/Q whatever mode it is in.
 *
 * Output: decoded text in a ring (rtty_rx_text_tail) and on the serial log,
 * one "rtty:" line per received line. RX only - nothing here can key. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool     on;
    int      offset_hz;     /* tone centre, RF offset from the dial */
    bool     reverse;
    float    sig_lvl;       /* smoothed conf, 0..1; squelch opens at 0.48 */
    bool     present;
    float    afc_hz;        /* AFC + acquisition offset in force */
    uint32_t chars;         /* characters printed since rtty_rx_set(on) */
    uint32_t acq_jumps;
    float    fs_hz;         /* rate the demod runs at; 0 = not fed yet */
} rtty_rx_status_t;

/* Turn the decoder on/off and set the signal. Safe from any task; the audio
 * task applies it on its next frame. */
void rtty_rx_set(bool on, int offset_hz, bool reverse);

bool rtty_rx_is_on(void);

void rtty_rx_status(rtty_rx_status_t *st);

/* Copy the last up to n-1 decoded characters into buf, NUL-terminated.
 * Returns the length. */
size_t rtty_rx_text_tail(char *buf, size_t n);

/* Called by rx_audio_task with the stage-1 output. nco_hz is the RF offset
 * from the dial that sits at baseband 0 (rx_audio's NCO1 minus the IF). */
void rtty_rx_feed(const float *re, const float *im, int n, float fs_hz, int nco_hz);
