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
 * Output: decoded text in a ring and on the serial log, one "rtty: rx:" line
 * per received line. RX only - nothing here can key. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool  on;
    int   offset_hz;     /* tone centre, RF offset from the dial */
    bool  reverse;
    float baud;          /* 45.45 default (R011) */
    float shift_hz;      /* 170 default */
    bool  afc;           /* AFC + acquisition, +/-150 Hz */
    bool  squelch;
} rtty_rx_cfg_t;

typedef struct {
    rtty_rx_cfg_t cfg;
    float    sig_lvl;       /* smoothed conf, 0..1; squelch opens at RTTY_SQ_OPEN */
    float    sq_open_lvl;   /* that threshold, for drawing the tick */
    bool     present;
    float    mark_lvl, space_lvl;   /* tone envelopes, arbitrary units */
    float    mark_db, space_db;     /* each tone over the noise floor, dB */
    float    afc_hz;        /* AFC + acquisition offset in force */
    bool     afc_held;      /* R026: a station was decoded since the tap, so afc_hz
                             * sits on it; false = any offset is acquisition alone */
    uint32_t chars;         /* characters printed since the decoder (re)started */
    uint32_t acq_jumps;
    float    fs_hz;         /* rate the demod runs at; 0 = not fed yet */
} rtty_rx_status_t;

/* The current settings (defaults before the first apply). */
/* The R011 defaults (the one copy) - config_io fills a fresh file from it. */
void rtty_rx_defaults(rtty_rx_cfg_t *c);
void rtty_rx_get_cfg(rtty_rx_cfg_t *c);

/* Apply settings. Safe from any task; the audio task restarts the decoder on
 * its next frame. Turning on wakes the audio chain. */
void rtty_rx_apply(const rtty_rx_cfg_t *c);

/* Shorthand kept for the web API's first form. */
void rtty_rx_set(bool on, int offset_hz, bool reverse);

bool rtty_rx_is_on(void);

void rtty_rx_status(rtty_rx_status_t *st);

/* Copy the last up to n-1 decoded characters into buf, NUL-terminated.
 * Returns the length. */
size_t rtty_rx_text_tail(char *buf, size_t n);

/* Incremental read: characters written since *cursor (a running count),
 * at most n. Advances *cursor; if the ring overran, skips to what is held.
 * Returns the number copied (not NUL-terminated). */
size_t rtty_rx_text_read(uint32_t *cursor, char *buf, size_t n);

/* Forget the text held (the page's Clear). Does not touch the decoder.
 * Bumps rtty_rx_text_clear_gen(), so every surface showing the text can clear
 * its own copy too - a web Clear empties the Tab5 grid and the other way round. */
void rtty_rx_text_clear(void);
uint32_t rtty_rx_text_clear_gen(void);

/* Running count of characters ever written - a reader's cursor starts here. */
uint32_t rtty_rx_text_count(void);

/* Called by rx_audio_task with RTTY's own front end (rtty_fe_run, NOT the audio
 * path's stage 1 - R023). nco_hz is the RF offset from the dial that sits at
 * baseband 0; the front end puts the tone centre there, so it is offset_hz. */
void rtty_rx_feed(const float *re, const float *im, int n, float fs_hz, int nco_hz);
