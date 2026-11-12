/* R009 - RTTY TX timing gate. See rtty_tx_test.h. */
#include "rtty_tx_test.h"
#include "rtty_baudot.h"
#include "tx_band_guard.h"
#include "wspr_rx.h"
#include "cat/cat.h"
#include "storage/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rtty_txt";

#define STOP_RETRIES 5

static volatile bool s_running;
static volatile bool s_abort;
static rtty_tx_test_cfg_t s_cfg;
static char s_call[16];

bool rtty_tx_test_running(void) { return s_running; }
void rtty_tx_test_abort(void)   { s_abort = true; }

/* The tones' RF span, for the band guard. */
static void rf_span(const rtty_tx_test_cfg_t *c, uint32_t dial, uint32_t *lo, uint32_t *hi)
{
    *lo = dial + (uint32_t)(c->centre_hz - c->shift_hz / 2);
    *hi = dial + (uint32_t)(c->centre_hz + c->shift_hz / 2);
}

/* Sleep to an absolute instant: vTaskDelay to 1 ms short, then spin. The
 * 1 kHz tick alone would put up to 1 ms of jitter on a 22 ms bit. */
static void wait_until(int64_t target)
{
    int64_t now = esp_timer_get_time();
    if (target - now > 1500) vTaskDelay(pdMS_TO_TICKS((uint32_t)((target - now - 1000) / 1000)));
    while (esp_timer_get_time() < target) { }
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/* RX; must get through - same rule and same fallback as ft8_tx's
 * tx_cmd_critical(): retry, then hand it to the poll task to re-assert. */
static void key_up(void)
{
    for (int i = 0; i < STOP_RETRIES; i++) {
        if (cat_send_raw_cmd("RX;") == ESP_OK) return;
        ESP_LOGW(TAG, "RX; failed, attempt %d/%d", i + 1, STOP_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "RX; NEVER GOT THROUGH - handing to CAT to re-assert");
    cat_request_force_rx();
}

/* 30 s x 45 bits/s, at most one change per bit = 1364. PSRAM: internal RAM is tight. */
#define MAX_EDGES 1536

static void burst_task(void *arg)
{
    (void)arg;
    const rtty_tx_test_cfg_t c = s_cfg;
    const double bit_us = 1e6 / (double)c.baud;
    const double mark_hz  = c.centre_hz + c.shift_hz / 2.0;   /* rtty_demod: mark = centre + shift/2 */
    const double space_hz = c.centre_hz - c.shift_hz / 2.0;

    int32_t *slip = heap_caps_malloc(sizeof(int32_t) * MAX_EDGES, MALLOC_CAP_SPIRAM);
    int32_t *wus  = heap_caps_malloc(sizeof(int32_t) * MAX_EDGES, MALLOC_CAP_SPIRAM);
    int n_edge = 0, fails = 0, chars = 0;

    char text[160];
    snprintf(text, sizeof text,
             "RYRYRYRYRY THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 DE %s %s\r\n",
             s_call, s_call);

    cat_poll_hold(CAT_HOLD_TX_BURST);

    /* ⛔ BAND GUARD, re-checked here, right above the key-down: the dial can
     * move between the API call and this task starting. */
    char gerr[160] = "";
    uint32_t lo, hi;
    rf_span(&c, cat_get_frequency(), &lo, &hi);
    bool ok = tx_band_guard_allows(lo, hi, "RTTY TX test", gerr, sizeof gerr);
    if (!ok) {
        ESP_LOGE(TAG, "refused before key-down: %s", gerr);
        cat_poll_release(CAT_HOLD_TX_BURST);
        goto out;
    }

    ESP_LOGI(TAG, "KEYING: %.2f Bd, mark %.0f / space %.0f Hz, RF %lu..%lu, %d s - text '%s'",
             (double)c.baud, mark_hz, space_hz, (unsigned long)lo, (unsigned long)hi,
             c.seconds, s_call);
    cat_send_raw_cmd("TA%.2f;", mark_hz);
    cat_send_raw_cmd("TX;");

    const int64_t t0 = esp_timer_get_time() + 20000;   /* 20 ms for the PA to come up */
    const int64_t t_end = t0 + (int64_t)c.seconds * 1000000;
    double t_bit = 0.0;                    /* offset from t0, in us */
    int cur = 1;                           /* 1 = mark (already set) */

    /* Emit one bit: write TA only when the tone changes. */
#define EMIT_BIT(v, nbits) do {                                                       \
        int64_t _tgt = t0 + (int64_t)t_bit;                                      \
        if ((v) != cur) {                                                        \
            wait_until(_tgt);                                                    \
            int64_t _a = esp_timer_get_time();                                   \
            esp_err_t _e = cat_send_raw_cmd("TA%.2f;", (v) ? mark_hz : space_hz); \
            int64_t _b = esp_timer_get_time();                                   \
            if (_e != ESP_OK) fails++;                                           \
            if (n_edge < MAX_EDGES && slip && wus) {                             \
                slip[n_edge] = (int32_t)(_a - _tgt); wus[n_edge] = (int32_t)(_b - _a); n_edge++; \
            }                                                                    \
            cur = (v);                                                           \
        }                                                                        \
        t_bit += (nbits) * bit_us;                                               \
    } while (0)

    /* Half a second of mark so the receiver's squelch and AFC settle. */
    t_bit += 0.5e6;

    rtty_baudot_t bd;
    rtty_baudot_init(&bd, true);
    bool aborted = false;
    while (!aborted) {
        for (const char *p = text; *p; p++) {
            uint8_t codes[2];
            int nc = rtty_baudot_encode(&bd, *p, codes);
            for (int k = 0; k < nc; k++) {
                EMIT_BIT(0, 1.0);                                   /* start: space */
                for (int b = 0; b < 5; b++) EMIT_BIT((codes[k] >> b) & 1, 1.0);   /* LSB first */
                EMIT_BIT(1, 1.5);                                   /* stop: mark */
            }
            chars++;
            if (s_abort) { aborted = true; break; }
            if (t0 + (int64_t)t_bit >= t_end) break;
        }
        if (t0 + (int64_t)t_bit >= t_end) break;
    }
#undef EMIT_BIT
    /* Let the last stop bit and a short mark tail go out. */
    wait_until(t0 + (int64_t)t_bit + 200000);
    key_up();
    cat_poll_release(CAT_HOLD_TX_BURST);

    ESP_LOGI(TAG, "burst %s: %d chars, %d tone changes, %d write fails, %lld ms on air",
             aborted ? "ABORTED" : "complete", chars, n_edge, fails,
             (long long)((esp_timer_get_time() - t0) / 1000));
    if (n_edge > 0 && slip && wus) {
        qsort(slip, (size_t)n_edge, sizeof(int32_t), cmp_i32);
        qsort(wus,  (size_t)n_edge, sizeof(int32_t), cmp_i32);
        int n = n_edge;
        ESP_LOGI(TAG, "TA slip us: min %ld p50 %ld p90 %ld p99 %ld max %ld",
                 (long)slip[0], (long)slip[n / 2], (long)slip[n * 9 / 10],
                 (long)slip[n * 99 / 100], (long)slip[n - 1]);
        ESP_LOGI(TAG, "TA write us: min %ld p50 %ld p90 %ld p99 %ld max %ld",
                 (long)wus[0], (long)wus[n / 2], (long)wus[n * 9 / 10],
                 (long)wus[n * 99 / 100], (long)wus[n - 1]);
    }
out:
    free(slip);
    free(wus);
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t rtty_tx_test_start(const rtty_tx_test_cfg_t *cfg, char *err, size_t err_len)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_running) { snprintf(err, err_len, "an RTTY test burst is already running"); return ESP_ERR_INVALID_STATE; }
    rtty_tx_test_cfg_t c = *cfg;
    if (!(c.baud >= 20.0f && c.baud <= 100.0f)) { snprintf(err, err_len, "baud out of range"); return ESP_ERR_INVALID_ARG; }
    if (c.shift_hz < 50 || c.shift_hz > 1000)    { snprintf(err, err_len, "shift out of range"); return ESP_ERR_INVALID_ARG; }
    if (c.centre_hz - c.shift_hz / 2 < 300 || c.centre_hz + c.shift_hz / 2 > 2700) {
        snprintf(err, err_len, "tones must lie in 300..2700 Hz"); return ESP_ERR_INVALID_ARG;
    }
    if (c.seconds < 1) c.seconds = 1;
    if (c.seconds > RTTY_TX_TEST_MAX_S) c.seconds = RTTY_TX_TEST_MAX_S;

    qmx_settings_t *s = malloc(sizeof(*s));
    if (!s) { snprintf(err, err_len, "no memory"); return ESP_ERR_NO_MEM; }
    settings_load_all(s);
    bool sim = s->sim_mode_en;
    snprintf(s_call, sizeof s_call, "%s", s->my_callsign);
    free(s);
    if (sim)              { snprintf(err, err_len, "simulation mode is on"); return ESP_ERR_INVALID_STATE; }
    if (!s_call[0])       { snprintf(err, err_len, "no callsign set"); return ESP_ERR_INVALID_STATE; }
    if (strcmp(cat_get_mode_str(), "DiGi") != 0) {
        snprintf(err, err_len, "radio must be in DiGi (is %s)", cat_get_mode_str()); return ESP_ERR_INVALID_STATE;
    }
    if (cat_poll_held_by_other(CAT_HOLD_TX_BURST)) { snprintf(err, err_len, "another burst holds the CAT link"); return ESP_ERR_INVALID_STATE; }
    if (wspr_rx_running())                          { snprintf(err, err_len, "WSPR is running"); return ESP_ERR_INVALID_STATE; }

    uint32_t lo, hi;
    rf_span(&c, cat_get_frequency(), &lo, &hi);
    if (!tx_band_guard_allows(lo, hi, "RTTY TX test", err, err_len)) return ESP_ERR_INVALID_STATE;

    s_cfg = c;
    s_abort = false;
    s_running = true;
    /* Core 1, priority 5: above rx_audio (3) so a tone change is not queued
     * behind a DSP frame. The spin in wait_until is at most ~1 ms per edge. */
    if (xTaskCreatePinnedToCore(burst_task, "rtty_txt", 4096, NULL, 5, NULL, 1) != pdPASS) {
        s_running = false;
        snprintf(err, err_len, "could not start the task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
