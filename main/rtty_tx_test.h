/* R009 - RTTY TX timing gate. A BENCH TEST, not the RTTY transmitter (R010).
 *
 * Question: can the QMX follow `TA<freq>;` tone changes at one per RTTY bit
 * (22 ms at 45.45 Bd)? The host side is already measured (cat_ta_latency_probe:
 * write p99 1.9 ms). Only RF answers the radio side, so this KEYS THE RADIO:
 * TX;, then TA toggles between mark and space for a fixed text, then RX;.
 * A receiver running fldigi decodes it; clean copy = the gate passes.
 *
 * Operator-started only (POST /api/cmd {"action":"rtty_tx_test"}), never
 * armed by any automatic path. Refuses unless: DiGi mode, the band guard
 * passes both tones, no other burst holds the link, WSPR is not running,
 * simulation mode is off. Bounded to RTTY_TX_TEST_MAX_S. Abort with
 * {"action":"rtty_tx_test_abort"}. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define RTTY_TX_TEST_MAX_S 30

typedef struct {
    int   centre_hz;   /* audio centre of the pair, Hz (mark = centre + shift/2) */
    int   shift_hz;    /* 170 */
    float baud;        /* 45.45 */
    int   seconds;     /* how long to keep sending the text, 1..RTTY_TX_TEST_MAX_S */
} rtty_tx_test_cfg_t;

/* Checks the refusals, then starts the burst on its own task and returns.
 * ESP_OK = started; otherwise err holds the reason. */
esp_err_t rtty_tx_test_start(const rtty_tx_test_cfg_t *cfg, char *err, size_t err_len);

void rtty_tx_test_abort(void);
bool rtty_tx_test_running(void);
