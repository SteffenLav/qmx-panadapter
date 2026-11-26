/* RTTY transmitter (R010). See rtty_tx.h and docs/rtty-tx-design.md.
 *
 * The bit loop is R009's burst loop, which passed on air 2026-11-14 (fldigi
 * copied "THE QUICK BROWN FOX ... DE OZ1LAV" clean; TA slip p99 0.9 ms
 * against a 22 ms bit). rtty_tx_test.c is gone: one keying path, not two. */
#include "rtty_tx.h"
#include "rtty_tx_core.h"
#include "rtty_rx.h"
#include "rtty_log.h"
#include "cw_callsign.h"
#include "tx_band_guard.h"
#include "wspr_rx.h"
#include "cat/cat.h"
#include "storage/settings.h"
#include "ui/ui.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rtty_tx";

#define STOP_RETRIES 5

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* PSRAM - see rtty_rx.c s_text for why. */
static EXT_RAM_BSS_ATTR rtty_txc_t   s_core;            /* guarded by s_mux */
static EXT_RAM_BSS_ATTR rtty_tx_status_t s_st;          /* guarded by s_mux */
static volatile bool s_keyed;          /* an over owns the radio, from start to RX; */
static volatile bool s_stop;
#define TX_TEXT_RING 512
static EXT_RAM_BSS_ATTR char s_tx_text[TX_TEXT_RING];   /* guarded by s_mux; PSRAM */
static uint32_t s_tx_text_w;               /* characters ever written */

/* Set by start_over() for the task; the task owns them after that. */
static uint32_t s_dial;
static int      s_centre, s_shift;
static bool     s_reverse, s_need_qsy, s_need_digi;
static float    s_baud;
static int      s_mark_hz, s_space_hz;

bool rtty_tx_keyed(void) { return s_keyed; }

uint32_t rtty_tx_text_count(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t w = s_tx_text_w;
    portEXIT_CRITICAL(&s_mux);
    return w;
}

size_t rtty_tx_text_read(uint32_t *cursor, char *buf, size_t n)
{
    size_t k = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_tx_text_w - *cursor > TX_TEXT_RING) *cursor = s_tx_text_w - TX_TEXT_RING;
    while (*cursor != s_tx_text_w && k < n) buf[k++] = s_tx_text[(*cursor)++ % TX_TEXT_RING];
    portEXIT_CRITICAL(&s_mux);
    return k;
}

void rtty_tx_status(rtty_tx_status_t *st)
{
    portENTER_CRITICAL(&s_mux);
    *st = s_st;
    st->keyed  = s_keyed;
    st->queued = rtty_txc_queued(&s_core);
    portEXIT_CRITICAL(&s_mux);
}

static void set_err(const char *e)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(s_st.err, sizeof s_st.err, "%s", e);
    portEXIT_CRITICAL(&s_mux);
}

/* Sleep to an absolute instant: 1 ms ticks to within 2 ms, then spin - the
 * tick alone would put up to 1 ms of jitter on a 22 ms bit. Returns false
 * at once when Stop is asked for, so Stop never waits out a run of marks. */
static bool wait_until(int64_t target)
{
    for (;;) {
        if (s_stop) return false;
        int64_t left = target - esp_timer_get_time();
        if (left <= 0) return true;
        if (left > 2000) vTaskDelay(1); else while (esp_timer_get_time() < target) { }
    }
}

/* RX; must get through - ft8_tx's tx_cmd_critical() rule: retry, then hand
 * it to the poll task to re-assert. */
static void key_up(void)
{
    cat_send_raw_cmd("TA0;");
    for (int i = 0; i < STOP_RETRIES; i++) {
        if (cat_send_raw_cmd("RX;") == ESP_OK) return;
        ESP_LOGW(TAG, "RX; failed, attempt %d/%d", i + 1, STOP_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "RX; NEVER GOT THROUGH - handing to CAT to re-assert");
    cat_request_force_rx();
}

/* The over's RF span for the band guard. */
static bool guard(uint32_t dial, int lo_hz, int hi_hz, char *err, size_t err_len)
{
    return tx_band_guard_allows(dial + (uint32_t)lo_hz, dial + (uint32_t)hi_hz,
                                "RTTY TX", err, err_len);
}

static uint32_t qsy_target(uint32_t dial, int centre)
{
    return cat_quantise_dial_hz((uint32_t)((int64_t)dial + centre - RTTY_TX_QSY_CENTRE));
}

/* Move the dial so the decoder's centre sits at RTTY_TX_QSY_CENTRE, with the
 * decoder re-offset by the same amount: RF, lines and decode stay where they
 * were (operator, 2026-11-14: "Move the dial"). AFC is folded into the new
 * offset, because rtty_rx_apply() restarts the demodulator. Waits for the
 * radio to report the new dial - so it must run with the CAT poll NOT held. */
/* Radio to DiGi (MD6) through the poll task (cat_request_mode: retried, not
 * dropped by the 200 ms write limiter - see ui.c restore_apply_step), then
 * wait for the MD readback, as ft8_tx_arm() does. */
static bool to_digi(char *err, size_t err_len)
{
    ESP_LOGI(TAG, "radio is in %s - switching to DiGi", cat_get_mode_str());
    cat_request_mode("DIGI");
    for (int i = 0; i < 15 && strcmp(cat_get_mode_str(), "DiGi") != 0; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (strcmp(cat_get_mode_str(), "DiGi") != 0) {
        snprintf(err, err_len, "radio did not switch to DiGi (is %s)", cat_get_mode_str());
        return false;
    }
    return true;
}

static bool qsy(uint32_t *dial, int *centre, char *err, size_t err_len)
{
    uint32_t want = qsy_target(*dial, *centre);
    if (cat_set_frequency_forced(want) != ESP_OK) { snprintf(err, err_len, "dial move failed"); return false; }
    for (int i = 0; i < 50 && cat_get_frequency() != want; i++) vTaskDelay(pdMS_TO_TICKS(60));
    if (cat_get_frequency() != want) {
        cat_cancel_pending_freq_if(want);
        snprintf(err, err_len, "radio did not confirm the dial move");
        return false;
    }
    ui_update_frequency(want);
    int new_centre = (int)((int64_t)*dial + *centre - (int64_t)want);
    rtty_rx_cfg_t rc; rtty_rx_get_cfg(&rc);
    rc.offset_hz = new_centre;
    rtty_rx_apply(&rc);
    ESP_LOGI(TAG, "QSY for TX: dial %lu -> %lu, centre %d -> %d Hz",
             (unsigned long)*dial, (unsigned long)want, *centre, new_centre);
    *dial = want; *centre = new_centre;
    return true;
}

static void over_end(const char *why, int fails, int64_t t0)
{
    portENTER_CRITICAL(&s_mux);
    int dropped = rtty_txc_cancel(&s_core);
    uint32_t sent = s_st.sent;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "over ended (%s): %lu chars, %d dropped, %d write fails, %lld ms",
             why, (unsigned long)sent, dropped, fails,
             t0 ? (long long)((esp_timer_get_time() - t0) / 1000) : 0LL);
}

/* One bit at the over's absolute bit clock: a TA write only when the tone
 * changes. Returns false when Stop was asked for (checked at every bit). */
typedef struct { int64_t t0; double t_bit, bit_us; int cur, fails; } bitclk_t;

static bool emit(bitclk_t *k, int v, double nbits)
{
    if (s_stop) return false;
    if (v != k->cur) {
        if (!wait_until(k->t0 + (int64_t)k->t_bit)) return false;
        if (cat_send_raw_cmd("TA%d;", v ? s_mark_hz : s_space_hz) != ESP_OK) k->fails++;
        k->cur = v;
    }
    k->t_bit += nbits * k->bit_us;
    return true;
}

static void over_task(void *arg)
{
    (void)arg;
    char err[96] = "";
    /* ⛔ THE MODE SWITCH AND THE DIAL MOVE GO BEFORE cat_poll_hold(): with
     * the poll held, a write is parked, not sent, and its readback never
     * comes. The radio stays in DiGi after the over, as after FT8 TX. */
    /* No radio, no over (2026-11-26, sibling of the WSPR fix): otherwise
     * TX; fails silently and key_up()'s force-RX logs a false "may still be
     * transmitting". */
    if (!cat_is_ready()) { snprintf(err, sizeof err, "no radio connected"); goto refused; }
    if (s_need_digi && !to_digi(err, sizeof err)) goto refused;
    if (s_need_qsy && !qsy(&s_dial, &s_centre, err, sizeof err)) goto refused;
    /* ⛔ BAND GUARD, right above key-down, on the dial actually in force. */
    if (!tx_band_guard_allows(s_dial + (uint32_t)(s_centre - s_shift / 2),
                              s_dial + (uint32_t)(s_centre + s_shift / 2),
                              "RTTY TX", err, sizeof err)) goto refused;
    if (cat_poll_held_by_other(CAT_HOLD_TX_BURST)) {
        snprintf(err, sizeof err, "another transmission holds the radio");
        goto refused;
    }
    {
        int hi = s_centre + s_shift / 2, lo = s_centre - s_shift / 2;
        s_mark_hz  = s_reverse ? lo : hi;          /* rtty_demod: mark = centre + shift/2 */
        s_space_hz = s_reverse ? hi : lo;
        portENTER_CRITICAL(&s_mux);
        s_st.centre_hz = s_centre;
        portEXIT_CRITICAL(&s_mux);
    }

    cat_poll_hold(CAT_HOLD_TX_BURST);
    ESP_LOGI(TAG, "KEYING: %.2f Bd, mark %d / space %d Hz, dial %lu",
             (double)s_baud, s_mark_hz, s_space_hz, (unsigned long)s_dial);
    cat_send_raw_cmd("TA%d;", s_mark_hz);
    cat_send_raw_cmd("TX;");
    bitclk_t k = { esp_timer_get_time() + 20000, 0.5e6, 1e6 / (double)s_baud, 1, 0 };
    /* (20 ms for the PA, then 0.5 s of mark so receivers' squelch/AFC settle) */
    const int64_t t_max = k.t0 + (int64_t)RTTY_TX_MAX_S * 1000000;
    const int idle_max = (int)ceil(RTTY_TX_IDLE_S * (double)s_baud / 7.5);
    rtty_baudot_t echo; rtty_baudot_init(&echo, true);
    const char *why = "queue empty";

    for (;;) {
        if (k.t0 + (int64_t)k.t_bit >= t_max) { why = "180 s limit"; break; }
        portENTER_CRITICAL(&s_mux);
        int code = rtty_txc_next(&s_core, idle_max);
        portEXIT_CRITICAL(&s_mux);
        if (code < 0) break;
        bool go = emit(&k, 0, 1.0);                                       /* start */
        for (int b = 0; go && b < 5; b++) go = emit(&k, (code >> b) & 1, 1.0);   /* LSB first */
        if (go) go = emit(&k, 1, 1.5);                                    /* stop */
        if (!go) { why = "stopped"; break; }
        int ch = rtty_baudot_decode(&echo, (uint8_t)code);
        portENTER_CRITICAL(&s_mux);
        if (ch && ch != '\r') {
            size_t n = strlen(s_st.echo);
            if (n + 1 >= sizeof s_st.echo) { memmove(s_st.echo, s_st.echo + 1, n); n--; }
            s_st.echo[n] = (ch == '\n') ? ' ' : (char)ch; s_st.echo[n + 1] = 0;
            s_st.sent++;
            s_tx_text[s_tx_text_w++ % TX_TEXT_RING] = (char)ch;
        }
        s_st.seconds = (int)((esp_timer_get_time() - k.t0) / 1000000);
        portEXIT_CRITICAL(&s_mux);
    }
    if (!s_stop) wait_until(k.t0 + (int64_t)k.t_bit + 200000);  /* last stop bit + tail */
    key_up();
    cat_poll_release(CAT_HOLD_TX_BURST);
    over_end(why, k.fails, k.t0);
    s_keyed = false;
    vTaskDelete(NULL);
    return;

refused:
    ESP_LOGW(TAG, "over refused before key-down: %s", err);
    set_err(err);
    over_end("refused", 0, 0);
    s_keyed = false;
    vTaskDelete(NULL);
}

/* The quick refusals, decided on the caller so it gets the reason at once.
 * Nothing here blocks: the dial move and key-down run on the task. */
static esp_err_t start_over(char *err, size_t err_len)
{
    char call[16]; settings_get_my_callsign(call, sizeof call);
    if (!call[0])                   { snprintf(err, err_len, "no callsign set"); return ESP_ERR_INVALID_STATE; }
    if (settings_get_sim_mode_en()) { snprintf(err, err_len, "simulation mode is on"); return ESP_ERR_INVALID_STATE; }
    /* Not in DiGi: the over task switches it (to_digi), as FT8 and WSPR TX
     * do. This was a refusal until 2026-11-15, and the RTTY page dims the
     * Mode control, so a radio left in USB (bench2 overnight) refused every
     * CQ with no way to fix it from the page. Not done here: the Tab5 caller
     * is the LVGL thread, and the confirm takes up to 1.5 s. */
    bool need_digi = strcmp(cat_get_mode_str(), "DiGi") != 0;
    if (cat_poll_held_by_other(CAT_HOLD_TX_BURST)) {
        snprintf(err, err_len, "another transmission holds the radio"); return ESP_ERR_INVALID_STATE;
    }
    if (wspr_rx_running())          { snprintf(err, err_len, "WSPR is running"); return ESP_ERR_INVALID_STATE; }

    rtty_rx_status_t rs; rtty_rx_status(&rs);
    if (!rs.cfg.on)                 { snprintf(err, err_len, "RTTY page is not open"); return ESP_ERR_INVALID_STATE; }
    /* ⛔ AFC THAT NO DECODE CONFIRMED IS NOT WHERE TO TRANSMIT. Acquisition
     * (R013) jumps to any tone pair within +/-150 Hz until a station is
     * decoded (R026 then holds it). bench2 2026-11-14, first on-air CQ: tap
     * on a free spot in a contest, AFC +145 Hz with nothing ever decoded,
     * and the CQ went out 145 Hz off the tap - onto whatever pair it had
     * found. So unless a station was decoded since the tap, restart the
     * decoder (AFC 0: the orange lines go back to the tap) and send there.
     * TX still equals the lines at key-down. */
    int afc = (int)lroundf(rs.afc_hz);
    if (afc != 0 && !rs.afc_held) {
        rtty_rx_cfg_t rc; rtty_rx_get_cfg(&rc);
        rtty_rx_apply(&rc);
        ESP_LOGW(TAG, "AFC %+d Hz not on a decoded station: TX at the tap, AFC reset", afc);
        afc = 0;
    }
    int centre = rs.cfg.offset_hz + afc;
    int shift  = (int)lroundf(rs.cfg.shift_hz);
    uint32_t dial = cat_get_frequency();
    if (!dial)                      { snprintf(err, err_len, "no dial reading from the radio"); return ESP_ERR_INVALID_STATE; }

    bool need_qsy = (centre - shift / 2 < RTTY_TX_AUDIO_LO || centre + shift / 2 > RTTY_TX_AUDIO_HI);
    /* Guard the destination BEFORE anything moves: a refused over must not
     * leave the dial somewhere the operator did not put it. */
    uint32_t d2 = need_qsy ? qsy_target(dial, centre) : dial;
    int c2 = (int)((int64_t)dial + centre - (int64_t)d2);
    if (!guard(d2, c2 - shift / 2, c2 + shift / 2, err, err_len)) return ESP_ERR_INVALID_STATE;

    s_dial = dial; s_centre = centre; s_shift = shift;
    s_reverse = rs.cfg.reverse; s_baud = rs.cfg.baud; s_need_qsy = need_qsy;
    s_need_digi = need_digi;
    portENTER_CRITICAL(&s_mux);
    s_st.sent = 0; s_st.seconds = 0; s_st.echo[0] = 0;
    portEXIT_CRITICAL(&s_mux);
    s_stop = false;
    s_keyed = true;
    /* Core 1, priority 7: ABOVE httpd and render (5) and fft_task (4).
     * Was 5 (R009, measured on bench2 with an idle web server). On bench1
     * 2026-11-15 a typed web over (two HTTP requests per 150 ms) had bit
     * edges off by >2.5 ms in 11 % of intervals (p95 3.9 ms) against 1.6 %
     * for a one-request CQ, and fldigi lost a LTRS and a space: at equal
     * priority FreeRTOS time-slices the keying task away at an edge. The
     * busy-wait in wait_until() is <= 2 ms per edge, so <= 9 % of core 1,
     * and only during an over. */
    if (xTaskCreatePinnedToCore(over_task, "rtty_tx", 4096, NULL, 7, NULL, 1) != pdPASS) {
        s_keyed = false;
        snprintf(err, err_len, "could not start the task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t rtty_tx_send(const char *text, bool end, char *err, size_t err_len)
{
    if (!text) text = "";
    portENTER_CRITICAL(&s_mux);
    bool was_keyed = s_keyed;
    if (!was_keyed) rtty_txc_init(&s_core);
    bool ok = rtty_txc_push(&s_core, text);
    if (ok && end) rtty_txc_end(&s_core);
    portEXIT_CRITICAL(&s_mux);
    if (!ok) { snprintf(err, err_len, "TX buffer full"); set_err(err); return ESP_ERR_NO_MEM; }
    if (was_keyed) { set_err(""); return ESP_OK; }
    esp_err_t e = start_over(err, err_len);
    if (e != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        rtty_txc_cancel(&s_core);
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "send refused: %s", err);
        set_err(err);
    } else {
        set_err("");
    }
    return e;
}

void rtty_tx_stop(void)
{
    if (!s_keyed) return;
    s_stop = true;          /* the task sees it within 1 ms tick, then TA0; RX; */
    ESP_LOGI(TAG, "stop requested");
}

/* ---- the station being worked + the four messages ------------------------ */

static char     s_call[16];   /* guarded by s_mux */
static uint32_t s_call_gen;

bool rtty_tx_set_call(const char *call)
{
    char c[16] = "";
    if (call && call[0]) {
        size_t n = strlen(call);
        if (n >= sizeof c) return false;
        for (size_t i = 0; i <= n; i++) c[i] = (char)toupper((unsigned char)call[i]);
        if (!cw_call_structure_ok(c)) return false;
    }
    portENTER_CRITICAL(&s_mux);
    if (strcmp(s_call, c)) { memcpy(s_call, c, sizeof s_call); s_call_gen++; }
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void rtty_tx_get_call(char *buf, size_t n)
{
    if (!buf || !n) return;
    portENTER_CRITICAL(&s_mux);
    snprintf(buf, n, "%s", s_call);
    portEXIT_CRITICAL(&s_mux);
}

uint32_t rtty_tx_call_gen(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t g = s_call_gen;
    portEXIT_CRITICAL(&s_mux);
    return g;
}

/* Moved here from rtty_screen_view.c's macro_cb() so the web page sends the
 * same text (2026-11-15). The leading newline starts the over on a fresh
 * line at the receiver. */
esp_err_t rtty_tx_macro(rtty_msg_t which, char *err, size_t err_len)
{
    char my[16], grid[12], c[16], t[200];
    settings_get_my_callsign(my, sizeof my);
    settings_get_my_grid(grid, sizeof grid);
    rtty_tx_get_call(c, sizeof c);
    if (!my[0]) { snprintf(err, err_len, "no callsign set (Identity)"); return ESP_ERR_INVALID_STATE; }
    if (which != RTTY_MSG_CQ && !c[0]) {
        snprintf(err, err_len, "pick their callsign first");
        return ESP_ERR_INVALID_STATE;
    }
    /* R030 contest mode: the short forms a contest station expects. The
     * exchange is the serial (advanced on LOG, rtty_log.c, so a repeat sends
     * the same number) or the fixed text set for the contest. */
    settings_rtty_contest_t ct;
    settings_get_rtty_contest(&ct);
    if (ct.on) {
        char ex[16];
        rtty_log_sent_exch(ex, sizeof ex);
        switch (which) {
        case RTTY_MSG_CQ:     snprintf(t, sizeof t, "\nCQ TEST %s %s CQ\n", my, my); break;
        case RTTY_MSG_ANSWER: snprintf(t, sizeof t, "\n%s %s\n", my, my); break;
        case RTTY_MSG_EXCH:   snprintf(t, sizeof t, "\n%s 599 %s %s\n", c, ex, ex); break;
        default:              snprintf(t, sizeof t, "\nTU %s\n", my); break;
        }
    } else switch (which) {
    case RTTY_MSG_CQ:
        snprintf(t, sizeof t, "\nCQ CQ CQ DE %s %s %s\nCQ CQ CQ DE %s %s %s PSE K\n",
                 my, my, my, my, my, my);
        break;
    case RTTY_MSG_ANSWER:
        snprintf(t, sizeof t, "\n%s %s DE %s %s %s K\n", c, c, my, my, my);
        break;
    case RTTY_MSG_EXCH:
        if (grid[0])
            snprintf(t, sizeof t, "\n%s DE %s TNX FER CALL UR RST 599 599 QTH %.4s %.4s\n%s DE %s K\n",
                     c, my, grid, grid, c, my);
        else
            snprintf(t, sizeof t, "\n%s DE %s TNX FER CALL UR RST 599 599\n%s DE %s K\n", c, my, c, my);
        break;
    default:
        snprintf(t, sizeof t, "\n%s DE %s TNX FER QSO 73 GL\n%s DE %s SK\n", c, my, c, my);
        break;
    }
    ESP_LOGI(TAG, "macro: %s%s", which == RTTY_MSG_CQ ? "CQ" : which == RTTY_MSG_ANSWER ? "Answer" :
             which == RTTY_MSG_EXCH ? "Exch" : "73", ct.on ? " (contest)" : "");
    return rtty_tx_send(t, true, err, err_len);
}
