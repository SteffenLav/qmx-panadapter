/* JS8 free-text TRANSMIT run (J8) - see js8_ftx.h for the contract. */

#include "js8_ftx.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ft8_status.h"
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

/* display_text is 32 bytes and the frame counter has to fit in it with room
 * for "free text, frame NN of NN" - see js8_ftx_start(). */
_Static_assert(JS8_FTX_MAX_FRAMES < 100, "frame counter assumes two digits");

typedef struct {
    ft8_tx_request_t req[JS8_FTX_MAX_FRAMES];
    int   total;
    int   sent;          /* frames whose burst has completed */
    bool  armed;         /* req[sent] is currently armed or on the air */
    bool  active;
    char  text[JS8_JSC_TEXT_MAX * 4];
    /* Characters of `text` carried by each frame, from the encoder rather
     * than re-derived here - two copies of the packing rule would drift, and
     * the packing is not a simple division (both codings are tried per frame
     * and the one carrying more wins, so frames hold different amounts). */
    int   used[JS8_FTX_MAX_FRAMES];
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
    int used[JS8_FTX_MAX_FRAMES] = {0};
    int n = js8_jsc_text_to_frames_used(norm, fr, it, JS8_FTX_MAX_FRAMES, used);
    if (n <= 0) {
        if (out_err) snprintf(out_err, out_err_len, "Nothing sendable in that message");
        return false;
    }

    int tone = audio_freq_hz > 0 ? audio_freq_hz : ft8_tx_pick_tone_hz();

    memset(&s_run, 0, sizeof(s_run));
    s_run.total = n;
    snprintf(s_run.text, sizeof(s_run.text), "%s", norm);
    memcpy(s_run.used, used, sizeof(s_run.used));

    /* ⛔ ENCODE EVERY FRAME NOW. A failure here is a refusal the operator sees
     * with the radio cold; a failure three frames in is a half-sent message
     * and a keyed radio. Same reason ft8_tx_build_request() encodes up front. */
    for (int i = 0; i < n; i++) {
        ft8_tx_request_t *r = &s_run.req[i];
        memset(r, 0, sizeof(*r));
        r->kind           = FT8_TX_KIND_CQ;   /* no target call; not a ladder step */
        r->audio_freq_hz  = tone;
        r->use_parity     = false;            /* consecutive slots, either parity */
        /* ⛔ THE SELECTED SUBMODE, not a hardcoded Normal. Every frame of a
         * multi-frame run must be built for the physical layer the slot loop
         * is about to transmit on - the sync arrays differ, and a run encoded
         * for Normal and sent at Turbo's symbol rate decodes nowhere. */
        r->protocol       = ft8_op_mode_protocol();
        js8_encode(&fr[(size_t)i * JS8_FRAME_BYTES], it[i], r->protocol, r->tones);
        /* What the status line shows for this frame: the WORDS in it, with
         * the frame number in front for how much is left to go.
         *
         * ⭐ THE WORDS, NOT ONLY A COUNT. Ryan KK6DZB, after a real QSO:
         * "rather than just show the sending frames". "3 of 7" says a run
         * is alive and nothing about what is on the air, which is the part
         * an operator is waiting to see go out. The slice comes from the
         * ENCODER's own character counts (js8_jsc_text_to_frames_used), so
         * it is the same packing that was encoded - never a re-pack here,
         * which would be a second implementation of a rule that is not even
         * a simple division: both codings are tried per frame and the one
         * carrying more wins, so frames hold different amounts.
         *
         * ⚠ 32 BYTES TOTAL, and snprintf truncates rather than overflows.
         * "12/12 " is six of them and a frame carries roughly a dozen
         * characters, so the words fit; a truncated word would still beat
         * no word. The old text was "free text, frame N of M" - 26 bytes
         * of the 32 spent saying nothing that changes.
         *
         * (old note, still true) The whole message does
         * not fit, and the frame number is what tells the operator how much
         * is left to go.
         *
         * ⚠ THIS STRING IS THE FRAME COUNTER ON BOTH SURFACES. It becomes
         * tx_text in ft8_tx_get_status(), which the Tab5 label (t_clock_cb)
         * and /api/status both build their TX line from - so it is read by an
         * operator, not by a developer, and "2/7" was too terse to be one.
         * Ryan KK6DZB held a JS8 QSO on v1.18.0 and said he "could not tell
         * if it had transmitted my full message yet". */
        /* ⚠ THE MODULO IS FOR THE COMPILER, not for the arithmetic. n is
         * at most JS8_FTX_MAX_FRAMES (12) - see the loop bound and the
         * static assert below - but -Werror=format-truncation reasons about
         * `int`, so a 10-digit frame number against a 32-byte field is a
         * build failure without it. */
        {
            size_t from = 0;
            for (int k = 0; k < i; k++) from += (size_t)used[k];
            const size_t nlen = strlen(norm);
            size_t take = (from < nlen) ? (size_t)used[i] : 0;
            if (from + take > nlen) take = nlen - from;
            snprintf(r->display_text, sizeof(r->display_text),
                     "%d/%d %.*s", (i + 1) % 100, n % 100,
                     (int)take, norm + from);
        }
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
            /* ⛔ THE COMPLETION MESSAGE IS NOT SET HERE, AND THE FIRST CUT
             * OF IT WAS. ft8_status_set() looked like the right home - it is
             * the one line both surfaces show while idle - but the decode loop
             * writes "RX: capturing..." to that same line every slot, so the
             * message was gone inside a second.
             *
             * MEASURED on the bench, 2026-10-14: a two-frame run finished and
             * /api/status already read "RX: capturing..." at the first poll
             * after it. The operator would have seen nothing, which is the
             * complaint this was meant to answer.
             *
             * ft8_screen_view watches this run go active -> idle on its 1 Hz
             * timer and says so there, in the two places that persist: a toast
             * on the glass and the STICKY web result, the same one a completed
             * QSO uses precisely because it must not expire unasked. */
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
        ft8_status_set("Free text STOPPED at frame %d of %d: %s",
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
    ft8_status_set("Free text cancelled at frame %d of %d",
                   s_run.sent + 1, s_run.total);

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

/* ⭐ THE WORDS ON THE AIR RIGHT NOW, not a frame number.
 *
 * Ryan KK6DZB, 2026-10-13, after a real JS8 QSO: a multi-frame free-text
 * message showed only "free text 3/7" while it went out, so an operator could
 * see that something was being sent but not what. The counts come from the
 * encoder (js8_jsc_text_to_frames_used), so this is a slice of the SAME
 * normalised string that was encoded - never a re-pack, which would be a
 * second implementation of the packing rule and would drift from the first.
 *
 * Returns false when nothing is being sent, so a caller can leave its line
 * alone rather than printing an empty one. */
bool js8_ftx_frame_text(char *out, size_t out_len)
{
    if (out && out_len) out[0] = ' ';
    if (!s_run.active || !out || out_len == 0) return false;
    const int idx = s_run.sent;
    if (idx < 0 || idx >= s_run.total) return false;

    /* Where this frame starts: the sum of what every earlier frame carried. */
    size_t from = 0;
    for (int i = 0; i < idx; i++) from += (size_t)s_run.used[i];
    const size_t len = strlen(s_run.text);
    if (from >= len) return false;

    size_t take = (size_t)s_run.used[idx];
    if (take == 0 || from + take > len) take = len - from;
    if (take >= out_len) take = out_len - 1;
    memcpy(out, s_run.text + from, take);
    out[take] = ' ';
    return true;
}
