/* RTTY receive on the device - see rtty_rx.h. R005. */
#include "rtty_rx.h"
#include "rtty_demod.h"
#include "rtty_baudot.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "rx_audio.h"

static const char *TAG = "rtty";

/* Defaults agreed 2026-11-12 (R011): 45.45 Bd, 170 Hz, USOS, AFC +/-150. */
#define RTTY_RX_BAUD     45.45f
#define RTTY_RX_SHIFT    170.0f
#define RTTY_RX_AFC_HZ   150.0f
#define RTTY_RX_SQUELCH  RTTY_SQ_OPEN

#define TEXT_RING 2048          /* power of two */
#define LOG_LINE  96

/* Requested by rtty_rx_set (any task), applied by the feed (audio task). */
static volatile bool s_want_on;
static volatile int  s_want_off_hz;
static volatile bool s_want_rev;
static volatile uint32_t s_want_seq;     /* bumped on every set */

/* Owned by the audio task. */
static rtty_demod_t *s_dem;              /* PSRAM, ~22 KB, allocated once */
static rtty_baudot_t s_bd;
static uint32_t s_have_seq;
static float    s_have_fs;
static int      s_have_nco;
static bool     s_running;
static char     s_line[LOG_LINE + 1];
static int      s_line_n;

/* Shared: text ring + status snapshot. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char     s_text[TEXT_RING];
static uint32_t s_text_w;
static rtty_rx_status_t s_st;

void rtty_rx_set(bool on, int offset_hz, bool reverse)
{
    s_want_off_hz = offset_hz;
    s_want_rev = reverse;
    s_want_on = on;
    s_want_seq++;
    if (on) rx_audio_wake();   /* the chain may be asleep with no listener */
    ESP_LOGI(TAG, "%s offset %d Hz%s", on ? "ON" : "off", offset_hz, reverse ? " reverse" : "");
}

bool rtty_rx_is_on(void) { return s_want_on; }

void rtty_rx_status(rtty_rx_status_t *st)
{
    portENTER_CRITICAL(&s_mux);
    *st = s_st;
    portEXIT_CRITICAL(&s_mux);
    st->on = s_want_on;
    st->offset_hz = s_want_off_hz;
    st->reverse = s_want_rev;
}

size_t rtty_rx_text_tail(char *buf, size_t n)
{
    if (!buf || n == 0) return 0;
    portENTER_CRITICAL(&s_mux);
    uint32_t w = s_text_w;
    size_t len = w < TEXT_RING ? w : TEXT_RING;
    if (len > n - 1) len = n - 1;
    for (size_t i = 0; i < len; i++) buf[i] = s_text[(w - len + i) & (TEXT_RING - 1)];
    portEXIT_CRITICAL(&s_mux);
    buf[len] = 0;
    return len;
}

static void flush_line(void)
{
    if (s_line_n == 0) return;
    s_line[s_line_n] = 0;
    ESP_LOGI(TAG, "rx: %s", s_line);
    s_line_n = 0;
}

static void on_char(const rtty_char_t *c, void *user)
{
    (void)user;
    int ch = rtty_baudot_decode(&s_bd, c->code);
    if (ch == 0) return;
    if (ch == '\a') ch = '*';
    portENTER_CRITICAL(&s_mux);
    s_text[s_text_w & (TEXT_RING - 1)] = (char)ch;
    s_text_w++;
    s_st.chars++;
    portEXIT_CRITICAL(&s_mux);
    if (ch == '\r') return;
    if (ch == '\n') { flush_line(); return; }
    s_line[s_line_n++] = (char)ch;
    if (s_line_n >= LOG_LINE) flush_line();
}

static bool start(float fs_hz, int nco_hz)
{
    if (!s_dem) {
        s_dem = heap_caps_calloc(1, sizeof(*s_dem), MALLOC_CAP_SPIRAM);
        if (!s_dem) { ESP_LOGE(TAG, "no PSRAM for the demodulator (%u B)", (unsigned)sizeof(*s_dem)); return false; }
    }
    rtty_demod_cfg_t cfg = {
        .fs_hz = fs_hz, .baud = RTTY_RX_BAUD, .shift_hz = RTTY_RX_SHIFT,
        /* the tone centre's RF offset, moved to where NCO1 put baseband 0 */
        .centre_hz = (float)(s_want_off_hz - nco_hz),
        .reverse = s_want_rev, .squelch = RTTY_RX_SQUELCH, .afc_hz = RTTY_RX_AFC_HZ,
    };
    if (!rtty_demod_init(s_dem, &cfg)) {
        ESP_LOGE(TAG, "demod init refused fs %.0f", (double)fs_hz);
        return false;
    }
    rtty_baudot_init(&s_bd, true);
    s_line_n = 0;
    ESP_LOGI(TAG, "demod at %.0f Hz, baseband centre %+.0f Hz (offset %d, nco %d), %u B",
             (double)fs_hz, (double)cfg.centre_hz, s_want_off_hz, nco_hz, (unsigned)sizeof(*s_dem));
    return true;
}

void rtty_rx_feed(const float *re, const float *im, int n, float fs_hz, int nco_hz)
{
    uint32_t seq = s_want_seq;
    if (!s_want_on) {
        if (s_running) { flush_line(); s_running = false; ESP_LOGI(TAG, "stopped"); }
        return;
    }
    /* (Re)start on a new request, a new rate (decimation follows the mode)
     * or a retune of rx_audio's first mix (mode or CW offset changed). */
    if (!s_running || seq != s_have_seq || fs_hz != s_have_fs || nco_hz != s_have_nco) {
        bool fresh = !s_running || seq != s_have_seq;
        s_have_seq = seq; s_have_fs = fs_hz; s_have_nco = nco_hz;
        s_running = start(fs_hz, nco_hz);
        if (!s_running) { s_want_on = false; return; }
        if (fresh) {
            portENTER_CRITICAL(&s_mux);
            s_st.chars = 0;
            portEXIT_CRITICAL(&s_mux);
        }
    }
    rtty_demod_feed(s_dem, re, im, n, on_char, NULL);

    portENTER_CRITICAL(&s_mux);
    s_st.sig_lvl = s_dem->sig_lvl;
    s_st.present = s_dem->present;
    s_st.afc_hz = s_dem->afc_off;
    s_st.acq_jumps = s_dem->n_acq;
    s_st.fs_hz = fs_hz;
    portEXIT_CRITICAL(&s_mux);
}
