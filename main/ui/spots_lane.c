// Spots lane rendering. Contract and layout rationale in spots_lane.h.

#include "spots_lane.h"
#include "ui.h"
#include "ui_theme.h"
#include "net/spots.h"
#include "adif/adif_log.h"
#include "storage/settings.h"
#include "cat.h"
#include "display/display.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "spots_ui";

// Object pool sizes. Lines are cheap (a 2 px rect), labels are not - each is an
// LVGL label with its own text buffer - so there are deliberately more lines
// than labels: at high density this degrades to "a line for every spot, names on
// the ones worth naming" rather than dropping spots entirely.
#define MAX_TICKS   40
#define MAX_LABELS  16

// Vertical layout (operator's, 2026-08-05): the block of callsigns is CENTRED on
// the middle of the spectrum, and each line drops from its own label DOWNWARDS to
// the frequency axis at the bottom - so the line points at the frequency it
// marks. An earlier version ran the line upwards from the label to the top of the
// spectrum, which pointed at nothing.
//
// The block is centred rather than started at the middle so the lowest row still
// has room for a usable line: with three rows beginning at mid, row 2 had only
// 25 px left to reach the axis.
//
// The label pass therefore has to run BEFORE the line pass, since where the line
// starts depends on which row the name landed in.
#define SPOT_FONT   (&lv_font_montserrat_22)
#define ROW_H       25    // montserrat_22 plus a little breathing room
#define LABEL_ROWS   3    // see pick_row()
#define LABEL_PAD    5    // horizontal clearance between two labels in a row
#define LABEL_EXT_CLICK 10  // fingertip margin around a label's hit area

// Off-screen counters. Inset from the screen border because a target flush
// against the edge is awkward to hit - your fingertip runs out of glass - and
// backed by a dedicated hit rectangle roughly a fingertip and a half in size.
// A big ext_click_area on the label could not work: LVGL clips it to the parent,
// and the label already sits at the parent's bottom edge, so it could only grow
// upwards. That is why the first attempt still felt unhittable.
#define EDGE_INSET   6
#define EDGE_HIT_W   140
#define EDGE_HIT_H   80

// The line is drawn at the SAME opacity as its callsign (operator's request), so
// the two read as one object and a faint spot is faint in both. See-through comes
// from the line being 2 px wide, not from dimming it. The label's dark backing is
// what keeps bright text legible where it crosses a strong signal.
#define LABEL_BG_OPA  120     // of 255, behind the text only

#define FONT_H        22      // montserrat_22, for the geometry helpers below

// Age fades the spot out: a POTA spot is a claim about *now*, and an hour-old
// one pointing at an empty frequency is worse than no spot at all.
#define AGE_FULL_S   (5  * 60)
#define AGE_HALF_S   (15 * 60)
#define AGE_GONE_S   (30 * 60)

// Colours are semantic, not decorative:
//   amber = POTA activation, green = RBN skimmer, grey = already worked on this
//   band. Grey is the important one - it answers "do I need this station?"
//
// Deliberately BRIGHT (operator's request): these sit over a live spectrum with
// a vivid green trace, so a mid-tone would disappear into it. Worked-before is
// the only muted one, and even that is light enough to read - it needs to be
// legible but not to compete for attention.
#define COL_POTA    0xFFC864
#define COL_RBN     0x70FF90
#define COL_WORKED  0xC0C0C0

static lv_obj_t *s_lane;
static lv_obj_t *s_ticks[MAX_TICKS];
static lv_obj_t *s_labels[MAX_LABELS];
static lv_obj_t *s_edge_l, *s_edge_r;
static lv_obj_t *s_edge_l_hit, *s_edge_r_hit;   // generous invisible tap targets

static int       s_lane_h;         // = SPECTRUM_H; the overlay's own height
static int       s_lane_y;         // the overlay's top edge in SCREEN coords
static uint32_t  s_view_lo, s_view_hi;
static uint32_t  s_drawn_lo, s_drawn_hi;
static uint32_t  s_drawn_version = 0xFFFFFFFFu;
static bool      s_visible;

// Repaint scratch, allocated ONCE in PSRAM at build time.
//
// This must not live on the stack or in .bss. repaint() runs from an LVGL timer
// and from a touch handler, i.e. on taskLVGL's ~8 KB stack - the same path that
// took down v0.20.0 when ft8_qso_start() put an 11 KB snapshot array in its
// frame. And .bss would be internal RAM, the scarcest pool on this board (the
// watermark already reaches 0 KB). At SPOTS_MAX=200 this table is ~12 KB, so
// either mistake would be a real one.
typedef struct {
    spot_t   buf[SPOTS_MAX];
    uint16_t idx[SPOTS_MAX];      // index into buf, visible only, sorted by x
    int16_t  x[SPOTS_MAX];        // screen x, parallel to idx
    uint16_t order[SPOTS_MAX];    // index into idx, naming priority order
    uint32_t colour[SPOTS_MAX];   // per-spot, parallel to buf
    uint8_t  worked[SPOTS_MAX];   // per-spot, parallel to buf
    lv_opa_t opa[SPOTS_MAX];      // per-spot, parallel to buf
    int8_t   row[SPOTS_MAX];      // label row per VISIBLE index, -1 = unnamed
} work_t;
static work_t *s_w;

// What each drawn callsign points at, so a tap can act on it. The label carries
// its index here in user_data (stored +1, so 0 means "nothing").
typedef struct { uint32_t freq_hz; uint8_t mode; uint32_t colour; } tap_target_t;
static tap_target_t s_label_target[MAX_LABELS];

// Nearest OFF-screen spot on each side, within the current band. Drives the
// "<3" / "5>" counters, which are tappable: they are the only way to reach a
// spot you cannot see. 0 = nothing that side.
static tap_target_t s_off_l, s_off_r;

// ---- helpers ---------------------------------------------------------------

static int freq_to_x(uint32_t hz)
{
    if (s_view_hi <= s_view_lo) return -1;
    double span = (double)s_view_hi - (double)s_view_lo;
    return (int)(((double)hz - (double)s_view_lo) / span * (double)DISPLAY_H_RES);
}

// Full opacity while fresh, linearly down to half at AGE_HALF_S, hidden past
// AGE_GONE_S. An unparseable timestamp (heard_unix == 0) counts as fresh rather
// than ancient - suppressing a spot we simply could not date would hide real
// activity, which is the worse error.
static lv_opa_t age_opa(int64_t heard_unix, int64_t now)
{
    if (heard_unix <= 0) return LV_OPA_COVER;
    int64_t age = now - heard_unix;
    if (age < 0)          return LV_OPA_COVER;   // clock skew between us and the spotter
    if (age <= AGE_FULL_S) return LV_OPA_COVER;
    if (age >= AGE_GONE_S) return LV_OPA_TRANSP;
    if (age <= AGE_HALF_S) {
        // COVER -> 50% across [AGE_FULL_S, AGE_HALF_S]
        int32_t t = (int32_t)(age - AGE_FULL_S);
        int32_t d = AGE_HALF_S - AGE_FULL_S;
        return (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER / 2) * t / d);
    }
    // 50% -> 15% across [AGE_HALF_S, AGE_GONE_S], so it visibly dies before it
    // vanishes instead of blinking out at full strength.
    int32_t t = (int32_t)(age - AGE_HALF_S);
    int32_t d = AGE_GONE_S - AGE_HALF_S;
    return (lv_opa_t)(LV_OPA_COVER / 2 - (LV_OPA_COVER / 2 - 38) * t / d);
}

// Ranking for which spots get a NAME when there is not room for all of them.
// Lower sorts first. Unworked beats worked (that is the whole point of looking
// at spots), and within a class the fresher one wins.
static int name_priority(int i, int64_t now)
{
    const spot_t *sp = &s_w->buf[i];
    int age_s = (sp->heard_unix > 0 && now > sp->heard_unix) ? (int)(now - sp->heard_unix) : 0;
    return (int)s_w->worked[i] * 100000 + age_s;
}

// Greedy row assignment. Pure, so the self-test can drive it directly: returns
// the row a label of width w starting at x can use, or -1 if every row is taken.
// row_end[] carries the right edge of the last label placed in each row and is
// advanced on success.
//
// THREE rows, raised from two when the font went 14 -> 22: the wider labels
// collided far more often, and a real 4-spot cluster on the 20 m CW POTA
// frequencies was getting only 2 names. The old argument against a third row was
// that it cost waterfall pixels - void now that this is an overlay, with free
// vertical space between the middle of the spectrum and its bottom edge.
static int pick_row(int x, int w, int row_end[LABEL_ROWS])
{
    for (int r = 0; r < LABEL_ROWS; r++) {
        if (x >= row_end[r] + LABEL_PAD) { row_end[r] = x + w; return r; }
    }
    return -1;
}

// ---- vertical geometry (pure, so the self-test can check it) ---------------

// Top of the label block, centred on the middle of the spectrum.
static int label_block_top(int h)
{
    int t = h / 2 - (LABEL_ROWS * ROW_H) / 2;
    return t < 2 ? 2 : t;
}

static int label_row_y(int h, int row) { return label_block_top(h) + row * ROW_H; }

// Where a spot's line starts: just under its own label, running to the axis.
// Only ever called for a NAMED spot - a line with no callsign above it says
// "something is here" without saying what, which just clutters the trace
// (operator's call, 2026-08-05). No label, no line.
static int line_top_y(int h, int row) { return label_row_y(h, row) + ROW_H; }

static void hide_all(void)
{
    for (int i = 0; i < MAX_TICKS; i++)  if (s_ticks[i])  lv_obj_add_flag(s_ticks[i],  LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < MAX_LABELS; i++) if (s_labels[i]) lv_obj_add_flag(s_labels[i], LV_OBJ_FLAG_HIDDEN);
    if (s_edge_l) lv_obj_add_flag(s_edge_l, LV_OBJ_FLAG_HIDDEN);
    if (s_edge_r) lv_obj_add_flag(s_edge_r, LV_OBJ_FLAG_HIDDEN);
    // The tap targets go with them - an invisible live target over empty
    // spectrum would swallow tap-to-tune for nothing.
    if (s_edge_l_hit) lv_obj_add_flag(s_edge_l_hit, LV_OBJ_FLAG_HIDDEN);
    if (s_edge_r_hit) lv_obj_add_flag(s_edge_r_hit, LV_OBJ_FLAG_HIDDEN);
}

// ---- repaint ---------------------------------------------------------------

static void repaint(void)
{
    if (!s_lane || !s_visible || !s_w) return;

    // Opting out simply draws nothing. Now that this is an overlay rather than a
    // strip it costs no layout height either way, so there is nothing to reflow.
    qmx_settings_t st;
    settings_load_all(&st);
    if (!st.spots_en) { hide_all(); return; }

    // Take the whole table so the off-screen counts are meaningful, not just the
    // visible slice.
    int n = spots_get_in_range_wait(s_w->buf, SPOTS_MAX, 0, 0xFFFFFFFFu, 20);
    if (n < 0) return;                 // lock busy: keep the current picture

    hide_all();

    int64_t now = (int64_t)time(NULL);

    // Per-spot properties computed ONCE. adif_log_contains_call_on_band() is a
    // linear scan of the worked-call cache, so calling it from both the colour
    // and the priority helper (as the first cut did) meant hundreds of scans per
    // second on the LVGL thread for nothing.
    for (int i = 0; i < n; i++) {
        s_w->worked[i] = (s_w->buf[i].call[0] &&
                          adif_log_contains_call_on_band(s_w->buf[i].call, s_w->buf[i].freq_hz)) ? 1 : 0;
        s_w->colour[i] = s_w->worked[i] ? COL_WORKED
                       : (s_w->buf[i].source == SPOT_SRC_RBN ? COL_RBN : COL_POTA);
        s_w->opa[i]    = age_opa(s_w->buf[i].heard_unix, now);
    }

    // The off-screen counts are scoped to the BAND we are looking at, not to all
    // of HF. Counting globally (the first cut) reported things like "51 <" while
    // every one of those spots was on 40 m and 80 m - an arrow inviting the
    // operator to tune somewhere that has nothing to do with where they are.
    // "Just outside this window, on this band" is the only reading that earns a
    // direction arrow.
    s_off_l.freq_hz = 0;
    s_off_r.freq_hz = 0;

    uint32_t band_lo = 0, band_hi = 0xFFFFFFFFu;
    {
        uint32_t mid = s_view_lo + (s_view_hi - s_view_lo) / 2;
        uint32_t lo, hi;
        if (ui_validate_band_freq_hz(mid, &lo, &hi)) { band_lo = lo; band_hi = hi; }
    }

    // Split into visible / off-screen-left / off-screen-right.
    int vis_n = 0, off_l = 0, off_r = 0;
    for (int i = 0; i < n; i++) {
        if (s_w->opa[i] == LV_OPA_TRANSP) continue;
        uint32_t f = s_w->buf[i].freq_hz;
        // Off-screen: count it, and remember the NEAREST one so the counter can
        // be tapped to jump there.
        if (f < s_view_lo) {
            if (f >= band_lo) {
                off_l++;
                if (f > s_off_l.freq_hz) { s_off_l.freq_hz = f; s_off_l.mode = (uint8_t)s_w->buf[i].mode; s_off_l.colour = s_w->colour[i]; }
            }
            continue;
        }
        if (f > s_view_hi) {
            if (f <= band_hi) {
                off_r++;
                if (!s_off_r.freq_hz || f < s_off_r.freq_hz) { s_off_r.freq_hz = f; s_off_r.mode = (uint8_t)s_w->buf[i].mode; s_off_r.colour = s_w->colour[i]; }
            }
            continue;
        }
        int x = freq_to_x(s_w->buf[i].freq_hz);
        if (x < 0 || x >= DISPLAY_H_RES) continue;
        s_w->idx[vis_n] = (uint16_t)i;
        s_w->x[vis_n]   = (int16_t)x;
        vis_n++;
    }

    // Sort visible by x (insertion sort - the feed arrives roughly
    // frequency-ordered, so this is near-linear in practice).
    for (int i = 1; i < vis_n; i++) {
        int16_t xi = s_w->x[i]; uint16_t ii = s_w->idx[i];
        int j = i - 1;
        while (j >= 0 && s_w->x[j] > xi) { s_w->x[j+1] = s_w->x[j]; s_w->idx[j+1] = s_w->idx[j]; j--; }
        s_w->x[j+1] = xi; s_w->idx[j+1] = ii;
    }

    // Which visible spots deserve a name: best-priority first, capped by the
    // label pool.
    for (int i = 0; i < vis_n; i++) s_w->row[i] = -1;
    for (int i = 0; i < vis_n; i++) s_w->order[i] = (uint16_t)i;
    for (int i = 1; i < vis_n; i++) {
        uint16_t oi = s_w->order[i];
        int pi = name_priority(s_w->idx[oi], now);
        int j = i - 1;
        while (j >= 0 && name_priority(s_w->idx[s_w->order[j]], now) > pi) {
            s_w->order[j+1] = s_w->order[j]; j--;
        }
        s_w->order[j+1] = oi;
    }

    // Greedy row packing, best-priority first. Past LABEL_ROWS the display would
    // be an unreadable wall of text, so extra spots keep their line and lose only
    // their name.
    int row_end[LABEL_ROWS];
    for (int r = 0; r < LABEL_ROWS; r++) row_end[r] = -10000;
    int used = 0;
    for (int k = 0; k < vis_n && used < MAX_LABELS; k++) {
        int i  = s_w->order[k];
        int si = s_w->idx[i];
        const spot_t *sp = &s_w->buf[si];
        if (!sp->call[0]) continue;

        lv_obj_t *lb = s_labels[used];
        if (!lb) break;
        lv_label_set_text(lb, sp->call);
        lv_obj_update_layout(lb);
        int w = lv_obj_get_width(lb);
        int x = s_w->x[i] - w / 2;                      // centre the name on its tick
        if (x < 0) x = 0;
        if (x + w > DISPLAY_H_RES) x = DISPLAY_H_RES - w;

        int row = pick_row(x, w, row_end);
        if (row < 0) continue;                          // no room: line only

        lv_obj_set_pos(lb, x, label_row_y(s_lane_h, row));
        lv_obj_set_style_text_color(lb, lv_color_hex(s_w->colour[si]), 0);
        lv_obj_set_style_text_opa(lb, s_w->opa[si], 0);
        lv_obj_set_style_bg_opa(lb, (lv_opa_t)((int)LABEL_BG_OPA * s_w->opa[si] / 255), 0);
        // The label is the tap target (the container cannot be, or it would eat
        // every spectrum gesture). It carries an index into s_label_target[]
        // rather than a bare frequency, because a tap has to set the MODE too.
        s_label_target[used].freq_hz = sp->freq_hz;
        s_label_target[used].mode    = (uint8_t)sp->mode;
        lv_obj_set_user_data(lb, (void *)(uintptr_t)(used + 1));
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        s_w->row[i] = (int8_t)row;          // the line pass needs this
        used++;
    }

    // Lines: from just under each spot's own label DOWN to the frequency axis, so
    // the line points at the frequency it marks.
    //
    // ONLY for spots that got a name. A line with no callsign above it marks a
    // frequency without saying whose it is, which is clutter rather than
    // information - and on screen it read as a leftover from a label that had
    // just aged out. So: no label, no line.
    int tick_n = 0;
    for (int i = 0; i < vis_n && tick_n < MAX_TICKS; i++) {
        if (s_w->row[i] < 0) continue;          // unnamed: draw nothing at all
        int si = s_w->idx[i];
        lv_obj_t *t = s_ticks[tick_n];
        if (!t) break;
        int top = line_top_y(s_lane_h, s_w->row[i]);
        int len = s_lane_h - top;
        if (len < 4) len = 4;
        lv_obj_set_pos(t, s_w->x[i], top);
        lv_obj_set_size(t, 2, len);
        lv_obj_set_style_bg_color(t, lv_color_hex(s_w->colour[si]), 0);
        // Same opacity as the callsign, so line and name read as one object.
        lv_obj_set_style_bg_opa(t, s_w->opa[si], 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_HIDDEN);
        tick_n++;
    }

    // Off-screen counts, so the lane says "there is more, that way" instead of
    // silently implying the band is empty outside the window.
    char b[16];
    if (off_l > 0 && s_edge_l) {
        snprintf(b, sizeof(b), "<%d", off_l);
        lv_label_set_text(s_edge_l, b);
        // Coloured like the spot it will take you to, so it reads as part of the
        // same picture rather than as chrome (operator's request).
        lv_obj_set_style_text_color(s_edge_l, lv_color_hex(s_off_l.colour ? s_off_l.colour : COL_POTA), 0);
        lv_obj_clear_flag(s_edge_l, LV_OBJ_FLAG_HIDDEN);
        if (s_edge_l_hit) lv_obj_clear_flag(s_edge_l_hit, LV_OBJ_FLAG_HIDDEN);
    }
    if (off_r > 0 && s_edge_r) {
        snprintf(b, sizeof(b), "%d>", off_r);
        lv_label_set_text(s_edge_r, b);
        lv_obj_set_style_text_color(s_edge_r, lv_color_hex(s_off_r.colour ? s_off_r.colour : COL_POTA), 0);
        lv_obj_align(s_edge_r, LV_ALIGN_BOTTOM_RIGHT, -EDGE_INSET, -EDGE_INSET);
        lv_obj_clear_flag(s_edge_r, LV_OBJ_FLAG_HIDDEN);
        if (s_edge_r_hit) lv_obj_clear_flag(s_edge_r_hit, LV_OBJ_FLAG_HIDDEN);
    }

    s_drawn_lo = s_view_lo;
    s_drawn_hi = s_view_hi;
    s_drawn_version = spots_version();

    // Log only when the picture actually changes. The lane is on a display this
    // session cannot screenshot (the bench is behind a hotel subnet), so this is
    // how the mapping and the row packing get verified at all - and because it is
    // change-gated it stays silent while the view is still, instead of adding a
    // line a second to the diag ring.
    static int l_vis = -1, l_named = -1, l_l = -1, l_r = -1;
    if (vis_n != l_vis || used != l_named || off_l != l_l || off_r != l_r) {
        l_vis = vis_n; l_named = used; l_l = off_l; l_r = off_r;
        ESP_LOGI(TAG, "lane: %d visible, %d drawn (name+line) off L%d R%d | window %lu-%lu Hz",
                 vis_n, used, off_l, off_r,
                 (unsigned long)s_view_lo, (unsigned long)s_view_hi);
    }
}

// 1 Hz is enough: the feed refreshes once a minute and the only other thing
// that changes on its own is age. View changes repaint immediately via
// spots_lane_set_view(), so tuning does not wait for this tick.
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_visible) return;
    repaint();
}

// ---- tap to tune -----------------------------------------------------------

// Tapping a CALLSIGN tunes to it. The tap target is the label itself, never the
// container: a transparent container over the whole spectrum would swallow
// tap-to-tune, the one-finger pan and pinch-zoom, all of which live there. Tiny
// targets are also why each label gets a generous ext_click_area.
// The CAT mode a spot should be worked in. NULL means "leave the mode alone" -
// an unknown mode is not a reason to change the radio out from under the
// operator. SSB splits at 10 MHz by the usual convention (LSB below, USB above).
static const char *cat_mode_for_spot(uint8_t mode, uint32_t hz)
{
    switch ((spot_mode_t)mode) {
    case SPOT_MODE_CW:   return "CW";
    case SPOT_MODE_DIGI: return "DIGI";
    case SPOT_MODE_SSB:  return hz >= 10000000u ? "USB" : "LSB";
    default:             return NULL;
    }
}

// Tune to a spot: frequency AND mode. Frequency alone is not much use - landing
// on a CW activation while the radio is in USB just gives you a whistle
// (operator's point, 2026-08-05).
//
// Bandwidth is deliberately NOT set here. The QMX keeps a filter per mode and
// reloads it on a mode CHANGE, so the right width follows from the mode by
// itself - and forcing one would collide with the hard-won SSB filter dance
// (MMSSB|Filter RX + MMSSB|Bandwidth + FW; suppression, see CLAUDE.md).
static void tune_to_spot(const tap_target_t *t, const char *what)
{
    if (!t->freq_hz) return;
    const char *mode = cat_mode_for_spot(t->mode, t->freq_hz);
    ESP_LOGI(TAG, "%s -> %lu Hz mode=%s", what, (unsigned long)t->freq_hz,
             mode ? mode : "(unchanged)");
    cat_set_frequency_forced(t->freq_hz);   // deliberate user action, bypass the rate limiter
    ui_update_frequency(t->freq_hz);        // optimistic, same as tap-to-tune on the spectrum
    // Via the poll task: the LVGL thread must never write the CDC pipe directly
    // (it races the FA/MD/FW poll and the QMX answers '?;').
    if (mode) cat_request_mode(mode);
}

static void label_click_cb(lv_event_t *e)
{
    lv_obj_t *lb = lv_event_get_target(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(lb) - 1;
    if (idx < 0 || idx >= MAX_LABELS) return;
    tune_to_spot(&s_label_target[idx], "spot tap");
}

// The off-screen counters are tappable: they are the only route to a spot that
// is not on screen, and tuning to it brings it into view.
static void edge_l_click_cb(lv_event_t *e) { (void)e; tune_to_spot(&s_off_l, "spot tap (off-screen left)"); }
static void edge_r_click_cb(lv_event_t *e) { (void)e; tune_to_spot(&s_off_r, "spot tap (off-screen right)"); }

// ---- build -----------------------------------------------------------------

void spots_lane_build(lv_obj_t *parent, int y, int h)
{
    s_w = heap_caps_calloc(1, sizeof(work_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_w) {
        // Not fatal: the lane just stays empty. Better a missing strip than a
        // dead panadapter.
        ESP_LOGE(TAG, "no PSRAM for spots scratch (%u B) - lane disabled",
                 (unsigned)sizeof(work_t));
    }

    s_lane_h = h;
    s_lane_y = y;

    lv_obj_t *lane = lv_obj_create(parent);
    s_lane = lane;
    lv_obj_set_size(lane, DISPLAY_H_RES, h);
    lv_obj_align(lane, LV_ALIGN_TOP_LEFT, 0, y);
    // Fully transparent and NOT clickable: this sits on top of the whole
    // spectrum, so any background or any hit-testing here would either hide the
    // trace or eat tap-to-tune, the one-finger pan and pinch-zoom.
    lv_obj_set_style_bg_opa(lane, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lane, 0, 0);
    lv_obj_set_style_radius(lane, 0, 0);
    lv_obj_set_style_pad_all(lane, 0, 0);
    lv_obj_clear_flag(lane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(lane, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < MAX_TICKS; i++) {
        lv_obj_t *t = lv_obj_create(lane);
        // Size and position are set per-repaint (they depend on the label row);
        // this is just a starting shape.
        lv_obj_set_size(t, 2, h / 2);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_radius(t, 0, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);   // must not block the spectrum
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
        s_ticks[i] = t;
    }
    for (int i = 0; i < MAX_LABELS; i++) {
        lv_obj_t *lb = lv_label_create(lane);
        lv_obj_set_style_text_font(lb, SPOT_FONT, 0);
        lv_label_set_text(lb, "");
        // Dark backing so bright text stays readable where it crosses a strong
        // signal, kept translucent so the trace still shows through.
        lv_obj_set_style_bg_color(lb, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(lb, LABEL_BG_OPA, 0);
        lv_obj_set_style_pad_hor(lb, 2, 0);
        lv_obj_set_style_radius(lb, 2, 0);
        // The label is the tap target. ext_click_area gives a fingertip
        // something to hit without making the drawn box any bigger.
        lv_obj_add_flag(lb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(lb, 10);
        lv_obj_add_event_cb(lb, label_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);
        s_labels[i] = lb;
    }

    // Off-screen counts sit near the BOTTOM corners: the upper rows belong to the
    // callsigns, and the spectrum's top-right is the burger deadzone. Inset from
    // the very edge (EDGE_INSET) because a target flush against the screen border
    // is awkward to hit - your fingertip runs out of glass.
    //
    // Each gets a DEDICATED invisible hit rectangle rather than a big
    // ext_click_area on the label. ext_click_area is clipped to the parent, so on
    // a label already sitting at the parent's bottom edge it could only grow
    // upward - which is exactly why the first attempt still felt unhittable. The
    // rects are EDGE_HIT_W x EDGE_HIT_H, roughly a fingertip and a half, and they
    // are shown and hidden with their labels so an invisible live target never
    // sits over empty spectrum stealing tap-to-tune.
    s_edge_l_hit = lv_obj_create(lane);
    lv_obj_set_size(s_edge_l_hit, EDGE_HIT_W, EDGE_HIT_H);
    lv_obj_align(s_edge_l_hit, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_edge_l_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_edge_l_hit, 0, 0);
    lv_obj_set_style_pad_all(s_edge_l_hit, 0, 0);
    lv_obj_clear_flag(s_edge_l_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_edge_l_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_edge_l_hit, edge_l_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_edge_l_hit, LV_OBJ_FLAG_HIDDEN);

    s_edge_r_hit = lv_obj_create(lane);
    lv_obj_set_size(s_edge_r_hit, EDGE_HIT_W, EDGE_HIT_H);
    lv_obj_align(s_edge_r_hit, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(s_edge_r_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_edge_r_hit, 0, 0);
    lv_obj_set_style_pad_all(s_edge_r_hit, 0, 0);
    lv_obj_clear_flag(s_edge_r_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_edge_r_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_edge_r_hit, edge_r_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_edge_r_hit, LV_OBJ_FLAG_HIDDEN);

    // Labels created AFTER the rects so they draw on top, and left NON-clickable
    // so the whole rect behaves as one uniform target.
    s_edge_l = lv_label_create(lane);
    lv_obj_set_style_text_font(s_edge_l, SPOT_FONT, 0);
    lv_obj_set_style_bg_color(s_edge_l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_edge_l, LABEL_BG_OPA, 0);
    lv_obj_set_style_pad_hor(s_edge_l, 4, 0);
    lv_obj_set_style_radius(s_edge_l, 3, 0);
    lv_obj_clear_flag(s_edge_l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_edge_l, LV_ALIGN_BOTTOM_LEFT, EDGE_INSET, -EDGE_INSET);
    lv_obj_add_flag(s_edge_l, LV_OBJ_FLAG_HIDDEN);

    s_edge_r = lv_label_create(lane);
    lv_obj_set_style_text_font(s_edge_r, SPOT_FONT, 0);
    lv_obj_set_style_bg_color(s_edge_r, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_edge_r, LABEL_BG_OPA, 0);
    lv_obj_set_style_pad_hor(s_edge_r, 4, 0);
    lv_obj_set_style_radius(s_edge_r, 3, 0);
    lv_obj_clear_flag(s_edge_r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_edge_r, LV_ALIGN_BOTTOM_RIGHT, -EDGE_INSET, -EDGE_INSET);
    lv_obj_add_flag(s_edge_r, LV_OBJ_FLAG_HIDDEN);

    // Panadapter is the page the UI is built in; ui_apply_saved_mode() turns the
    // lane off afterwards if the saved page is FT8. Without this default the
    // lane would be built hidden and a boot straight into Panadapter would show
    // an empty strip.
    s_visible = true;

    lv_timer_create(tick_cb, 1000, NULL);
    ESP_LOGI(TAG, "spots overlay built over spectrum y=%d h=%d scratch=%uB", y, h,
             (unsigned)sizeof(work_t));
    spots_lane_selftest();
}

// ---- self-test -------------------------------------------------------------
//
// The lane is geometry, and geometry is exactly what cannot be checked by
// reading the code back. This bench has no way to see it either - the display is
// behind a subnet that blocks the screenshot endpoint - so the mapping and the
// row packing are verified the same way the CW copilot hints are: drive the real
// helpers with known inputs and assert the answers at boot. Cheap, and it runs
// on every unit, so a future refactor that breaks the mapping says so out loud
// instead of silently pointing callsigns at the wrong frequencies.

#define T_CHECK(cond, ...) do { if (!(cond)) { ESP_LOGE(TAG, "SELFTEST FAIL: " __VA_ARGS__); fails++; } } while (0)

// The overlay's height is passed in at build time, but the row geometry has to be
// checkable without it, so the test pins the value ui.c actually uses.
#define SPECTRUM_H_ASSUMED 200

void spots_lane_selftest(void)
{
    int fails = 0;

    // --- x mapping. 48 kHz window, 1280 px wide.
    uint32_t save_lo = s_view_lo, save_hi = s_view_hi;
    s_view_lo = 14000000; s_view_hi = 14048000;

    int x_lo  = freq_to_x(14000000);
    int x_mid = freq_to_x(14024000);
    int x_hi  = freq_to_x(14048000);
    T_CHECK(x_lo == 0,               "left edge -> %d, want 0", x_lo);
    T_CHECK(x_mid == DISPLAY_H_RES / 2, "centre -> %d, want %d", x_mid, DISPLAY_H_RES / 2);
    T_CHECK(x_hi == DISPLAY_H_RES,   "right edge -> %d, want %d", x_hi, DISPLAY_H_RES);
    // A real one: 14.074 MHz (FT8) is 74/48 of the way past the low edge... i.e.
    // outside this window entirely, which must NOT silently clamp into view.
    T_CHECK(freq_to_x(14074000) > DISPLAY_H_RES, "14.074 should map off-screen right");
    // Quarter-window, the same fraction the axis labels use.
    T_CHECK(freq_to_x(14012000) == DISPLAY_H_RES / 4, "quarter -> %d", freq_to_x(14012000));

    // Zoomed in 8x (6 kHz span): 1 kHz must be ~213 px, so a tick can be told
    // apart from its neighbour 1 kHz away - the case the whole feature is for.
    s_view_lo = 14071000; s_view_hi = 14077000;
    int d = freq_to_x(14075000) - freq_to_x(14074000);
    T_CHECK(d > 200 && d < 226, "1 kHz at 8x -> %d px, want ~213", d);

    // --- age fade
    int64_t now = 1000000;
    T_CHECK(age_opa(now, now)              == LV_OPA_COVER,  "fresh spot not opaque");
    T_CHECK(age_opa(now - 60, now)         == LV_OPA_COVER,  "1 min old not opaque");
    T_CHECK(age_opa(0, now)                == LV_OPA_COVER,  "undated spot must stay visible");
    T_CHECK(age_opa(now + 120, now)        == LV_OPA_COVER,  "clock-skewed spot must stay visible");
    T_CHECK(age_opa(now - AGE_GONE_S, now) == LV_OPA_TRANSP, "30 min old still drawn");
    T_CHECK(age_opa(now - AGE_GONE_S - 999, now) == LV_OPA_TRANSP, "ancient still drawn");
    lv_opa_t half = age_opa(now - AGE_HALF_S, now);
    T_CHECK(half > 100 && half < 140, "15 min old -> opa %d, want ~128", (int)half);
    // Monotonic: a spot must never get *more* solid as it ages.
    lv_opa_t prev = LV_OPA_COVER;
    for (int age = 0; age <= AGE_GONE_S; age += 30) {
        lv_opa_t o = age_opa(now - age, now);
        T_CHECK(o <= prev, "opacity rose at age %d (%d > %d)", age, (int)o, (int)prev);
        prev = o;
    }

    // --- two-row packing
    int re[LABEL_ROWS];
    for (int r = 0; r < LABEL_ROWS; r++) re[r] = -10000;
    T_CHECK(pick_row(100, 50, re) == 0, "first label should take row 0");
    T_CHECK(pick_row(100, 50, re) == 1, "overlapping label should fall to row 1");
    T_CHECK(pick_row(100, 50, re) == 2, "third overlapping label should fall to row 2");
    T_CHECK(pick_row(100, 50, re) == -1, "fourth overlapping label should be line-only");
    // Clear of all rows: back to row 0.
    T_CHECK(pick_row(400, 50, re) == 0, "well-separated label should return to row 0");
    // Exactly LABEL_PAD clear is allowed; one pixel less is not.
    int re2[LABEL_ROWS];
    for (int r = 0; r < LABEL_ROWS; r++) re2[r] = -10000;
    pick_row(0, 50, re2);
    T_CHECK(pick_row(50 + LABEL_PAD, 50, re2) == 0, "exactly LABEL_PAD clear should fit row 0");
    int re3[LABEL_ROWS];
    for (int r = 0; r < LABEL_ROWS; r++) re3[r] = -10000;
    pick_row(0, 50, re3);
    T_CHECK(pick_row(50 + LABEL_PAD - 1, 50, re3) == 1, "one px short should go to row 1");
    // --- vertical geometry
    const int H = SPECTRUM_H_ASSUMED;
    // The label block is centred on the middle of the spectrum.
    int blk_top = label_block_top(H), blk_bot = blk_top + LABEL_ROWS * ROW_H;
    int blk_mid = (blk_top + blk_bot) / 2;
    T_CHECK(blk_mid > H / 2 - ROW_H && blk_mid < H / 2 + ROW_H,
            "label block centre %d is not near mid-spectrum %d", blk_mid, H / 2);
    T_CHECK(blk_top >= 0, "label block starts above the spectrum (%d)", blk_top);
    // Every row must fit, and its line must run DOWNWARDS with usable length.
    for (int r = 0; r < LABEL_ROWS; r++) {
        int ly = label_row_y(H, r);
        T_CHECK(ly + FONT_H < H, "row %d text ends at %d, past the spectrum (%d)", r, ly + FONT_H, H);
        int top = line_top_y(H, r);
        T_CHECK(top > ly, "row %d line starts at %d, ABOVE its own label at %d (must drop)", r, top, ly);
        T_CHECK(H - top >= 8, "row %d line is only %d px to the axis", r, H - top);
    }
    // Rows must be ordered top to bottom.
    for (int r = 1; r < LABEL_ROWS; r++)
        T_CHECK(label_row_y(H, r) > label_row_y(H, r - 1), "row %d is not below row %d", r, r - 1);

    s_view_lo = save_lo; s_view_hi = save_hi;

    if (fails == 0) ESP_LOGI(TAG, "spots lane self-test: PASS");
    else            ESP_LOGE(TAG, "spots lane self-test: %d FAILURE(S)", fails);
}

lv_obj_t *spots_lane_obj(void) { return s_lane; }

int spots_lane_top_hit_y(void)
{
    if (!s_lane_h) return 0;        // not built yet
    return s_lane_y + label_block_top(s_lane_h) - LABEL_EXT_CLICK;
}

void spots_lane_set_view(uint32_t lo_hz, uint32_t hi_hz)
{
    if (hi_hz <= lo_hz) return;
    if (lo_hz == s_view_lo && hi_hz == s_view_hi) return;
    s_view_lo = lo_hz;
    s_view_hi = hi_hz;
    if (s_visible) repaint();          // follow the VFO without waiting for the tick
}

void spots_lane_set_visible(bool visible)
{
    if (!s_lane) return;
    s_visible = visible;
    if (visible) {
        lv_obj_clear_flag(s_lane, LV_OBJ_FLAG_HIDDEN);
        repaint();
    } else {
        lv_obj_add_flag(s_lane, LV_OBJ_FLAG_HIDDEN);
    }
}
