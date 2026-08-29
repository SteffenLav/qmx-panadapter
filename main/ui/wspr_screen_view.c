/* The WSPR page. See wspr_screen_view.h and docs/wspr-ui-design.md. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "lvgl.h"
#include "esp_attr.h"      /* EXT_RAM_BSS_ATTR on the row snapshot */

#include "ui.h"
#include "ui_theme.h"
#include "wspr_screen_view.h"
#include "wspr_spots.h"
#include "net/wsprnet.h"
#include "wspr_rx.h"
#include "cat.h"
#include "esp_heap_caps.h"
#include "util/dxcc.h"
#include "wspr_tx.h"
#include "storage/settings.h"
#include "wspr_sim.h"

/* JetBrains Mono, already compiled in for the QMX terminal page (#147). The
 * spot list is space-padded columns of short tokens, and in a PROPORTIONAL font
 * those do not line up - the header would sit visibly off its own rows. Reusing
 * the font that is already in the binary costs nothing and is the difference
 * between a table and a mess. */
LV_FONT_DECLARE(qmx_mono_25);

/* Duplicated from ui.c / ft8_screen_view.c, which already each carry their own
 * copy. Following the existing pattern rather than introducing a shared header
 * as a side effect of adding a page - but all three must move together. */
#define TOP_BAR_H     60
#define BOTTOM_BAR_H  36

#define MID_Y   TOP_BAR_H
#define MID_H   (720 - TOP_BAR_H - BOTTOM_BAR_H)
#define MID_W   1280
/* ⭐ 372, NOT 320, TO FIT "MODE: WSPR" AT 48 pt. The header is ~336 px wide
 * and starts at x=16, so 320 left it running out of the panel and over the
 * waterfall. The operator accepted that overlap once ("the wf can run under
 * it") but only because the alternative offered then was a smaller font; given
 * a wider panel he would rather it simply fit. What the panel takes, the
 * decode table gives back - see ROW_FMT. */
#define LEFT_W  372

/* Rows the list can show at once. The pane is MID_H tall and a mono-25 row plus
 * line spacing is ~31 px, so 18 is a screenful including the cycle headers -
 * this is a screenful with the header - NOT the ring's capacity. Deliberately
 * bounded: the snapshot is copied onto the caller's buffer and this runs on
 * taskLVGL, where CLAUDE.md keeps a list of crashes caused by kB-scale locals
 * (the v0.20.1 pounce crash was an 11 KB array on exactly this task). */
/* Right-hand area, split as the operator asked: the captured window's
 * waterfall on top, the decode log underneath. */
#define RIGHT_X    (LEFT_W + 8)
#define RIGHT_W    (MID_W - RIGHT_X - 8)
#define WF_Y       6
#define WF_H       200
#define AXIS_Y     (WF_Y + WF_H + 2)
#define AXIS_H     22
#define LIST_Y     (AXIS_Y + AXIS_H + 8)

#define AXIS_TICKS 7      /* 1350..1650 every 50 Hz */
/* Rows RENDERED, not rows visible - the pane shows about a dozen and scrolls
 * through the rest, which is what the operator asked for ("like FT8/4"). 64 is
 * a quarter of the 256-entry ring: several screenfuls to scroll back through
 * without rendering a log nobody will reach. */
#define VIEW_ROWS  64

static lv_obj_t *s_container;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_dial;
static lv_obj_t *s_lbl_cycle;
static lv_obj_t *s_bar_cycle;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_heard;

static lv_obj_t *s_dd_dial;
static lv_obj_t *s_btn_tx;
static lv_obj_t *s_lbl_tx;

/* THE standard WSPR dial for each band - the whole list, not a range.
 *
 * WSPR lives in a 200 Hz sub-band per band, and a station outside it is heard
 * by nobody. A free-entry keypad would therefore hand the operator a way to be
 * silently wrong, which is the exact error class CLAUDE.md keeps recording; a
 * list of the real ones cannot be. These are USB dial frequencies - the
 * transmission itself sits ~1400-1600 Hz above each. */
/* The type and the radio-availability accessor live in the header now, because
 * the band-hop tick list needs the same table and the same filtering. */
static const wspr_band_t kBands[] = {
    { "160", "160 m  1.836600",  1836600u },
    { "80",  "80 m   3.568600",  3568600u },
    { "60",  "60 m   5.287200",  5287200u },
    { "40",  "40 m   7.038600",  7038600u },
    { "30",  "30 m  10.138700", 10138700u },
    { "20",  "20 m  14.095600", 14095600u },
    { "17",  "17 m  18.104600", 18104600u },
    { "15",  "15 m  21.094600", 21094600u },
    { "12",  "12 m  24.924600", 24924600u },
    { "10",  "10 m  28.124600", 28124600u },
    { "6",   "6 m   50.293000", 50293000u },
};
#define N_BANDS ((int)(sizeof(kBands) / sizeof(kBands[0])))

const wspr_band_t *wspr_bands(int *out_count)
{
    if (out_count) *out_count = N_BANDS;
    return kBands;
}

int wspr_bands_available(uint8_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int nradio = 0;
    const cat_band_entry_t *radio = cat_get_band_list(&nradio);
    int n = 0;
    for (int i = 0; i < N_BANDS && n < max; i++) {
        if (nradio > 0) {
            int have = 0;
            for (int r = 0; r < nradio; r++)
                if (!strcmp(radio[r].name, kBands[i].name)) { have = 1; break; }
            if (!have) continue;
        }
        out[n++] = (uint8_t)i;
    }
    return n;
}

/* Up here rather than beside the first ESP_LOGI that used it: band hopping logs
 * the dial change it makes, and that code sits well above the old site. */
static const char *TAG = "wspr_view";

/* Duty is a CYCLING VALUE, per docs/wspr-ui-design.md: WSPR asks "what
 * fraction of slots", never "transmit now". 0 is a legitimate state - enabled
 * but silent - while setting up. */
/* The ONLY legal duty values, and now shared with the settings drawer so the
 * two cannot offer different sets. Exported through wspr_screen_view.h. */
const uint8_t kDuty[] = { 0, 10, 20, 33, 50 };
#define N_DUTY ((int)(sizeof(kDuty) / sizeof(kDuty[0])))

/* Which kBands entries this radio can reach, in table order. Built when the
 * page is constructed and refreshed whenever CAT reports a band list, because
 * at boot the page can be built before the radio has answered. */
static uint8_t s_avail[16];
static int     s_navail;

/* The picker lists only the bands the radio has, so its selection index is into
 * s_avail[], never into kBands[] directly. Getting that wrong would silently
 * tune the wrong band. */
static void rebuild_dial_options(void)
{
    if (!s_dd_dial) return;
    char opts[N_BANDS * 20];
    size_t used = 0;
    opts[0] = 0;
    for (int k = 0; k < s_navail; k++) {
        used += (size_t)snprintf(opts + used, sizeof(opts) - used, "%s%s",
                                 k ? "\n" : "", kBands[s_avail[k]].label);
    }
    lv_dropdown_set_options(s_dd_dial, opts);
}

static void dial_changed_cb(lv_event_t *e)
{
    uint16_t k = lv_dropdown_get_selected(lv_event_get_target(e));
    if (k >= (uint16_t)s_navail) return;
    const int i = s_avail[k];
    settings_set_wspr_dial_hz(kBands[i].dial_hz);
    /* Forced: the ordinary setter shares a 200 ms rate limit with the CAT
     * poll, and a band change the operator just asked for must not be the
     * write that gets dropped. */
    wspr_rx_wf_floor_reset();   /* the new band has its own noise floor */
    cat_set_frequency_forced(kBands[i].dial_hz);
}

/* ---- THE LOWER HALF OF THE LEFT PANEL ------------------------------------
 *
 * Everything below the TX buttons answers a question the decode list cannot:
 * how is the band DOING, rather than what did it just say.
 *
 *   BEST DX      - the furthest station this session. WSPR's whole point is
 *                  how far a few milliwatts got, and that answer otherwise
 *                  scrolls off the list within a few cycles.
 *   HISTORY      - stations per cycle, oldest left. A snapshot cannot tell an
 *                  opening band from a closing one; a row of bars can.
 *   WSPRNET      - what would be published, and whether it can be.
 *   BAND HOP     - which bands to rotate through, ticked off.
 *
 * All four read state that already exists. None of them measures anything new,
 * which is deliberate: this is presentation, and the measuring belongs in the
 * decoder where it can be validated against wsprd.
 */
#define EX_X      16
#define EX_W      (LEFT_W - 32)
#define EX_DX_Y   322
#define EX_HIST_Y 404
#define EX_NET_Y  470
/* ⚠ BAND HOP SITS AT THE BOTTOM, and the gap above it is not slack.
 * The wsprnet line above wraps to TWO lines once the counts reach two digits
 * ("wsprnet: off - 12 of 25 calls confirmed"), and at 506 the second line
 * printed straight through the BAND HOP heading. Anchored to the panel's
 * bottom instead of stacked below its neighbour, so a line that grows can
 * never reach it. */
#define EX_HOP_Y  (MID_H - 90)

#define HIST_BARS  WSPR_CYCLE_HISTORY
#define HIST_BAR_W 6
#define HIST_GAP   1
#define HIST_H     30

static lv_obj_t *s_lbl_dx;
static lv_obj_t *s_hist_bar[HIST_BARS];
static lv_obj_t *s_lbl_net;
static lv_obj_t *s_hop_cb[16];
static uint8_t   s_hop_band[16];   /* kBands index behind each checkbox */
static int       s_hop_n;
static lv_obj_t *s_btn_hop;        /* opens the picker */
static lv_obj_t *s_lbl_hop;        /* says which bands are ticked */
static lv_obj_t *s_hop_modal;      /* NULL when closed */

static void hop_button_refresh(void);

static void hop_toggled_cb(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    uint16_t mask = 0;
    for (int i = 0; i < s_hop_n; i++) {
        if (s_hop_cb[i] && lv_obj_has_state(s_hop_cb[i], LV_STATE_CHECKED))
            mask |= (uint16_t)(1u << s_hop_band[i]);
    }
    (void)cb;
    settings_set_wspr_hop_mask(mask);
    /* Hopping is ON exactly when more than one band is ticked. A separate
     * enable switch would be a second thing to get wrong, and "one band ticked"
     * already means "stay there" - which is the same as off. */
    settings_set_wspr_hop_en(__builtin_popcount(mask) > 1);
    hop_button_refresh();
}

/* ---- THE BAND-HOP PICKER ----------------------------------------------
 *
 * A full-screen window with finger-sized rows, opened from the panel button.
 * The panel itself only ever shows WHICH bands are ticked; choosing them is a
 * deliberate act that gets room to happen in.
 *
 * ⚠ THE LIST IS BUILT HERE, ON OPEN, NOT AT PAGE INIT. That is not tidiness:
 * wspr_bands_available() filters against cat_get_band_list(), and CAT does not
 * answer until ~17 s after boot. Built during init the filter always saw an
 * empty radio list and silently offered every band in the table on every
 * radio. Built on open, the radio has long since answered.
 */
static void hop_modal_close(void)
{
    if (!s_hop_modal) return;
    lv_obj_del(s_hop_modal);
    s_hop_modal = NULL;
    for (int i = 0; i < 16; i++) s_hop_cb[i] = NULL;
    s_hop_n = 0;
    hop_button_refresh();
}

static void hop_close_cb(lv_event_t *e) { (void)e; hop_modal_close(); }

static void hop_modal_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_hop_modal) return;

    s_hop_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_hop_modal, 1280, 720);
    lv_obj_set_pos(s_hop_modal, 0, 0);
    lv_obj_set_style_bg_color(s_hop_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_hop_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_hop_modal, 0, 0);
    lv_obj_clear_flag(s_hop_modal, LV_OBJ_FLAG_SCROLLABLE);
    /* A scrim you dismiss, not a control you press - so the mouse pointer
     * stays white over it (ui_theme.h). */
    lv_obj_add_flag(s_hop_modal, UI_FLAG_NOT_HOT);
    lv_obj_add_flag(s_hop_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hop_modal, hop_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_hop_modal);
    lv_obj_set_size(panel, 780, 660);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Presses inside the panel must not reach the scrim's dismiss handler. */
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Band hop");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);   /* 36 and 40 are not built into this image; 32 is */
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(panel);
    /* Says how it works, in the order the questions actually arise. The
     * operator asked all three of these, which is the sign the window was
     * showing controls without explaining them:
     *   - what does a tick DO?       one cycle each, so two minutes per band
     *   - what if I tick only one?   nothing hops; the band selector wins
     *   - so what should I do?       pick at least two - say it plainly */
    lv_label_set_text(hint,
        "Pick at least two bands.\n"
        "The radio moves to the next ticked band every cycle -\n"
        "two minutes on each - in the order listed below, then wraps.\n"
        "\n"
        "Fewer than two ticked means no hopping at all: the band\n"
        "selector on the page decides, and the radio stays there.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 62);

    qmx_settings_t hs;
    settings_load_all(&hs);
    s_hop_n = wspr_bands_available(s_hop_band, (int)sizeof(s_hop_band));

    if (s_hop_n == 0) {
        /* Says which of the two it is. "No bands" with the radio off reads as
         * a broken feature; it is a disconnected radio. */
        lv_obj_t *none = lv_label_create(panel);
        lv_label_set_text(none, cat_is_ready()
            ? "The radio reported no bands."
            : "Waiting for the radio - connect the QMX and reopen this.");
        lv_obj_set_style_text_font(none, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(none, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_align(none, LV_ALIGN_TOP_LEFT, 0, 238);
    }

    /* Two columns of finger-sized rows. 64 px pitch and a 32 px tick box: the
     * grid this replaces used 30 px rows and a 20 px box, which is what made
     * it unusable with a finger. */
    for (int i = 0; i < s_hop_n; i++) {
        lv_obj_t *cb = lv_checkbox_create(panel);
        lv_checkbox_set_text(cb, kBands[s_hop_band[i]].name);
        lv_obj_set_style_text_font(cb, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(cb, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_pad_all(cb, 8, 0);
        lv_obj_set_style_width(cb, 32, LV_PART_INDICATOR);
        lv_obj_set_style_height(cb, 32, LV_PART_INDICATOR);
        lv_obj_align(cb, LV_ALIGN_TOP_LEFT, (i % 2) * 340, 238 + (i / 2) * 64);
        if (hs.wspr_hop_mask & (1u << s_hop_band[i]))
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        lv_obj_add_event_cb(cb, hop_toggled_cb, LV_EVENT_VALUE_CHANGED, NULL);
        s_hop_cb[i] = cb;
    }

    lv_obj_t *done = lv_btn_create(panel);
    lv_obj_set_size(done, 200, 64);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(done, 8, 0);
    lv_obj_add_event_cb(done, hop_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "Done");
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_28, 0);
    lv_obj_center(dl);
}

/* Opened from the settings drawer now that the button has moved off this page.
 * The modal parents to lv_layer_top(), so it is indifferent to whether the
 * WSPR page is visible - and it rebuilds its band list every time it opens,
 * which is what makes it correct when the radio answered late. */
void wspr_screen_view_open_hop_picker(void)
{
    hop_modal_open_cb(NULL);
}

/* The panel button says what is ticked, so the picker never has to be opened
 * just to find out. */
static void hop_button_refresh(void)
{
    if (!s_lbl_hop) return;
    qmx_settings_t hs;
    settings_load_all(&hs);

    uint8_t bands[16];
    int n = wspr_bands_available(bands, (int)sizeof(bands));
    char t[64];
    size_t off = 0;
    int ticked = 0;
    for (int i = 0; i < n && off < sizeof(t) - 8; i++) {
        if (!(hs.wspr_hop_mask & (1u << bands[i]))) continue;
        ticked++;
        off += (size_t)snprintf(t + off, sizeof(t) - off, "%s%s",
                                ticked > 1 ? " " : "", kBands[bands[i]].name);
    }
    /* ⚠ The names only fit while there are few of them. EX_W is 340 px and
     * montserrat_28 averages ~15 px a character, so about 22 characters -
     * "160 80 60 40 30 20" is 18 and fits, but a QMX+ with eleven bands ticked
     * would be 33 and run off the button. Past the limit it says how many
     * instead, which is the useful summary anyway; the picker has the detail. */
    if (ticked == 0)        snprintf(t, sizeof(t), "Band hop: off");
    else if (off > 22)      snprintf(t, sizeof(t), "%d bands", ticked);
    lv_label_set_text(s_lbl_hop, t);
}


static lv_obj_t *ex_heading(const char *text, int y)
{
    lv_obj_t *l = lv_label_create(s_container);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(l, EX_X, y);
    return l;
}

static void build_left_extras(void)
{
    /* ---- best DX ---- */
    ex_heading("BEST DX", EX_DX_Y);
    s_lbl_dx = lv_label_create(s_container);
    lv_label_set_text(s_lbl_dx, "-");
    lv_obj_set_style_text_font(s_lbl_dx, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_dx, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_width(s_lbl_dx, EX_W);
    lv_obj_set_pos(s_lbl_dx, EX_X, EX_DX_Y + 22);

    /* ---- cycle history ---- */
    ex_heading("STATIONS PER CYCLE", EX_HIST_Y);
    for (int i = 0; i < HIST_BARS; i++) {
        lv_obj_t *b = lv_obj_create(s_container);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, HIST_BAR_W, 2);
        lv_obj_set_pos(b, EX_X + i * (HIST_BAR_W + HIST_GAP),
                       EX_HIST_Y + 22 + HIST_H - 2);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_BORDER), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_add_flag(b, UI_FLAG_NOT_HOT);
        s_hist_bar[i] = b;
    }

    /* ---- wsprnet ---- */
    s_lbl_net = lv_label_create(s_container);
    lv_label_set_text(s_lbl_net, "wsprnet: -");
    /* ⛔ NOT 18. This project settled long ago that 18 is below what is
     * readable on this screen at arm's length, and it went in here anyway. */
    lv_obj_set_style_text_font(s_lbl_net, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_net, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_width(s_lbl_net, EX_W);
    lv_obj_set_pos(s_lbl_net, EX_X, EX_NET_Y);

    /* ---- band hop ----
     *
     * ⭐ A BUTTON, NOT A GRID OF CHECKBOXES IN THE PANEL, AND FOR TWO REASONS.
     *
     * The obvious one is the operator's: eleven 20 px checkboxes crammed into
     * whatever height was left at the bottom of the panel cannot be hit with a
     * finger. It was a list you could read and not use.
     *
     * The one that would have gone unnoticed is worse. The tick list is
     * filtered to the bands the RADIO reports (wspr_bands_available ->
     * cat_get_band_list), but it was built in this init function, which runs
     * during boot - and CAT does not come up until about 17 s. So nradio was
     * always 0, the filter never applied, and every band in the table was
     * offered on every radio. Exactly the shape of the CW-pitch bug CLAUDE.md
     * records: a value read once, too early, and never revisited.
     *
     * Building the list when the WINDOW OPENS fixes both at once - by then the
     * radio has long since answered. */
/* BAND HOP moved to the settings drawer (operator, 2026-08-28), for the same
     * reason Duty did: choosing WHICH BANDS to rotate through is a decision made
     * once for a session, not a control reached while watching spots arrive. The
     * picker itself is unchanged and still parents to lv_layer_top(), so it
     * opens correctly from the drawer with this page hidden. */
}

/* ---- BAND HOPPING --------------------------------------------------------
 *
 * ⛔ THERE IS NO SAFE MOMENT INSIDE A CYCLE TO CHANGE BAND. The capture arms on
 * the even minute and runs the FULL 120 s, so a dial change at any point during
 * it corrupts that window - the first half would be one band and the rest
 * another, and the decoder would be handed something no station transmitted.
 *
 * So the hop happens in the last few seconds BEFORE a boundary, which is the
 * only gap there is: the previous capture has finished and the next has not
 * armed. Three seconds is comfortably more than a QMX takes to retune and
 * comfortably less than the gap.
 *
 * Hopping is ON exactly when more than one band is ticked - "one band ticked"
 * already means "stay there", so a separate enable switch would only be a
 * second thing to get wrong.
 */
#define HOP_LEAD_SEC 3

static int64_t s_hop_done_cycle = -1;

static void hop_maybe(void)
{
    qmx_settings_t hs;
    settings_load_all(&hs);
    if (!hs.wspr_hop_en) return;

    const uint16_t mask = hs.wspr_hop_mask;
    if (__builtin_popcount(mask) < 2) return;

    const time_t now = time(NULL);
    if (now < 1600000000) return;                /* clock not set yet */
    const int64_t next_cycle = (int64_t)(now / 120) + 1;
    if ((now % 120) < (120 - HOP_LEAD_SEC)) return;
    if (s_hop_done_cycle == next_cycle) return;  /* already hopped for it */

    /* Next ticked band AFTER the current one, wrapping - so the rotation is the
     * table's order and an operator can predict where it goes next. */
    int cur = -1;
    for (int i = 0; i < N_BANDS; i++)
        if (kBands[i].dial_hz == hs.wspr_dial_hz) { cur = i; break; }

    int pick = -1;
    for (int step = 1; step <= N_BANDS; step++) {
        const int i = (cur < 0 ? 0 : (cur + step) % N_BANDS);
        if (!(mask & (1u << i))) continue;
        /* ⚠ A stored mask can name a band this RADIO does not have - the mask
         * outlives a change of radio, and settings.h says why it is not
         * silently pruned. Skip it here rather than tuning somewhere the
         * hardware cannot filter. */
        int reachable = 0;
        for (int k = 0; k < s_navail; k++) if (s_avail[k] == i) { reachable = 1; break; }
        if (!reachable) continue;
        pick = i;
        break;
    }
    if (pick < 0 || kBands[pick].dial_hz == hs.wspr_dial_hz) {
        s_hop_done_cycle = next_cycle;
        return;
    }

    s_hop_done_cycle = next_cycle;
    settings_set_wspr_dial_hz(kBands[pick].dial_hz);
    wspr_rx_wf_floor_reset();   /* the new band has its own noise floor */
    cat_set_frequency_forced(kBands[pick].dial_hz);
    if (s_dd_dial) {
        for (int k = 0; k < s_navail; k++)
            if (s_avail[k] == pick) { lv_dropdown_set_selected(s_dd_dial, (uint16_t)k); break; }
    }
    ESP_LOGI(TAG, "band hop -> %s m (%lu Hz) for the cycle starting in %llds",
             kBands[pick].name, (unsigned long)kBands[pick].dial_hz,
             (long long)(120 - (now % 120)));
}

static void refresh_left_extras(void)
{
    hop_maybe();

    /* Best DX. An ACCESSOR, not a snapshot - see wspr_spots.h for why a 10 KB
     * copy must not land on taskLVGL. */
    if (s_lbl_dx) {
        wspr_spot_t dx;
        char t[64];
        if (wspr_spots_best_dx(&dx) && dx.km >= 0) {
            /* SPELLED OUT here, unlike the table's COUNTRY column. That
             * column is one of ten on a fixed-width line and has to fall back
             * to the alpha-3; this line has the whole panel width to itself,
             * so "Germany" beats "DEU" with nothing to trade for it. Falls
             * back the same way when the callsign is not in the DXCC table. */
            const char *full = dxcc_lookup(dx.call);
            const char *where = (full && full[0]) ? full
                              : (dx.cty[0] ? dx.cty : dx.grid);
            snprintf(t, sizeof(t), "%s  %s\n%ld km  %d dBm",
                     dx.call, where,
                     (long)dx.km, (int)dx.power_dbm);
        } else {
            snprintf(t, sizeof(t), "-");
        }
        lv_label_set_text(s_lbl_dx, t);
    }

    /* Stations per cycle. Scaled to the busiest cycle held rather than to a
     * fixed ceiling: what matters is the SHAPE - rising or falling - and a fixed
     * scale would flatten a quiet band into nothing. */
    {
        uint8_t h[HIST_BARS];
        int n = wspr_rx_cycle_history(h, HIST_BARS);
        int peak = 1;
        for (int i = 0; i < n; i++) if (h[i] > peak) peak = h[i];
        for (int i = 0; i < HIST_BARS; i++) {
            if (!s_hist_bar[i]) continue;
            /* Oldest at the left, so a partly-filled history grows rightwards
             * the way the decode list does. */
            int v = (i < n) ? h[i] : -1;
            int px = (v <= 0) ? 2 : 2 + (v * (HIST_H - 2)) / peak;
            lv_obj_set_size(s_hist_bar[i], HIST_BAR_W, px);
            lv_obj_set_pos(s_hist_bar[i], EX_X + i * (HIST_BAR_W + HIST_GAP),
                           EX_HIST_Y + 22 + HIST_H - px);
            uint32_t c = (v < 0)  ? UI_COLOR_BORDER          /* no cycle yet */
                       : (v == 0) ? 0x553333                 /* heard nothing */
                                  : UI_COLOR_SUCCESS_BORDER;
            lv_obj_set_style_bg_color(s_hist_bar[i], lv_color_hex(c), 0);
        }
    }

    /* ⛔ THIS SAID "off" AS A STRING LITERAL, AND WENT ON SAYING IT AFTER THE
     * UPLOADER WAS BUILT AND PUBLISHING. The operator watched his own spots
     * appear on wsprnet.org while this line told him it was switched off. It
     * was written when upload genuinely did not exist and was honest then;
     * nothing tied it to the truth afterwards.
     *
     * It now asks the uploader. Third time in one day that a thing was added
     * and its surface left behind (wspr_en on the wrong endpoint, wspr_net_en
     * missing from /api/settings, and this) - a status line must READ state,
     * never restate what someone believed when they typed it.
     *
     * The confirmed count stays, because it is the part the operator cannot
     * get anywhere else: how many of the calls heard are eligible under the
     * heard-more-than-once rule that gates publication. */
    if (s_lbl_net) {
        char t[96];
        const int rpt = wspr_spots_repeat_calls();
        const int all = wspr_spots_unique_calls();
        /* ⚠ TWO LINES, AND THE WIDTH IS PART OF THE CONTRACT. The panel is
         * 340 px at 22 pt - about 28 characters - and this label sits directly
         * above the BAND HOP heading. "wsprnet: on - 8 sent, 3 waiting" over
         * "18 of 25 calls confirmed" ran to THREE wrapped lines and printed
         * through the heading below, which is the same collision the wsprnet
         * line already caused once when its counts reached double figures.
         * Both halves are kept short at the source rather than trimmed here:
         * see the note beside s_status in wsprnet.c. */
        snprintf(t, sizeof(t), "wsprnet: %s\n%d/%d confirmed",
                 wsprnet_status(), rpt, all);
        lv_label_set_text(s_lbl_net, t);
    }
}

static void tx_toggle_cb(lv_event_t *e)
{
    (void)e;
    qmx_settings_t st;
    settings_load_all(&st);
    settings_set_wspr_tx_en(!st.wspr_tx_en);
}

/* duty_cycle_cb moved to the settings drawer with its button (2026-08-28). */
static lv_obj_t *s_list;           /* right pane, one label per line */
static lv_obj_t *s_lbl_rows;
static lv_obj_t *s_wf_canvas;

static uint8_t  *s_wf_buf;      /* RGB565 canvas pixels */
static uint8_t  *s_wf_data;     /* WSPR_WF_HIST_ROWS x WSPR_WF_COLS, NEWEST ROW FIRST */
static uint32_t  s_wf_seen;

/* First logging in this file: the dial push is the one thing here that
 * silently changes the radio, so it says what it did and why. */


static int   s_last_spot_count = -1;
static char  s_last_status[48];

/* ---- THE STORED DIAL HAS TO BE PUSHED TO THE RADIO -------------------
 *
 * ⛔ IT USED TO BE PUSHED ONLY BY A TAP ON THE PICKER. Nothing re-applied it on
 * page entry, at boot, after a QMX power cycle, or on leaving simulation - so
 * the device could sit on the WSPR page with 20 m stored while the radio was on
 * 7.074 MHz, quietly decoding a 200 Hz slice of the FT8 calling frequency.
 * Observed exactly that on 2026-08-24, and again when a QMX power cycle brought
 * the radio back on 30 m mid-session.
 *
 * ⛔ AND IT CANNOT SIMPLY BE PUSHED AT PAGE-ENTRY TIME. CAT link-up is ~17 s
 * after boot, so an immediate write often has nowhere to go - this project
 * already shipped that bug once, where the CW-pitch value was written at ~4.5 s
 * and went nowhere on EVERY boot. So the push stays PENDING until
 * cat_is_ready() and then fires once.
 *
 * ⛔ AND IT MUST NOT FIGHT THE OPERATOR. Re-pushing continuously would drag the
 * radio back every time someone deliberately tuned off the sub-band. So this is
 * a BOUNDED ONE-SHOT armed by three discrete events - entering the page, CAT
 * coming back (which is what a QMX power cycle looks like from here), and
 * simulation being switched off - and it gives up rather than surprising
 * anyone minutes later. */
#define DIAL_PUSH_TRIES 60          /* ~60 s: comfortably past CAT link-up */

static int  s_dial_push_left;
static bool s_cat_was_ready;
static bool s_sim_was_on;

static void arm_dial_push(const char *why)
{
    s_dial_push_left = DIAL_PUSH_TRIES;
    ESP_LOGI(TAG, "dial: will push the stored WSPR dial to the radio (%s)", why);
}

/* ONE format string for the header AND every row.
 *
 * These used to be two independent strings - a hand-spaced header and a
 * printf format - and they drifted: PWR's data ended at column 43 where its
 * header started, and KM/BRG were off by one and two. Nothing catches that
 * except looking at the screen, which is how the operator found it.
 *
 * Every field is passed as a STRING, including the numeric ones, so the header
 * can be produced by the same specifiers. Numbers are right-aligned and their
 * headers with them, which is what a numeric column wants.
 *
 * Monospaced by construction (qmx_mono_25) - column arithmetic in characters
 * only means anything in a fixed-advance font. */
/* ---- THE COLUMN BUDGET, because it is exactly full ---------------------
 *
 * qmx_mono_25's advance is 15 px (240 sixteenths - see CELL_W in
 * qmx_term_view.c), and the pane is RIGHT_W = 944 px, so there are exactly
 * 62 characters. Every column below is its own true maximum and the gaps are
 * a single space, which is what "squeeze them together but keep a proper gap"
 * has to mean when the row is already at the edge:
 *
 *   UTC 5 (HH:MM)   CALL 10   GRID 4   COUNTRY 11   SNR 3   DRF 3
 *   HZ 6 (1416.3)   PWR 3     KM 5 (18897)          BRG 3
 *   = 53 + 9 single spaces = 62. Full. Nothing more fits.
 *
 * Consequences worth knowing before editing this:
 *  - GRID is 4 because WSPR_SPOT_GRID_MAX is 5. A 6-char grid cannot arrive.
 *  - KM is 5 because the antipode is ~20000 km.
 *  - DRIFT is headed DRF: the word is 5 characters and the data is 3, and
 *    since ONE format string serves header and rows the column would have to
 *    be 5 to hold the title. Abbreviating the title is cheaper than two wasted
 *    columns on every row.
 *  - CALL gets 10 - the struct's whole capacity - because a truncated
 *    CALLSIGN is a wrong identity, which this project does not print. COUNTRY
 *    is allowed to fall back instead of truncating; see country_field().
 *  - The UTC column is blank on all but the first row of a cycle. That still
 *    costs 6 characters, and it is worth it: it replaced a standalone
 *    timestamp line AND a blank line per group, so a 3-spot cycle went from
 *    5 lines to 3. */
/* ⭐ THE CALL COLUMN PAYS FOR THE WIDER LEFT PANEL, AND SEVEN IS WHAT FITS.
 *
 * qmx_mono_25 advances exactly 15.0 px per character, so the pane holds
 * RIGHT_W / 15 = 59 characters; the row was 62 and CALL gives up the three.
 * (I first took them from COUNTRY instead, on the grounds that CALL has no
 * graceful fallback. The operator asked twice for CALL, so CALL it is - and
 * MEASURING the font rather than estimating it is what made 7 possible where
 * a guess had said 6.)
 *
 * ⚠ Seven is not arbitrary and it is not free. Every callsign in a live 25-
 * station sample from this bench is six characters or fewer, so the table
 * aligns in practice - but a COMPOUND call (BH4RRG/QRP is ten) still prints in
 * full and pushes that row's later columns right. printf does not truncate,
 * and it must not: a clipped callsign is a different station.
 *
 * ⛔ THE WIDTHS ARE DEFINED ONCE AND THE FORMAT IS BUILT FROM THEM. Every
 * column width used to appear twice - in ROW_FMT and again wherever the field
 * was prepared - and that drift has already caused two bugs in one evening
 * (COUNTRY_W left at 11 when the format went to 7, then the reverse). A
 * stringified constant cannot disagree with itself. */
#define W_UTC   5
#define W_CALL  7
#define W_GRID  4
#define W_CTY  11
#define W_SNR   3
#define W_DRF   3
#define W_TONE  6
#define W_PWR   3
#define W_KM    5
#define W_BRG   3

#define STRINGIFY2(x) #x
#define STRINGIFY(x)  STRINGIFY2(x)

#define ROW_FMT "%-" STRINGIFY(W_UTC)  "s %-" STRINGIFY(W_CALL) "s %-"                      STRINGIFY(W_GRID) "s %-" STRINGIFY(W_CTY)  "s %"                       STRINGIFY(W_SNR)  "s %"  STRINGIFY(W_DRF)  "s %"                       STRINGIFY(W_TONE) "s %"  STRINGIFY(W_PWR)  "s %"                       STRINGIFY(W_KM)   "s %"  STRINGIFY(W_BRG)  "s"

/* Spelled out if it fits, else the DXCC alpha-3. NEVER truncated: "United
 * Stat" is not a country and a clipped name reads as a bug, while USA is
 * simply the shorter true answer. The full name comes from the callsign via
 * dxcc_lookup(), the same source the web panel uses, so the two screens
 * cannot disagree. */
#define COUNTRY_W W_CTY   /* one number, see the widths above */
static const char *country_field(const wspr_spot_t *sp)
{
    const char *full = dxcc_lookup(sp->call);
    if (full && full[0] && strlen(full) <= COUNTRY_W) return full;
    return sp->cty[0] ? sp->cty : "--";
}

static void fmt_row(char *out, size_t n, const wspr_spot_t *sp, const char *utc)
{
    char snr[16], drift[16], hz[16], pwr[16], km[20], brg[16];

    /* An unmeasured value prints as a dash, never as a number. WSPR_SNR_UNKNOWN
     * and WSPR_DRIFT_UNKNOWN exist precisely so this cannot quietly become a
     * fabricated measurement - the same rule that deleted the ADIF "599". */
    if (sp->snr_db == WSPR_SNR_UNKNOWN) snprintf(snr, sizeof(snr), "--");
    else snprintf(snr, sizeof(snr), "%+d", sp->snr_db);

    if (sp->drift_hz == WSPR_DRIFT_UNKNOWN) snprintf(drift, sizeof(drift), "--");
    else snprintf(drift, sizeof(drift), "%+d", sp->drift_hz);

    snprintf(hz,  sizeof(hz),  "%.1f", (double)sp->freq_hz);
    snprintf(pwr, sizeof(pwr), "%d", (int)sp->power_dbm);

    if (sp->km < 0) snprintf(km, sizeof(km), "--");
    else snprintf(km, sizeof(km), "%d", (int)sp->km);

    if (sp->bearing_deg < 0) snprintf(brg, sizeof(brg), "--");
    else snprintf(brg, sizeof(brg), "%d", (int)sp->bearing_deg);

    snprintf(out, n, ROW_FMT, utc, sp->call, sp->grid,
             country_field(sp), snr, drift, hz, pwr, km, brg);
}

static void fmt_header(char *out, size_t n)
{
    /* CENTRED over each column. printf has no centring conversion, so each
     * heading is padded into a buffer of exactly its column width first - and
     * because it then arrives at ROW_FMT already the right length, the format's
     * own left/right alignment cannot move it again.
     *
     * "TONE" rather than "HZ": every column here is a number in some unit, so
     * "HZ" named the unit while the others name the quantity. What the column
     * holds is the station's audio tone within the 200 Hz window. */
    char h[10][16];
    const char *raw[10] = { "UTC", "CALL", "GRID", "COUNTRY", "SNR",
                            "DRF", "TONE", "PWR", "KM", "BRG" };
    const int   w[10]   = { W_UTC, W_CALL, W_GRID, W_CTY, W_SNR,
                            W_DRF, W_TONE, W_PWR, W_KM, W_BRG };
    for (int i = 0; i < 10; i++) {
        const int len  = (int)strlen(raw[i]);
        const int pad  = w[i] > len ? w[i] - len : 0;
        const int left = pad / 2;
        int k = 0;
        for (int j = 0; j < left && k < (int)sizeof(h[i]) - 1; j++) h[i][k++] = ' ';
        for (int j = 0; raw[i][j] && k < (int)sizeof(h[i]) - 1; j++) h[i][k++] = raw[i][j];
        for (int j = 0; j < pad - left && k < (int)sizeof(h[i]) - 1; j++) h[i][k++] = ' ';
        h[i][k] = '\0';
    }
    snprintf(out, n, ROW_FMT, h[0], h[1], h[2], h[3], h[4],
             h[5], h[6], h[7], h[8], h[9]);
}

static void cycle_label(char *out, size_t n, int64_t utc)
{
    time_t t = (time_t)utc;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    /* HH:MM only - 5 characters, which is the column width. The word UTC is
     * in the column HEADER now, so repeating it on every group wasted 4. */
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

void wspr_screen_view_init(lv_obj_t *parent)
{
    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, MID_W, MID_H);
    lv_obj_set_pos(s_container, 0, MID_Y);
    lv_obj_set_style_bg_color(s_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_radius(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    /* A backdrop, not a control - the pointer must not go green over it. */
    lv_obj_add_flag(s_container, UI_FLAG_NOT_HOT);

    /* ---------------- left pane ---------------- */
    s_lbl_title = lv_label_create(s_container);
    lv_label_set_text(s_lbl_title, "MODE: WSPR");
    /* ⛔ THE SIZE IS MEASURED, NOT GUESSED - and the guess was wrong.
     *
     * This started at 28 under a comment asserting that "MODE: WSPR" could not
     * fit at 48 because it is one character longer than the FT8 page's
     * "MODE: FT8". That was an ESTIMATE (~296 px against 288 available) written
     * as if it were a fact, and it cost the page a header two sizes smaller
     * than every other mode's for no established reason. The operator asked why
     * it looked odd next to the other pages, which was the right question.
     *
     * It also compared against the wrong budget. The controls below use a 16 px
     * margin, but a title is not a control and need not share it: the decode
     * list starts at RIGHT_X (LEFT_W + 8), so the header can have the panel's
     * full width and still clear it.
     *
     * So: try the big font, ASK LVGL how wide the text actually is, and step
     * down only if it genuinely does not fit. Same pattern as the bottom bar's
     * version label in ui.c. That way this page matches the others whenever it
     * can, and can never spill into the CALL column when it cannot. */
    /* 48, the same as every other mode page, and it DOES NOT FIT in the panel.
     *
     * Measured at runtime rather than guessed: "MODE: WSPR" renders about
     * 336 px at 48 pt against this panel's 320. "MODE: FT8" is one character
     * shorter, ~302 px, which is exactly why the other pages fit and this one
     * cannot. Two alternatives were built and rejected by the operator - a
     * smaller font, and splitting "MODE:" off at 20 so only the name was large.
     * His call, made with the constraint in front of him: consistency with the
     * other pages matters more than the overlap, "if it then lap over the wf
     * window then so be it".
     *
     * So it is foregrounded deliberately. The waterfall canvas is created after
     * this label and LVGL draws siblings in creation order, so without this the
     * title would be drawn UNDER the waterfall and simply disappear - the
     * opposite of what was asked for. The overlap is ~24 px into the top-left
     * corner of the waterfall, which carries the 1350 Hz edge of the window. */
    lv_label_set_text(s_lbl_title, "MODE: WSPR");
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_pos(s_lbl_title, 16, 4);
    lv_obj_move_foreground(s_lbl_title);

    /* The dial, boxed like the FT8 page's preset. Read-only for now: the
     * standard-dial picker is the next piece (see docs/wspr-ui-design.md - a
     * free-entry keypad is deliberately NOT wanted, because every band has one
     * canonical WSPR frequency and anything else is simply not in the
     * sub-band). */
    /* ⛔ NO WRAPPER BOX. The dropdown used to be centred inside an lv_obj of the
     * SAME colour that also had its own 1 px border and its own default
     * padding - so the control rendered as a rounded box inset inside a second
     * rounded box, with the chevron floating in the gap between them. That is
     * the "strange" band button: two frames where the design has one, and an
     * inner control narrower than everything below it.
     *
     * A dropdown is already a styleable box. Styling it directly gives one
     * frame, full panel width, and the same left edge as the cycle bar and the
     * TX buttons underneath. */
    s_dd_dial = lv_dropdown_create(s_container);
    s_navail = wspr_bands_available(s_avail, (int)sizeof(s_avail));
    rebuild_dial_options();
    lv_obj_set_size(s_dd_dial, LEFT_W - 32, 56);
    lv_obj_set_pos(s_dd_dial, 16, 70);
    lv_obj_set_style_radius(s_dd_dial, 8, 0);
    lv_obj_set_style_border_width(s_dd_dial, 1, 0);
    /* 28, not 24 and certainly not 20. Read on the actual screen at arm's
     * length this was the smallest thing in the column and the one carrying
     * the frequency. The panel is 372 px wide now, so "20 m  14.095600" at
     * 28 pt is ~230 px inside a 340 px control - it fits with the chevron. */
    lv_obj_set_style_text_font(s_dd_dial, &lv_font_montserrat_28, 0);
    /* Dark like everything else on this page; the stock dropdown is white. */
    lv_obj_set_style_bg_color(s_dd_dial, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_text_color(s_dd_dial, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_dd_dial, lv_color_hex(UI_COLOR_BORDER), 0);
    {
        lv_obj_t *list = lv_dropdown_get_list(s_dd_dial);
        if (list) {
            lv_obj_set_style_bg_color(list, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
            lv_obj_set_style_text_color(list, lv_color_hex(UI_COLOR_TEXT), 0);
            lv_obj_set_style_text_font(list, &lv_font_montserrat_24, 0);
            /* SHOW EVERY BAND WITHOUT SCROLLING. The stock list caps itself
             * well below what this panel has room for, so a six-entry picker
             * arrived scrollable for no reason - and a scrollbar on a list that
             * would fit is a control asking to be fumbled on a touch screen.
             * Bounded by the pane rather than unbounded, because a QMX+ offers
             * eleven bands and the list must not run off the bottom. */
            lv_obj_set_style_max_height(list, MID_H - 90, 0);
        }
    }
    /* Start on the STORED dial, not on entry 0.
     *
     * A screenshot caught this: with the radio wedged cat_get_frequency()
     * returns 0, the tick's sync never runs, and the picker sat on "160 m"
     * while the stored dial was 20 m. A control that displays a band it is not
     * set to is worse than one that displays nothing. */
    {
        qmx_settings_t ds;
        settings_load_all(&ds);
        for (int k = 0; k < s_navail; k++) {
            if (kBands[s_avail[k]].dial_hz == ds.wspr_dial_hz) {
                lv_dropdown_set_selected(s_dd_dial, (uint16_t)k);
                break;
            }
        }
    }
    lv_obj_add_event_cb(s_dd_dial, dial_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_lbl_dial = NULL;   /* the dropdown IS the dial readout now */

    /* The cycle: plain language above, one 120 s bar below. It orients - "am I
     * receiving, how long left" - rather than urging, because nothing in WSPR
     * needs a decision inside the cycle. */
    s_lbl_cycle = lv_label_create(s_container);
    lv_label_set_text(s_lbl_cycle, "starting...");
    lv_obj_set_style_text_font(s_lbl_cycle, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_cycle, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
    lv_obj_set_pos(s_lbl_cycle, 16, 136);

    s_bar_cycle = lv_bar_create(s_container);
    lv_obj_set_size(s_bar_cycle, LEFT_W - 32, 10);
    lv_obj_set_pos(s_bar_cycle, 16, 170);
    lv_bar_set_range(s_bar_cycle, 0, 120);
    lv_bar_set_value(s_bar_cycle, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_cycle, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_bg_color(s_bar_cycle, lv_color_hex(UI_COLOR_PRIMARY), LV_PART_INDICATOR);

    s_lbl_status = lv_label_create(s_container);
    lv_label_set_text(s_lbl_status, "");
    /* 22, not 18 - this line carries the capture and decode progress and was
     * the smallest text on the page. */
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_pos(s_lbl_status, 16, 192);

    s_lbl_heard = lv_label_create(s_container);
    lv_label_set_text(s_lbl_heard, "Heard nothing yet");
    lv_obj_set_style_text_font(s_lbl_heard, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_lbl_heard, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_lbl_heard, 16, 224);

    /* TX and Duty, side by side.
     *
     * A WSPR transmission keys the radio for 110 SECONDS - eight times an FT8
     * burst. This project's rule for controls that key the radio (written for
     * SWR Tune) is that they must be impossible to trigger by accident and
     * visibly ACTIVE while engaged, which is why TX is a labelled toggle
     * reading OFF/ON rather than a one-tap "transmit". */
    /* FULL WIDTH: the Duty button used to share this row, and moved to the
     * settings drawer (operator, 2026-08-28). Duty is a policy chosen once for
     * a session - "how much of the time may this thing transmit" - while TX
     * ON/OFF is the control reached during one. Splitting them puts the
     * decision where it belongs and gives the one live control the whole row. */
    s_btn_tx = lv_btn_create(s_container);
    lv_obj_set_size(s_btn_tx, LEFT_W - 32, 56);
    lv_obj_set_pos(s_btn_tx, 16, 258);
    lv_obj_set_style_radius(s_btn_tx, 8, 0);
    lv_obj_add_event_cb(s_btn_tx, tx_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_tx = lv_label_create(s_btn_tx);
    lv_label_set_text(s_lbl_tx, "TX  OFF");
    /* 28, not 20: these are 56 px buttons and the label was sitting in the
     * middle of one looking like a caption. The panel is 372 px wide now, so
     * each half is ~166 px - "TX  OFF" at 28 pt is ~110 px and still fits. */
    lv_obj_set_style_text_font(s_lbl_tx, &lv_font_montserrat_28, 0);
    lv_obj_center(s_lbl_tx);

    build_left_extras();

    /* ---------------- right pane, upper: the captured window ---------------- */
    /* RGB565 at display resolution rather than a 205x176 image scaled up:
     * lv_canvas has no scaling, and drawing straight into display pixels keeps
     * the frequency axis below it exactly aligned with the columns. */
    s_wf_buf = heap_caps_malloc(RIGHT_W * WF_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_wf_buf) {
        s_wf_canvas = lv_canvas_create(s_container);
        lv_canvas_set_buffer(s_wf_canvas, s_wf_buf, RIGHT_W, WF_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_wf_canvas, RIGHT_X, WF_Y);
        lv_canvas_fill_bg(s_wf_canvas, lv_color_hex(0x000000), LV_OPA_COVER);
        lv_obj_add_flag(s_wf_canvas, UI_FLAG_NOT_HOT);
    }

    /* The frequency scale. Evenly spaced ticks with numbers, because a
     * waterfall without them cannot answer "where is that signal?" - which is
     * the only question it is there to answer. */
    /* ONE LABEL PER TICK, positioned absolutely.
     *
     * The first version was a single space-padded string, which needs the
     * font's space width to be known - I assumed ~10 px for montserrat_18 and
     * it is about half that, so the scale ended at x~730 of a 944 px waterfall
     * and every label pointed at the wrong column. Absolute positions cannot be
     * wrong: each label is placed by the SAME arithmetic that maps a frequency
     * to a waterfall column, then centred on it. */
    for (int i = 0; i < AXIS_TICKS; i++) {
        int hz = 1350 + i * 50;
        int x  = (hz - (int)WSPR_WF_LO_HZ) * RIGHT_W /
                 (int)(WSPR_WF_HI_HZ - WSPR_WF_LO_HZ);
        lv_obj_t *t = lv_label_create(s_container);
        lv_label_set_text_fmt(t, "%d", hz);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_update_layout(t);
        int w = lv_obj_get_width(t);
        int px = RIGHT_X + x - w / 2;                /* centre on its column */
        if (px < RIGHT_X) px = RIGHT_X;              /* keep the ends on-screen */
        if (px + w > RIGHT_X + RIGHT_W) px = RIGHT_X + RIGHT_W - w;
        lv_obj_set_pos(t, px, AXIS_Y);
        /* A 1 px tick above the number, so the eye can follow it into the
         * waterfall rather than estimating. */
        lv_obj_t *tick = lv_obj_create(s_container);
        lv_obj_remove_style_all(tick);
        lv_obj_set_size(tick, 1, 4);
        lv_obj_set_pos(tick, RIGHT_X + x, AXIS_Y - 4);
        lv_obj_set_style_bg_color(tick, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
        lv_obj_add_flag(tick, UI_FLAG_NOT_HOT);
    }

    /* ---------------- right pane, lower: the log ---------------- */
    lv_obj_t *hdr = lv_label_create(s_container);
    { char h[160]; fmt_header(h, sizeof(h)); lv_label_set_text(hdr, h); }
    lv_obj_set_style_text_font(hdr, &qmx_mono_25, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(hdr, RIGHT_X, LIST_Y);

    s_list = lv_obj_create(s_container);
    lv_obj_set_size(s_list, RIGHT_W, MID_H - LIST_Y - 34);
    lv_obj_set_pos(s_list, RIGHT_X, LIST_Y + 30);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    /* SCROLLABLE, vertically only. The ring holds far more than a screenful
     * and the operator wants to reach all of it, the way the FT8 list works.
     * Horizontal scrolling is off: the table is sized to the pane, so sideways
     * travel would only ever be a way to lose the columns off the edge.
     * NOT_HOT because this is a surface you drag, not a control you press -
     * nothing here is tappable (a WSPR spot is a measurement, not a station to
     * work). */
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_list, UI_FLAG_NOT_HOT);

    /* ONE label holding every line, not one object per row.
     *
     * The FT8 list needs per-row objects because rows are touch targets - you
     * tap a station to work it. Nothing here is tappable: a WSPR spot is a
     * measurement, there is nobody to reply to. So a single multi-line label is
     * both simpler and much cheaper on an LVGL object budget this board has
     * repeatedly run into. */
    s_lbl_rows = lv_label_create(s_list);
    lv_label_set_text(s_lbl_rows, "Listening...");
    lv_obj_set_style_text_font(s_lbl_rows, &qmx_mono_25, 0);
    lv_obj_set_style_text_color(s_lbl_rows, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_line_space(s_lbl_rows, 2, 0);
    lv_obj_set_pos(s_lbl_rows, 0, 0);

    /* ⛔ RE-FOREGROUNDED HERE, AT THE END, AND THAT IS THE WHOLE POINT.
     * lv_obj_move_foreground() only lifts a child above the siblings that
     * exist WHEN IT RUNS - and the title is created near the top of this
     * function while the waterfall canvas is created near the bottom. So the
     * call beside the title was undone by every object built after it, and the
     * operator saw the "R" of WSPR disappear behind the waterfall. Raising it
     * once more, after the last sibling exists, is what actually puts it in
     * front. */
    lv_obj_move_foreground(s_lbl_title);
}

void wspr_screen_view_show(void)
{
    if (!s_container) return;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_container);
    /* ⛔ AND THEN PUT THE EDGE STRIPS BACK ON TOP. This container is a
     * near-full-screen opaque pane, so foregrounding it buries them - which is
     * exactly why the swipe out of WSPR did nothing. CLAUDE.md already carried
     * this warning for the FT8 view. */
    ui_raise_edge_strips();
    s_last_spot_count = -1;      /* force a repaint on entry */
    s_last_status[0]  = '\0';
    /* Entering the page is the operator saying "receive WSPR", and that is
     * only true if the radio is actually on a WSPR dial. */
    arm_dial_push("page entry");
    s_cat_was_ready = cat_is_ready();
    s_sim_was_on    = wspr_sim_enabled();
}

void wspr_screen_view_hide(void)
{
    if (!s_container) return;
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *wspr_screen_view_get_container(void) { return s_container; }

/* SDR-ish ramp: black -> blue -> cyan -> yellow -> red, same family the
 * panadapter's waterfall uses so the two pages read alike. Returns RGB565
 * directly - see repaint_waterfall() for why this does not go through
 * lv_color_t. */
static inline uint16_t wf_rgb565(uint8_t v)
{
    uint8_t r, g, b;
    /* Cycle-boundary marker: a light green (144,238,144) the signal ramp cannot
     * produce, because wspr_rx.c clamps real intensities to 254. Deliberately
     * NOT a value picked out of the ramp - a strong signal passes through green
     * on its way to red, so a palette green would still be ambiguous. */
    if (v == WSPR_WF_MARK) return (uint16_t)(((144 >> 3) << 11) | ((238 >> 2) << 5) | (144 >> 3));
    if (v < 64)        { r = 0; g = 0;                      b = (uint8_t)(v * 3); }
    else if (v < 128)  { r = 0; g = (uint8_t)((v - 64) * 4); b = 255; }
    else if (v < 192)  { r = (uint8_t)((v - 128) * 4); g = 255; b = (uint8_t)(255 - (v - 128) * 4); }
    else               { r = 255; g = (uint8_t)(255 - (v - 192) * 4); b = 0; }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Repaint the captured window.
 *
 * ⛔ Writes STRAIGHT INTO THE RGB565 BUFFER, not via lv_canvas_set_px().
 * This is 944 x 200 = 188,800 pixels, and set_px() goes through LVGL's draw
 * layer for every one of them - enough to block taskLVGL long enough to starve
 * the HTTP server. The symptom was the operator's browser disconnecting every
 * time a cycle finished, and /ss.bmp truncating at 135 KB of 1.84 MB. A direct
 * buffer fill plus one invalidate does the same job without holding the task.
 *
 * Called whenever the sequence number moves - which since the carpet became
 * row-by-row is roughly once per WSPR symbol (~1.5 Hz) while a capture is
 * filling, NOT once per cycle as this comment used to claim. That is the
 * reason the direct-buffer rule above is load-bearing rather than a nicety. */
static void repaint_waterfall(void)
{
    if (!s_wf_canvas || !s_wf_data || !s_wf_buf) return;
    uint16_t *px = (uint16_t *)s_wf_buf;

    /* Row and column maps precomputed once instead of a divide per pixel. */
    static uint16_t colmap[RIGHT_W];
    static uint16_t rowmap[WF_H];
    for (int x = 0; x < RIGHT_W; x++) colmap[x] = (uint16_t)(x * WSPR_WF_COLS / RIGHT_W);
    /* out row 0 is the NEWEST row, so display y maps straight through and the
     * newest data lands at the top - the panadapter's convention. */
    for (int y = 0; y < WF_H;    y++) rowmap[y] = (uint16_t)(y * WSPR_WF_HIST_ROWS / WF_H);

    for (int y = 0; y < WF_H; y++) {
        const uint8_t *src = &s_wf_data[rowmap[y] * WSPR_WF_COLS];
        uint16_t *dst = &px[y * RIGHT_W];
        for (int x = 0; x < RIGHT_W; x++) dst[x] = wf_rgb565(src[colmap[x]]);
    }
    lv_obj_invalidate(s_wf_canvas);
}

void wspr_screen_view_tick(void)
{
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;

    /* After the visibility guard: these read the spot store under its mutex and
     * walk a 256-entry ring, which is pure cost on a page nobody is looking at. */
    refresh_left_extras();

    /* ---- re-arm triggers, then the pending push ---------------------- */
    {
        const bool cat_now = cat_is_ready();
        const bool sim_now = wspr_sim_enabled();
        /* CAT coming back is what a QMX power cycle looks like from here, and a
         * power cycle reloads the radio's own band config - measured, it came
         * back on 30 m while 20 m was stored. */
        if (cat_now && !s_cat_was_ready) arm_dial_push("CAT came back");
        /* Simulation never touches the radio, so switching it OFF is the first
         * moment the radio's actual frequency starts to matter again. */
        if (!sim_now && s_sim_was_on)    arm_dial_push("simulation off");
        s_cat_was_ready = cat_now;
        s_sim_was_on    = sim_now;

        if (s_dial_push_left > 0) {
            if (!cat_now) {
                s_dial_push_left--;      /* wait for the link, do not give up yet */
                if (s_dial_push_left == 0)
                    ESP_LOGW(TAG, "dial: CAT never became ready - the radio keeps "
                                  "whatever frequency it is on");
            } else {
                qmx_settings_t ds;
                settings_load_all(&ds);
                const uint32_t want = ds.wspr_dial_hz;
                const uint32_t have = cat_get_frequency();
                if (want && have != want) {
                    /* Forced: the ordinary setter shares a 200 ms rate limit
                     * with the CAT poll, and the one write that decides whether
                     * this page hears anything at all must not be the one that
                     * gets dropped. */
                    cat_set_frequency_forced(want);
                    ESP_LOGW(TAG, "dial: pushed %lu Hz to the radio (was %lu)",
                             (unsigned long)want, (unsigned long)have);
                } else if (want) {
                    ESP_LOGI(TAG, "dial: radio already on %lu Hz", (unsigned long)want);
                }
                s_dial_push_left = 0;    /* one shot - never fight manual tuning */
            }
        }
    }

    /* Dial: select the standard entry matching the radio, so the picker shows
     * where we actually are rather than what was last tapped. A dial that is
     * not a standard WSPR frequency leaves the selection alone - the operator
     * has tuned off the sub-band and the picker should not pretend otherwise. */
    uint32_t f = cat_get_frequency();
    if (f && s_dd_dial) {
        /* The radio may only have answered its band list AFTER the page was
         * built, so re-take it here; the picker is otherwise stuck with
         * whatever was known at construction (every band, if CAT was down). */
        int n = wspr_bands_available(s_avail, (int)sizeof(s_avail));
        if (n != s_navail) { s_navail = n; rebuild_dial_options(); }
        for (int k = 0; k < s_navail; k++) {
            if (kBands[s_avail[k]].dial_hz == f) {
                if (lv_dropdown_get_selected(s_dd_dial) != (uint16_t)k)
                    lv_dropdown_set_selected(s_dd_dial, (uint16_t)k);
                break;
            }
        }
    }

    /* TX and Duty, from settings so the web UI and the buttons cannot drift.
     *
     * While a burst is running the TX block goes UI_COLOR_TX_ACTIVE orange and
     * counts down: this project's rule for anything that keys the radio is
     * that the operator should never have to wonder whether it is
     * transmitting. 110 s is a long time to be unsure. */
    {
        qmx_settings_t st;
        settings_load_all(&st);

        char txt[48];
        int secs = 0;
        wspr_tx_state_t tst = wspr_tx_get_status(NULL, 0, &secs);

        /* ⚠ UNPROTECTED is said ON THIS PAGE, not only in the drawer.
         *
         * WSPR keys the PA for ~110 s out of every 120 and the finals overheat
         * at full power on that cycle. The guard defaults ON and switching it
         * off is deliberate and toasted - but someone who switched it off and
         * walked away had nothing in front of them saying so, and this is the
         * screen they are actually looking at. A protection whose absence is
         * invisible is a protection you cannot trust.
         *
         * Deliberately NOT a block. It is the operator's radio, and there are
         * legitimate reasons (a low supply, a dummy load, a QMX already turned
         * down). It just may not be silent. */
        bool unprotected = st.wspr_tx_en && !st.wspr_pa_reduce;

        if (tst == WSPR_TX_ACTIVE) {
            snprintf(txt, sizeof(txt), unprotected ? "TX  ON AIR  FULL PWR" : "TX  ON AIR");
            lv_obj_set_style_bg_color(s_btn_tx, lv_color_hex(UI_COLOR_TX_ACTIVE), 0);
        } else if (tst == WSPR_TX_ARMED) {
            snprintf(txt, sizeof(txt), "TX  in %d:%02d%s", secs / 60, secs % 60,
                     unprotected ? "  FULL PWR" : "");
            lv_obj_set_style_bg_color(s_btn_tx, lv_color_hex(UI_COLOR_PRIMARY), 0);
        } else {
            snprintf(txt, sizeof(txt), "TX  %s%s", st.wspr_tx_en ? "ON" : "OFF",
                     unprotected ? "  FULL PWR" : "");
            lv_obj_set_style_bg_color(s_btn_tx,
                lv_color_hex(st.wspr_tx_en ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE_RAISED), 0);
        }
        /* Red text on the TX block whenever the finals are unprotected, in every
         * one of the three states above - the risk does not pause between
         * bursts, because the next one is coming in under two minutes. */
        lv_obj_set_style_text_color(s_lbl_tx,
            lv_color_hex(unprotected ? 0xFF4010 : 0xFFFFFF), 0);
        if (strcmp(lv_label_get_text(s_lbl_tx), txt) != 0)
            lv_label_set_text(s_lbl_tx, txt);

        /* The Duty readout that used to live here went to the drawer with its
         * button (2026-08-28). Nothing is left to update: TX above still shows
         * whether transmitting is armed at all, which is the part that changes
         * during a session. */
    }

    /* cycle position: the bar is the 120 s window, so it is a real clock
     * position rather than a progress guess. */
    time_t now = time(NULL);
    int into = (int)(now % 120);
    lv_bar_set_value(s_bar_cycle, into, LV_ANIM_OFF);

    char c[48];
    snprintf(c, sizeof(c), "cycle  %d:%02d / 2:00", into / 60, into % 60);
    lv_label_set_text(s_lbl_cycle, c);

    /* status straight from the slot loop - change-detected, because writing an
     * identical string still costs LVGL an invalidate. */
    const char *st = wspr_rx_running() ? wspr_rx_status() : "receiver stopped";
    if (strncmp(st, s_last_status, sizeof(s_last_status)) != 0) {
        snprintf(s_last_status, sizeof(s_last_status), "%s", st);
        lv_label_set_text(s_lbl_status, s_last_status);
    }

    /* the captured window, repainted only when a new one has landed */
    uint32_t seq = wspr_rx_waterfall_seq();
    if (seq != s_wf_seen && s_wf_canvas) {
        if (!s_wf_data)
            s_wf_data = heap_caps_malloc(WSPR_WF_HIST_ROWS * WSPR_WF_COLS,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_wf_data && wspr_rx_get_waterfall(s_wf_data)) {
            s_wf_seen = seq;
            repaint_waterfall();
        }
    }

    /* Repainted when a spot was ADDED - not when the COUNT changed. The count
     * saturates at the ring size and then never moves again, which froze this
     * list and the header below for five hours of the 2026-08-24 overnight run
     * while the receiver decoded normally throughout. */
    int n = (int)wspr_spots_seq();
    if (n == s_last_spot_count) return;
    s_last_spot_count = n;

    /* BOTH numbers, because one of them alone is misread. "Heard 12 stations"
     * over a list showing 18 rows reads as a bug - the operator asked whether
     * the header was wrong within minutes of the list first working. It was
     * not: the header counts DISTINCT CALLSIGNS and the list has one row per
     * DECODE, so a station heard in four cycles is four rows and one station.
     *
     * "spots" is the WSPR word for a decode, so this is also the vocabulary
     * every other WSPR tool and wsprnet itself uses - saying both makes the
     * relationship obvious instead of leaving it to be worked out.
     *
     * ⚠ Neither figure is a session total. The ring holds WSPR_SPOT_RING (256)
     * entries, roughly eight cycles of a busy band, and older spots fall off
     * the end - so this is a rolling window, hours on a quiet band and about a
     * quarter of an hour on a crowded one. */
    int uniq = wspr_spots_unique_calls();
    int held = wspr_spots_count();
    char h[64];
    if (uniq == 0) snprintf(h, sizeof(h), "Heard nothing yet");
    else snprintf(h, sizeof(h), "%d station%s / %d spot%s",
                  uniq, uniq == 1 ? "" : "s", held, held == 1 ? "" : "s");
    lv_label_set_text(s_lbl_heard, h);

    if (n == 0) {
        lv_label_set_text(s_lbl_rows, "Listening...");
        return;
    }

    /* Static, never the stack - and in PSRAM, because it is read once per
     * second by a list rebuild and internal RAM is what the OTA verify runs
     * out of. colmap/rowmap above stay internal deliberately: colmap is read
     * once per PIXEL of a repaint. */
    EXT_RAM_BSS_ATTR static wspr_spot_t snap[VIEW_ROWS];
    int got = wspr_spots_get(snap, VIEW_ROWS);

    /* Grouped under the cycle each burst was heard in - the whole reason this
     * is a log and not a live list. */
    /* ⛔ STATIC AND IN PSRAM, NOT A STACK LOCAL. At 12 rows this was 1.5 KB on
     * the stack and got away with it; at 64 it is ~7.3 KB on taskLVGL, whose
     * stack is about 8 KB - and CLAUDE.md carries a list of crashes from
     * exactly this (the v0.20.1 pounce crash was an 11 KB array on this very
     * task, and the compiler reserves the frame at the prologue whether the
     * code path is taken or not). Safe as a static because this runs only on
     * taskLVGL, the same reasoning snap[] above uses. */
    EXT_RAM_BSS_ATTR static char buf[VIEW_ROWS * 110 + 256];
    size_t off = 0;
    int64_t last_cycle = 0;
    for (int i = 0; i < got && off < sizeof(buf) - 96; i++) {
        /* The cycle time is the row FIRST COLUMN, printed once per cycle and
         * blank for the rest. It used to be a line of its own preceded by a
         * blank line - two lines per cycle to carry five characters, which on
         * a pane this size was most of the log. A 3-spot cycle went 5 -> 3. */
        char utc[8] = "";
        if (snap[i].cycle_utc != last_cycle) {
            last_cycle = snap[i].cycle_utc;
            cycle_label(utc, sizeof(utc), last_cycle);
        }
        char row[160];
        fmt_row(row, sizeof(row), &snap[i], utc);
        off += snprintf(buf + off, sizeof(buf) - off, "%s\n", row);
    }
    lv_label_set_text(s_lbl_rows, buf);
}
