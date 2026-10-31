// JS8 simulation mode. See js8_sim.h for what this proves and what it does
// not - the short version is that the message codec is real and the physical
// layer is not.
//
// Shape of the file, in the order it reads:
//   - the phantom pool
//   - rendering: every line a phantom "sends" comes out of js8_frame_to_text()
//   - injection: the two sinks the real decoder feeds
//   - slot arithmetic, borrowed verbatim from ft8_sim.c's reasoning
//   - idle traffic
//   - answering what we send them
//   - the task
#include "js8_sim.h"

#include "ft8_test.h"          /* ft8_op_mode_get / _slot_ms              */
#include "ft8_tx.h"            /* ft8_tx_get_status - what we just sent   */
#include "ui/ft8_screen.h"     /* ft8_screen_record_decode, ft8_screen_clear */
#include "ft8_pileup.h"
#include "js8_chat.h"
#include "js8_target.h"
#include "js8_submode.h"
#include "storage/settings.h"
#include "cat.h"               /* the dial, for the chat entry's band     */
#include "adif_log.h"          /* adif_log_band_for_freq - the ONE table  */
#include "ft8/js8_message.h"
#include "ft8/js8_text.h"
#include "ui/ui_mode.h"

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "js8_sim";

/* ---- the pool ----------------------------------------------------------
 *
 * Six, which is what fits the stations pane without filling it - the point is
 * to have somebody to tap, not to simulate a contest.
 *
 * ⛔ THE CALLSIGNS MUST PACK INTO 28 BITS, because every directed line here is
 * built by js8_pack_directed() and a callsign it cannot carry returns 0 and
 * the whole message is silently dropped. These are all plain prefix+digit+
 * suffix forms. A compound call would need the COMPOUND frame, which this
 * simulator does not build.
 *
 * ⚠ VK3DEF's QF22 RENDERS AS "QF23", and that is NOT a typo in this table.
 * js8_pack_grid() -> js8_unpack_grid() is not an identity for any grid whose
 * SECOND LETTER IS A-I: exactly 16200 of the 32400 four-character squares,
 * which is the whole southern hemisphere, come back one square north. Swept
 * on the host, all 32400, 2026-10-31. The cause is visible in the packer:
 * `(int)dlat + 90` truncates TOWARD ZERO, which rounds a negative latitude
 * up, while the unpacker subtracts 90 exactly.
 *
 * ⛔ NOT FIXED HERE, AND NOT FIXED AT ALL YET. The packer's own comment
 * says that truncation is deliberate - "the reference assigns a float to an
 * int" - so a real JS8Call may do exactly the same thing, in which case our
 * pack is faithful and changing it would make us the odd station out. The
 * round trip disagreeing says the two halves differ; it does NOT say which
 * one is wrong. varicode.cpp's packGrid settles it and nothing here does.
 *
 * The phantom keeps its southern grid on purpose: the simulator should show
 * what the real render path shows, and hiding this would make it lie.
 *
 * `info` and `status` are what the phantom answers INFO? and STATUS? with.
 * They are free text, because a Directed frame has no field for them - the
 * same reason our own sheet leaves the mailbox family on the free-text path. */
typedef struct {
    const char *call;
    const char *grid;
    const char *info;
    const char *status;
    int   tone_hz;
    int   snr_db;            /* drifts; the report they give us            */
    bool  engaged;           /* mid-exchange with us                       */
    int64_t pend_slot;       /* 0 = nothing pending                        */
    js8_chat_kind_t pend_kind;
    char  pend_text[JS8_TEXT_MAX];
    char  last_text[JS8_TEXT_MAX];   /* what AGN? repeats                  */
} js8_phantom_t;

static js8_phantom_t s_ph[] = {
    { "W1ABC",  "FN42", "QMX+ 5W DIPOLE",   "MONITORING",      700,  -4, false, 0, 0, "", "" },
    { "DL2XYZ", "JO31", "FT991 40W VERT",   "QRV 20M",        1100, -11, false, 0, 0, "", "" },
    { "VK3DEF", "QF22", "IC7300 INV L",     "AWAY TIL 0600Z", 1500, -19, false, 0, 0, "", "" },
    { "JA1GHI", "PM95", "QMX 5W EFHW",      "MONITORING",     1900,  -8, false, 0, 0, "", "" },
    { "EA5JKL", "IM98", "FT710 HEXBEAM",    "QRV ANY BAND",   2300, -15, false, 0, 0, "", "" },
    { "K7MNO",  "CN87", "QMX+ 4W DOUBLET",  "AFK",            2700,  -2, false, 0, 0, "", "" },
};
#define N_PH ((int)(sizeof(s_ph) / sizeof(s_ph[0])))

/* The report a phantom gives drifts, for the reason ft8_sim.c spells out at
 * its own fading code: a number that never moves cannot exercise the code
 * that reacts to it. Nothing here pretends to be propagation. */
#define SIM_SNR_MIN_DB (-22)
#define SIM_SNR_MAX_DB (6)

static void snr_drift(js8_phantom_t *p)
{
    int step = (int)(esp_random() % 5u) - 2;        /* -2..+2 dB */
    p->snr_db += step;
    if (p->snr_db < SIM_SNR_MIN_DB) p->snr_db = SIM_SNR_MIN_DB;
    if (p->snr_db > SIM_SNR_MAX_DB) p->snr_db = SIM_SNR_MAX_DB;
}

/* ---- rendering ---------------------------------------------------------
 *
 * ⭐ EVERY DIRECTED LINE IS RENDERED BY js8_frame_to_text(), never by
 * snprintf here. The frame is packed first and then read back, so what the
 * phantom appears to say is exactly what our own renderer would print for
 * those bytes off the air. A hand-formatted string would be a second copy of
 * the renderer's rules and would drift from it - and the one thing a
 * simulator must not do is teach the operator a message shape the real
 * decoder never produces. */
static bool directed_text(const char *to, const char *from, uint8_t cmd, int num,
                          char *out, size_t cap)
{
    js8_directed_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.to, sizeof(d.to), "%s", to);
    snprintf(d.from, sizeof(d.from), "%s", from);
    d.cmd = cmd;
    d.num = num;

    uint8_t frame[JS8_FRAME_BYTES];
    if (!js8_pack_directed(&d, frame)) {
        ESP_LOGW(TAG, "could not pack %s -> %s cmd %u", from, to, (unsigned)cmd);
        return false;
    }
    return js8_frame_to_text(frame, out, cap);
}

static bool beacon_text(const js8_phantom_t *p, bool is_cq, char *out, size_t cap)
{
    js8_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    hb.is_cq = is_cq;
    hb.bits3 = 0;
    snprintf(hb.call, sizeof(hb.call), "%s", p->call);
    snprintf(hb.grid, sizeof(hb.grid), "%s", p->grid);

    uint8_t frame[JS8_FRAME_BYTES];
    if (!js8_pack_heartbeat(&hb, frame)) return false;
    return js8_frame_to_text(frame, out, cap);
}

/* ---- injection ---------------------------------------------------------
 *
 * ⛔ BOTH SINKS, because the real decoder feeds both and a phantom that
 * reached only one would be a station you can read but not tap, or tap but
 * not read. ft8_test.c's slot loop calls ft8_screen_record_decode() for the
 * stations pane and js8_chat_add() for the conversation; this mirrors that
 * pair and nothing else. It never touches ft8_qso.c, exactly as ft8_sim.c
 * never does - from the QSO machine's point of view a phantom IS a station,
 * because it arrives through the same input a real one would. */
static void inject(js8_phantom_t *p, js8_chat_kind_t kind, const char *text,
                   int64_t slot_sec)
{
    if (!text || !text[0]) return;

    /* A score the decode list will accept. The real one comes from the LDPC
     * stage, which is the stage this simulator skips, so there is no honest
     * number to give - it is fixed rather than dressed up as measured. */
    ft8_screen_record_decode(text, 30, p->snr_db, p->tone_hz, slot_sec, 0);

    /* ⛔ THE STORE HAS TO EXIST, AND ON A DEAD BAND NOTHING ELSE CREATES IT.
     * js8_chat_add() is a silent no-op while s_store is NULL, and the only
     * other caller of js8_chat_init() is inside ft8_test.c's slot loop, in the
     * branch that handles JS8 events from a REAL decode. With the simulator on
     * and no station on the air, that branch never runs - so every phantom
     * reached the stations pane through ft8_screen_record_decode() above and
     * the conversation pane stayed on "Nothing decoded yet". Reported from the
     * glass, 2026-10-31, and the /api/js8 read agreed: used 0, count 0, while
     * the log showed eleven injections.
     *
     * init is idempotent and remembers a failure, so calling it here costs a
     * pointer test per message. */
    js8_chat_init();

    uint32_t dial = cat_get_frequency();
    js8_chat_msg_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.kind      = kind;
    cm.first_utc = slot_sec;
    cm.last_utc  = slot_sec;
    cm.freq_hz   = p->tone_hz;
    cm.frames    = 1;
    cm.snr_db    = JS8_CHAT_NO_SNR;
    cm.proto     = (uint8_t)ft8_op_mode_protocol();
    cm.dial_hz   = dial;
    snprintf(cm.band, sizeof(cm.band), "%s", dial ? adif_log_band_for_freq(dial) : "");
    snprintf(cm.sender, sizeof(cm.sender), "%s", p->call);
    snprintf(cm.text, sizeof(cm.text), "%s", text);
    js8_chat_add(&cm);

    snprintf(p->last_text, sizeof(p->last_text), "%s", text);
    ESP_LOGI(TAG, "injected '%s' (snr=%d tone=%d slot=%lld)",
             text, p->snr_db, p->tone_hz, (long long)slot_sec);
}

/* ---- slot arithmetic ---------------------------------------------------
 *
 * ⛔ NOT `+ 15`. The same trap ft8_sim.c documents at length for FT4 is live
 * here for every JS8 submode but Normal: Turbo is 6 s, Fast 10 s, Slow 30 s,
 * and the engine records a slot as the whole-second truncation of a
 * MILLISECOND boundary. A reply scheduled a flat 15 s out lands in a slot
 * that does not exist, and nothing ever sees it. Work in ms on the engine's
 * own grid, then truncate the way it does. */
static int64_t sim_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int64_t sim_cur_slot(void)
{
    int64_t period = ft8_op_mode_slot_ms();
    if (period <= 0) period = 15000;
    return ((sim_now_ms() / period) * period) / 1000;
}

static int64_t sim_next_slot(int64_t slot_sec)
{
    int64_t period = ft8_op_mode_slot_ms();
    if (period <= 0) period = 15000;
    int64_t ms    = slot_sec * 1000;
    int64_t bound = ((ms + period / 2) / period) * period;   /* nearest grid point */
    return (bound + period) / 1000;
}

/* ---- idle traffic ------------------------------------------------------
 *
 * One message a slot at most, from one phantom. ⛔ NOT ALL SIX EVERY SLOT: a
 * JS8 band that busy does not exist, and the point of the pane is to show a
 * conversation arriving over time. A station that is mid-exchange with us
 * keeps quiet on its own account.
 *
 * The mix is what a real JS8 watering hole looks like - mostly heartbeats,
 * some CQs, and stations talking to each other rather than to us. The last
 * of those is the one that fills the conversation pane with traffic the
 * operator is NOT part of, which is exactly what the filter exists to hide. */
static void idle_traffic(int64_t slot_sec)
{
    int i = (int)(esp_random() % (uint32_t)N_PH);
    js8_phantom_t *p = &s_ph[i];
    if (p->engaged || p->pend_slot) return;

    snr_drift(p);

    char text[JS8_TEXT_MAX];
    uint32_t roll = esp_random() % 100u;

    if (roll < 50) {
        if (!beacon_text(p, false, text, sizeof(text))) return;      /* HB   */
        inject(p, JS8_CHAT_DIRECTED, text, slot_sec);
    } else if (roll < 75) {
        if (!beacon_text(p, true, text, sizeof(text))) return;       /* CQ   */
        inject(p, JS8_CHAT_DIRECTED, text, slot_sec);
    } else {
        /* Two phantoms talking to each other. */
        int j = (int)(esp_random() % (uint32_t)N_PH);
        if (j == i) j = (j + 1) % N_PH;
        static const uint8_t CHATTER[] = {
            JS8_CMD_SNR, JS8_CMD_73, JS8_CMD_SNR_QUERY, JS8_CMD_YES,
        };
        uint8_t cmd = CHATTER[esp_random() % (uint32_t)(sizeof(CHATTER))];
        int num = (cmd == JS8_CMD_SNR) ? s_ph[j].snr_db : JS8_NUM_NONE;
        if (!directed_text(s_ph[j].call, p->call, cmd, num, text, sizeof(text))) return;
        inject(p, JS8_CHAT_DIRECTED, text, slot_sec);
    }
}

/* ---- answering what we send -------------------------------------------- */

static void schedule(js8_phantom_t *p, js8_chat_kind_t kind, const char *text,
                     int64_t our_slot)
{
    snprintf(p->pend_text, sizeof(p->pend_text), "%s", text);
    p->pend_kind = kind;
    p->pend_slot = sim_next_slot(our_slot);
}

/* Build the phantom's answer to one directive.
 *
 * ⚠ THESE ANSWERS ARE OUR BEST READING OF JS8Call, NOT A MEASUREMENT. Nothing
 * here was observed from a real station: the directed replies are what our own
 * renderer produces for the obvious command, and the free-text ones are the
 * shape JS8Call documents. They are good enough to drive the screen, and they
 * are NOT evidence about what a real station sends. The only thing that
 * settles that is a real JS8Call on the air.
 *
 * ⛔ A free-text answer is a FREETEXT chat entry, not a Directed one. INFO,
 * STATUS and HEARING carry text a Directed frame has no field for, so on the
 * air they arrive as a reassembled run - and marking them Directed here would
 * show the operator a row shape the decoder cannot produce. */
static bool build_answer(js8_phantom_t *p, const char *my_call, const char *rest,
                         js8_chat_kind_t *kind_out, char *out, size_t cap)
{
    *kind_out = JS8_CHAT_DIRECTED;

    /* The queries that come back as free text.
     *
     * ⛔ REFUSE ON TRUNCATION, do not let snprintf shorten it. JS8_TEXT_MAX
     * is 48 and these lines carry two callsigns plus free text, so a compound
     * call or a longer info string reaches the limit - and the HEARING? case
     * below shows what silent truncation looks like when it lands on a
     * callsign. A phantom that says nothing is a simulator with a gap; one
     * that says half a word teaches the operator a message the decoder does
     * not produce. */
    if (strcmp(rest, "INFO?") == 0) {
        int n = snprintf(out, cap, "%s %s INFO %s", my_call, p->call, p->info);
        if (n < 0 || (size_t)n >= cap) return false;
        *kind_out = JS8_CHAT_FREETEXT;
        return true;
    }
    if (strcmp(rest, "STATUS?") == 0) {
        int n = snprintf(out, cap, "%s %s STATUS %s", my_call, p->call, p->status);
        if (n < 0 || (size_t)n >= cap) return false;
        *kind_out = JS8_CHAT_FREETEXT;
        return true;
    }
    if (strcmp(rest, "HEARING?") == 0) {
        /* The most recently heard, which is what the directive means. Any of
         * the others - a plausible list is all the screen needs.
         *
         * ⛔ WHOLE CALLSIGNS ONLY, AND THE ROOM IS MEASURED FIRST. The first
         * cut built four calls into a 64-byte list and then snprintf'd the
         * whole line into JS8_TEXT_MAX (48). On air it came out as
         * "...JA1GHI EA5JK" - EA5JKL with its last letter cut off, which is a
         * DIFFERENT STATION, silently. Seen on bench1, 2026-10-31, the first
         * time HEARING? was pressed. js8_message.c already states this rule
         * for the callsign codec ("a truncated callsign is a DIFFERENT
         * station, so it must refuse, not shorten"); it applies to a list of
         * them too. So: append only while a whole call plus its space fits,
         * and stop - a shorter list is honest, a cut one is not. */
        int off = snprintf(out, cap, "%s %s HEARING", my_call, p->call);
        if (off < 0 || (size_t)off >= cap) return false;

        int n = 0;
        for (int i = 0; i < N_PH && n < 4; i++) {
            if (&s_ph[i] == p) continue;
            size_t need = 1 + strlen(s_ph[i].call);        /* the space, too */
            if ((size_t)off + need + 1 > cap) break;       /* +1 for the NUL */
            off += snprintf(out + off, cap - (size_t)off, " %s", s_ph[i].call);
            n++;
        }
        if (!n) return false;                              /* nothing fitted */
        *kind_out = JS8_CHAT_FREETEXT;
        return true;
    }
    if (strcmp(rest, "GRID?") == 0) {
        /* ⚠ FREE TEXT, mirroring what OUR OWN grid macro sends ("THEM US
         * JO65"). JS8_CMD_GRID exists in the vocabulary but carries no
         * locator, so a bare "GRID" would answer the question with nothing.
         * See the comment at js8_send_grid() in ft8_screen_view.c. */
        int n = snprintf(out, cap, "%s %s %.4s", my_call, p->call, p->grid);
        if (n < 0 || (size_t)n >= cap) return false;
        *kind_out = JS8_CHAT_FREETEXT;
        return true;
    }

    /* The rest are one directed frame. */
    uint8_t cmd = JS8_CMD_NONE;
    int     num = JS8_NUM_NONE;

    if (strcmp(rest, "SNR?") == 0) {
        cmd = JS8_CMD_SNR;
        num = p->snr_db;
    } else if (strcmp(rest, "HW CPY?") == 0) {
        /* ⛔ THE INDEX COMES FROM THE LIBRARY, not from a number written
         * here. js8_message.h names only a handful of the 32 commands, and
         * js8_cmd_from_text() is the lookup that already decides what is
         * sendable - so the simulator cannot answer with a command our own
         * encoder would refuse, and no magic index can go stale against the
         * table. */
        cmd = js8_cmd_from_text("YES");
    } else if (strcmp(rest, "QSL?") == 0) {
        cmd = js8_cmd_from_text("QSL");
    } else if (strcmp(rest, "73") == 0 || strcmp(rest, "RR73") == 0 ||
               strcmp(rest, "QSL") == 0) {
        /* They sign off too, and the exchange is over. */
        cmd = JS8_CMD_73;
        p->engaged = false;
    } else if (strcmp(rest, "AGN?") == 0) {
        /* Repeat the last thing they said - which is what AGN? asks for, and
         * is the only answer here that is not newly built. */
        if (!p->last_text[0]) return false;
        snprintf(out, cap, "%s", p->last_text);
        return true;
    } else if (rest[0] == '-' || rest[0] == '+') {
        /* A report from us: acknowledge with their own. */
        cmd = JS8_CMD_SNR;
        num = p->snr_db;
    } else if (strcmp(rest, "YES") == 0 || strcmp(rest, "NO") == 0) {
        return false;                       /* nothing to say back */
    } else {
        return false;                       /* free text, or something we do not model */
    }

    if (cmd == JS8_CMD_NONE) return false;
    return directed_text(my_call, p->call, cmd, num, out, cap);
}

/* Split "TO FROM REST" the way ft8_qso.c's split_msg3() does. Three fields,
 * REST being everything after the second - so "HW CPY?" arrives whole. */
static bool split3(const char *text, char *a, size_t ca, char *b, size_t cb,
                   char *rest, size_t cr)
{
    const char *p = text;
    while (*p == ' ') p++;
    const char *s1 = p;
    while (*p && *p != ' ') p++;
    size_t l1 = (size_t)(p - s1);
    if (l1 == 0 || l1 >= ca) return false;
    memcpy(a, s1, l1); a[l1] = '\0';

    while (*p == ' ') p++;
    const char *s2 = p;
    while (*p && *p != ' ') p++;
    size_t l2 = (size_t)(p - s2);
    if (l2 == 0 || l2 >= cb) return false;
    memcpy(b, s2, l2); b[l2] = '\0';

    while (*p == ' ') p++;
    snprintf(rest, cr, "%s", p);
    return true;
}

/* We just transmitted. Decide who, if anyone, answers.
 *
 * ⛔ READ FROM THE TX STATUS, not from a hook inside the macro sheet. What
 * matters is what actually went out - the sheet, the SNR button, the free-text
 * box and the QSO machine all key the radio, and a phantom that only answered
 * one of them would make the others look broken. ft8_sim.c reads the same
 * place for the same reason. */
static void on_our_burst(const char *tx_text, const char *my_call, int64_t our_slot)
{
    char to[16], from[16], rest[32];
    if (!split3(tx_text, to, sizeof(to), from, sizeof(from), rest, sizeof(rest))) return;

    /* Our CQ or HB: somebody who is not busy comes back. This is the other
     * half of "too few stations" - calling CQ into an empty band proves
     * nothing about the reply path. */
    if (strcmp(to, "CQ") == 0 || strcmp(to, "HB") == 0) {
        for (int i = 0; i < N_PH; i++) {
            js8_phantom_t *p = &s_ph[(int)(esp_random() % (uint32_t)N_PH)];
            (void)i;
            if (p->engaged || p->pend_slot) continue;
            char text[JS8_TEXT_MAX];
            snr_drift(p);
            if (!directed_text(my_call, p->call, JS8_CMD_SNR, p->snr_db,
                               text, sizeof(text))) return;
            p->engaged = true;
            schedule(p, JS8_CHAT_DIRECTED, text, our_slot);
            return;
        }
        return;
    }

    for (int i = 0; i < N_PH; i++) {
        js8_phantom_t *p = &s_ph[i];
        if (strcasecmp(to, p->call) != 0) continue;
        if (p->pend_slot) return;             /* already owes us one */

        snr_drift(p);
        char text[JS8_TEXT_MAX];
        js8_chat_kind_t kind;
        if (!build_answer(p, my_call, rest, &kind, text, sizeof(text))) {
            ESP_LOGI(TAG, "%s has no answer for '%s'", p->call, rest);
            return;
        }
        p->engaged = true;
        schedule(p, kind, text, our_slot);
        ESP_LOGI(TAG, "%s will answer '%s' with '%s'", p->call, rest, text);
        return;
    }
}

/* ---- the task ---------------------------------------------------------- */

static void sim_reset(void)
{
    for (int i = 0; i < N_PH; i++) {
        s_ph[i].engaged   = false;
        s_ph[i].pend_slot = 0;
        s_ph[i].pend_text[0] = '\0';
        s_ph[i].last_text[0] = '\0';
    }
}

static void js8_sim_task(void *arg)
{
    (void)arg;
    bool    was_active   = false;
    bool    warned_call  = false;
    int64_t last_tx_slot = -1;
    int64_t last_idle_slot = -1;

    /* ⛔ STATIC, NOT ON THE STACK. qmx_settings_t is 3636 bytes - measured
     * with the target compiler, not guessed - and this task used to declare
     * one inside the loop. On the 8 KB stack it first shipped with, that one
     * local was 44% of the whole stack before settings_load_all() had taken
     * its own frame, and the task died of a stack-protection fault 31 s into
     * the first run on bench1 (2026-10-31, right after injecting its first
     * phantom). The task is this struct's only user, so a file-static costs
     * nothing and removes the largest consumer whatever the stack size. */
    static qmx_settings_t s;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        settings_load_all(&s);

        /* ⛔ THREE CONDITIONS, and each is a separate fact. The SETTING is the
         * operator's intent. The PAGE being up is why ft8_sim.c gates on it
         * too - phantoms injected into a list nobody is looking at are pure
         * cost. And the PROTOCOL is what divides this simulator from
         * ft8_sim.c: one switch, two simulators, and the protocol decides
         * which one runs so the operator never has a choice to get wrong. */
        bool want = s.sim_mode_en &&
                    ui_mode_get() == UI_MODE_FT8 &&
                    ft8_op_mode_get() == FT8_OP_MODE_JS8;

        if (!want) {
            if (was_active) {
                ESP_LOGI(TAG, "JS8 sim idle (off, not on the JS8 page, or not JS8)");
                was_active = false;
                sim_reset();
                /* Phantoms left on screen after the toggle are tappable, and a
                 * tap transmits. Clearing on the way out is the same rule
                 * ft8_sim.c follows, and for the same reason. */
                ft8_screen_clear();
                ft8_pileup_clear();
            }
            last_tx_slot = -1;
            continue;
        }

        if (!s.my_callsign[0]) {
            if (!warned_call) {
                ESP_LOGW(TAG, "JS8 sim ON but no callsign set - idling");
                warned_call = true;
            }
            continue;
        }
        warned_call = false;

        if (!was_active) {
            ESP_LOGI(TAG, "JS8 sim ON (my_call=%s, %d phantoms)", s.my_callsign, N_PH);
            was_active = true;
            sim_reset();
            /* Real decodes sitting on screen from before the toggle are as
             * confusing as leftover phantoms after it - a station the operator
             * has just been told is not real. Clear on the way IN as well. */
            ft8_screen_clear();
            ft8_pileup_clear();
        }

        char my_call[16];
        size_t n = 0;
        for (; s.my_callsign[n] && n < sizeof(my_call) - 1; n++)
            my_call[n] = (char)toupper((unsigned char)s.my_callsign[n]);
        my_call[n] = '\0';

        int64_t cur = sim_cur_slot();

        /* Did we transmit? Slot-keyed rather than edge-triggered, because this
         * task sleeps in 500 ms steps and an edge can be missed - ft8_sim.c
         * hit exactly that and lost every other burst. */
        char tx_text[40];
        ft8_tx_state_t tx_state = ft8_tx_get_status(tx_text, sizeof(tx_text), NULL);
        if (tx_state == FT8_TX_ACTIVE && cur != last_tx_slot) {
            last_tx_slot = cur;
            if (tx_text[0]) on_our_burst(tx_text, my_call, cur);
        }

        /* Anything owed to us, now that its slot has come round. */
        for (int i = 0; i < N_PH; i++) {
            js8_phantom_t *p = &s_ph[i];
            if (!p->pend_slot || cur < p->pend_slot) continue;
            inject(p, p->pend_kind, p->pend_text, cur);
            p->pend_slot = 0;

            /* A directed SNR? from a phantom is a question we owe an answer
             * to, and the real decode path notes exactly that so the TX bar
             * can prompt. Mirroring it here is what makes the prompt testable
             * at all without a second station. */
            char a[16], b[16], rest[32];
            if (split3(p->pend_text, a, sizeof(a), b, sizeof(b), rest, sizeof(rest)) &&
                strcmp(rest, "SNR?") == 0 && strcasecmp(a, my_call) == 0)
                js8_request_note(p->call, p->tone_hz, cur);
        }

        /* One idle message per slot, at most. */
        if (cur != last_idle_slot) {
            last_idle_slot = cur;
            idle_traffic(cur);
        }
    }
}

void js8_sim_init(void)
{
    /* PSRAM stack, low priority, core 1 - the same placement ft8_sim.c uses.
     *
     * ⛔ 32 KB, NOT THE 8 KB THIS SHIPPED WITH. 8 KB died of a stack
     * protection fault on the first run, and the measured cause - a 3636-byte
     * qmx_settings_t on the stack - is fixed above. This is deliberately NOT
     * sized back down to what the fix makes sufficient: the generous number
     * is the one that survives the next local somebody adds, and the thing
     * this task does on overflow is reboot the board. Still half of
     * ft8_sim.c's 64 KB, which also has to carry a synth and a decode. */
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        js8_sim_task, "js8_sim", 32768, NULL,
        tskIDLE_PRIORITY + 1, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) ESP_LOGE(TAG, "failed to spawn js8_sim_task (rc=%d)", (int)rc);
}
