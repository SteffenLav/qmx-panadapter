/* RTTY transmitter (R010). Design: docs/rtty-tx-design.md.
 *
 * rtty_tx_send() KEYS THE RADIO when it is idle: it checks the refusals,
 * moves the dial if the decoder's tones lie outside the passband, re-checks
 * the band guard and starts the over on its own task. While an over runs it
 * only appends text. The over ends when the queue is empty and `end` was
 * asked for, 3 s after typing stops, at RTTY_TX_MAX_S, or on rtty_tx_stop().
 *
 * Tones: centre = the decoder's tap offset + AFC, mark = centre + shift/2
 * (reverse swaps), the same convention as rtty_demod - you transmit where you
 * decode. Every press is a human decision: see docs/unattended-tx-inventory.md. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RTTY_TX_MAX_S       180    /* hard key-down limit per over */
#define RTTY_TX_IDLE_S      3      /* unkey after this long with nothing queued */
#define RTTY_TX_AUDIO_LO    400    /* lowest tone sent without moving the dial */
#define RTTY_TX_AUDIO_HI    2700   /* highest */
#define RTTY_TX_QSY_CENTRE  1500   /* where a dial move puts the centre */

typedef struct {
    bool     keyed;          /* an over is running */
    int      queued;         /* characters not yet sent */
    uint32_t sent;           /* characters sent this over */
    int      centre_hz;      /* audio centre of this over's tones */
    int      seconds;        /* key-down time of this over */
    char     echo[64];       /* the last characters sent, oldest first */
    char     err[96];        /* why the last send was refused, or "" */
} rtty_tx_status_t;

/* Queue text (and start an over when idle). end = finish the over once this
 * text has gone. ESP_OK = queued/started; otherwise err says why. */
esp_err_t rtty_tx_send(const char *text, bool end, char *err, size_t err_len);

/* Stop now: tone off, RX;, queue dropped. Safe to call when idle. */
void rtty_tx_stop(void);

bool rtty_tx_keyed(void);

/* What was actually sent, as a stream ('\n' kept, '\r' dropped), for the
 * RTTY page's text box (operator 2026-11-14: "i want to see my tx on the
 * decode - in red"). Same cursor scheme as rtty_rx_text_read(): a reader
 * that fell more than the ring behind skips ahead. */
uint32_t rtty_tx_text_count(void);
size_t   rtty_tx_text_read(uint32_t *cursor, char *buf, size_t n);
void rtty_tx_status(rtty_tx_status_t *st);

/* The station being worked - {CALL} in the macros, "To:" on both surfaces.
 * One copy here so a callsign picked on the Tab5 (a tap in the text) or on
 * the web page (a click, or typed) is the same one on both. set: "" clears;
 * anything else must pass cw_call_structure_ok() (false = refused, nothing
 * changed). gen bumps on every change, so a surface can notice the other's. */
bool     rtty_tx_set_call(const char *call);
void     rtty_tx_get_call(char *buf, size_t n);
uint32_t rtty_tx_call_gen(void);

/* The four fixed messages, built here so the Tab5 and the web send the same
 * bytes. CALL = rtty_tx_get_call(), MY = Identity callsign, GRID = 4-char
 * locator (left out when unset). Sends with end = true. ⛔ KEYS THE RADIO
 * like rtty_tx_send(); every caller is a human press. */
typedef enum { RTTY_MSG_CQ, RTTY_MSG_ANSWER, RTTY_MSG_EXCH, RTTY_MSG_73 } rtty_msg_t;
esp_err_t rtty_tx_macro(rtty_msg_t which, char *err, size_t err_len);
