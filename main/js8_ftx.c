/* JS8 free-text TRANSMIT run (J8) - see js8_ftx.h for the contract. */

#include "js8_ftx.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ft8_tx.h"
#include "ft8_test.h"
#include "ft8/js8.h"
#include "ft8/js8_jsc.h"
#include "ft8/js8_message.h"

static const char *TAG = "js8_ftx";

/* JS8 Normal is a 15 s slot, and one frame is one slot. The air time an
 * operator is agreeing to is frames x this, which is the number they must see
 * before anything is keyed. */
#define JS8_FTX_SLOT_SECS 15

typedef struct {
    ft8_tx_request_t req[JS8_FTX_MAX_FRAMES];
    int   total;
    int   sent;          /* frames whose burst has completed */
    bool  armed;         /* req[sent] is currently armed or on the air */
    bool  active;
    char  text[JS8_JSC_TEXT_MAX * 4];
} js8_ftx_run_t;

/* Only the LVGL task writes this (js8_ftx_tick, js8_ftx_start are both called
 * from there - see the header), so there is no lock. Readers that are only
 * displaying take a consistent-enough snapshot; nothing acts on it. */
static js8_ftx_run_t s_run;

/* ---- planning, which must not transmit -------------------------------- */

bool js8_ftx_plan(const char *text, int *frames, int *secs,
                  char *normalised, size_t norm_len)
{
    if (frames) *frames = 0;
    if (secs)   *secs   = 0;
    if (normalised && norm_len) normalised[0] = '\0';
    if (!text) return false;

    char norm[JS8_JSC_TEXT_MAX * 4];
    if (js8_jsc_normalize(text, norm, sizeof(norm)) <= 0) return false;
    if (normalised && norm_len) snprintf(normalised, norm_len, "%s", norm);

    uint8_t fr[JS8_FTX_MAX_FRAMES * JS8_FRAME_BYTES];
    uint8_t it[JS8_FTX_MAX_FRAMES];
    int n = js8_jsc_text_to_frames(norm, fr, it, JS8_FTX_MAX_FRAMES);
    if (n <= 0) return false;

    if (frames) *frames = n;
    if (secs)   *secs   = n * JS8_FTX_SLOT_SECS;
    return true;
}

/* ---- starting a run ---------------------------------------------------- */

bool js8_ftx_start(const char *text, int audio_freq_hz,
                   char *out_err, size_t out_err_len)
{
    if (out_err && out_err_len) out_err[0] = '\0';

    if (ft8_op_mode_get() != FT8_OP_MODE_JS8) {
        if (out_err) snprintf(out_err, out_err_len, "Not in JS8 mode");
        return false;
    }
    if (s_run.active) {
        if (out_err) snprintf(out_err, out_err_len,
                              "Already sending (%d/%d)", s_run.sent + 1, s_run.total);
        return false;
    }
    /* ⛔ Never start on top of someone else's transmission. A CQ run or a QSO
     * exchange owns the radio until it is done, and interleaving frames from
     * two senders would put nonsense on the air under our callsign. */
    if (ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_IDLE) {
        if (out_err) snprintf(out_err, out_err_len, "A transmission is already armed");
        return false;
    }

    char norm[sizeof(s_run.text)];
    if (js8_jsc_normalize(text ? text : "", norm, sizeof(norm)) <= 0) {
        if (out_err) snprintf(out_err, out_err_len, "Nothing sendable in that message");
        return false;
    }

    uint8_t fr[JS8_FTX_MAX_FRAMES * JS8_FRAME_BYTES];
    uint8_t it[JS8_FTX_MAX_FRAMES];
    int n = js8_jsc_text_to_frames(norm, fr, it, JS8_FTX_MAX_FRAMES);
    if (n <= 0) {
        if (out_err) snprintf(out_err, out_err_len, "Nothing sendable in that message");
        return false;
    }

    int tone = audio_freq_hz > 0 ? audio_freq_hz : ft8_tx_pick_tone_hz();

    memset(&s_run, 0, sizeof(s_run));
    s_run.total = n;
    snprintf(s_run.text, sizeof(s_run.text), "%s", norm);

    /* ⛔ ENCODE EVERY FRAME NOW. A failure here is a refusal the operator sees
     * with the radio cold; a failure three frames in is a half-sent message
     * and a keyed radio. Same reason ft8_tx_build_request() encodes up front. */
    for (int i = 0; i < n; i++) {
        ft8_tx_request_t *r = &s_run.req[i];
        memset(r, 0, sizeof(*r));
        r->kind           = FT8_TX_KIND_CQ;   /* no target call; not a ladder step */
        r->audio_freq_hz  = tone;
        r->use_parity     = false;            /* consecutive slots, either parity */
        r->protocol       = FTX_PROTOCOL_JS8;
        js8_encode(&fr[(size_t)i * JS8_FRAME_BYTES], it[i], r->tones);
        /* What the status line shows for this frame. The whole message does
         * not fit, and the frame number is what tells the operator how much
         * is left to go. */
        snprintf(r->display_text, sizeof(r->display_text),
                 "free text %d/%d", i + 1, n);
    }

    char err[64];
    if (!ft8_tx_arm(&s_run.req[0], err, sizeof(err))) {
        memset(&s_run, 0, sizeof(s_run));
        if (out_err) snprintf(out_err, out_err_len, "%s", err);
        return false;
    }
    s_run.active = true;
    s_run.armed  = true;

    ESP_LOGI(TAG, "free text armed: %d frame(s), %d s, %d Hz: '%s'",
             n, n * JS8_FTX_SLOT_SECS, tone, norm);
    return true;
}

/* ---- running it -------------------------------------------------------- */

void js8_ftx_tick(void)
{
    if (!s_run.active) return;

    ft8_tx_state_t st = ft8_tx_get_status(NULL, 0, NULL);

    if (s_run.armed) {
        /* Still armed or on the air - nothing to do until it lands. */
        if (st != FT8_TX_IDLE) return;
        /* Back to idle: that frame has been sent. */
        s_run.armed = false;
        s_run.sent++;
        if (s_run.sent >= s_run.total) {
            ESP_LOGI(TAG, "free text complete: %d frame(s) sent", s_run.sent);
            memset(&s_run, 0, sizeof(s_run));
            return;
        }
    }

    /* ⚠ The next frame is armed the moment TX goes idle, NOT on a timer. The
     * burst ends about 12.6 s into a 15 s slot, so this lands with roughly two
     * seconds of lead on the next boundary - enough for ft8_tx_arm()'s mode
     * check, and the same margin the CQ run has always run on. */
    char err[64];
    if (!ft8_tx_arm(&s_run.req[s_run.sent], err, sizeof(err))) {
        /* ⛔ SAY SO AND STOP. A run that silently drops its tail sends a
         * truncated message that reads as complete at the far end, which is
         * worse than an obvious failure. */
        ESP_LOGW(TAG, "free text stopped at frame %d/%d: %s",
                 s_run.sent + 1, s_run.total, err);
        memset(&s_run, 0, sizeof(s_run));
        return;
    }
    s_run.armed = true;
}

void js8_ftx_cancel(void)
{
    if (!s_run.active) return;
    ESP_LOGI(TAG, "free text cancelled at frame %d/%d", s_run.sent + 1, s_run.total);

    /* Disarm what has not started; wind down anything already on the air so
     * the radio is never left keyed. One of the two is a no-op. */
    ft8_tx_disarm();
    ft8_tx_request_abort();
    memset(&s_run, 0, sizeof(s_run));
}

bool js8_ftx_active(int *sent, int *total)
{
    if (sent)  *sent  = s_run.sent;
    if (total) *total = s_run.total;
    return s_run.active;
}

void js8_ftx_status(char *out, size_t out_len)
{
    if (!out || !out_len) return;
    if (!s_run.active) { out[0] = '\0'; return; }
    snprintf(out, out_len, "free text %d/%d", s_run.sent + 1, s_run.total);
}
