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

// Object pool sizes. Ticks are cheap (a 2 px rect), labels are not - each is an
// LVGL label with its own text buffer - so there are deliberately more ticks
// than labels: at high density the lane degrades to "ticks everywhere, names on
// the ones worth naming" rather than dropping spots entirely.
#define MAX_TICKS   40
#define MAX_LABELS  16

#define ROW_H       17
#define ROW_TOP      2    // first label row, just inside the top of the spectrum
#define LABEL_PAD    5    // horizontal clearance between two labels in a row

// See-through: the vertical line is faint enough to read the trace through it,
// and the label carries a dark backing so bright text stays legible where it
// crosses a strong signal. Both are scaled by the spot's age opacity.
#define LINE_OPA_MAX  90      // of 255
#define LABEL_BG_OPA  120     // of 255, behind the text only

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

static int       s_lane_h;         // = SPECTRUM_H; the overlay's own height
static uint32_t  s_view_lo, s_view_hi;
static uint32_t  s_drawn_lo, s_drawn_hi;
static uint32_t  s_drawn_version = 0xFFFFFFFFu;
static bool      s_visible;

// Spots currently drawn, for hit-testing a tap. Parallel to what we painted.
typedef struct { int x; uint32_t freq_hz; } hit_t;
static hit_t s_hits[MAX_TICKS];
static int   s_hit_n;

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
} work_t;
static work_t *s_w;

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
// the row a label of width w starting at x can use, or -1 if both are taken.
// row_end[] carries the right edge of the last label placed in each row and is
// advanced on success.
static int pick_row(int x, int w, int row_end[2])
{
    for (int r = 0; r < 2; r++) {
        if (x >= row_end[r] + LABEL_PAD) { row_end[r] = x + w; return r; }
    }
    return -1;
}

static void hide_all(void)
{
    for (int i = 0; i < MAX_TICKS; i++)  if (s_ticks[i])  lv_obj_add_flag(s_ticks[i],  LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < MAX_LABELS; i++) if (s_labels[i]) lv_obj_add_flag(s_labels[i], LV_OBJ_FLAG_HIDDEN);
    if (s_edge_l) lv_obj_add_flag(s_edge_l, LV_OBJ_FLAG_HIDDEN);
    if (s_edge_r) lv_obj_add_flag(s_edge_r, LV_OBJ_FLAG_HIDDEN);
    s_hit_n = 0;
}

// ---- repaint ---------------------------------------------------------------

static void repaint(void)
{
    if (!s_lane || !s_visible || !s_w) return;

    // Opting out empties the strip rather than reclaiming its pixels: the lane
    // height is compile-time (it sizes the waterfall buffer), so the honest
    // behaviour is a blank strip, not a relaid-out screen. See spots_lane.h.
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
        if (f < s_view_lo) { if (f >= band_lo) off_l++; continue; }
        if (f > s_view_hi) { if (f <= band_hi) off_r++; continue; }
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

    // Ticks for everything visible (up to the pool), so density is always
    // honestly represented even when names are dropped.
    int tick_n = 0;
    for (int i = 0; i < vis_n && tick_n < MAX_TICKS; i++) {
        int si = s_w->idx[i];
        lv_obj_t *t = s_ticks[tick_n];
        if (!t) break;
        lv_obj_set_pos(t, s_w->x[i], 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(s_w->colour[si]), 0);
        // Scale the line's translucency by age so a fading spot fades as a whole.
        lv_obj_set_style_bg_opa(t, (lv_opa_t)((int)LINE_OPA_MAX * s_w->opa[si] / 255), 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_HIDDEN);
        s_hits[tick_n].x = s_w->x[i];
        s_hits[tick_n].freq_hz = s_w->buf[si].freq_hz;
        tick_n++;
    }
    s_hit_n = tick_n;

    // Which visible spots deserve a name: best-priority first, capped by the
    // label pool.
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

    // Greedy two-row packing. Two rows, not three: a third row would cost
    // another 15 px of waterfall for the rare case that two rows cannot hold
    // the names, and at that density the lane is unreadable anyway - degrading
    // to ticks-only is the more honest answer than stacking labels into a wall.
    int row_end[2] = { -10000, -10000 };
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

        lv_obj_set_pos(lb, x, ROW_TOP + row * ROW_H);
        lv_obj_set_style_text_color(lb, lv_color_hex(s_w->colour[si]), 0);
        lv_obj_set_style_text_opa(lb, s_w->opa[si], 0);
        lv_obj_set_style_bg_opa(lb, (lv_opa_t)((int)LABEL_BG_OPA * s_w->opa[si] / 255), 0);
        // The label is the tap target (the container cannot be, or it would eat
        // every spectrum gesture), so it carries its own frequency.
        lv_obj_set_user_data(lb, (void *)(uintptr_t)sp->freq_hz);
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        used++;
    }

    // Off-screen counts, so the lane says "there is more, that way" instead of
    // silently implying the band is empty outside the window.
    char b[16];
    if (off_l > 0 && s_edge_l) {
        snprintf(b, sizeof(b), "<%d", off_l);
        lv_label_set_text(s_edge_l, b);
        lv_obj_clear_flag(s_edge_l, LV_OBJ_FLAG_HIDDEN);
    }
    if (off_r > 0 && s_edge_r) {
        snprintf(b, sizeof(b), "%d>", off_r);
        lv_label_set_text(s_edge_r, b);
        lv_obj_align(s_edge_r, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
        lv_obj_clear_flag(s_edge_r, LV_OBJ_FLAG_HIDDEN);
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
        ESP_LOGI(TAG, "lane: %d visible (%d named, %d ticks) off L%d R%d | window %lu-%lu Hz",
                 vis_n, used, tick_n, off_l, off_r,
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
static void label_click_cb(lv_event_t *e)
{
    lv_obj_t *lb = lv_event_get_target(e);
    uint32_t hz = (uint32_t)(uintptr_t)lv_obj_get_user_data(lb);
    if (!hz) return;
    ESP_LOGI(TAG, "spot tap -> %lu Hz", (unsigned long)hz);
    cat_set_frequency_forced(hz);      // deliberate user action, bypass the rate limiter
    ui_update_frequency(hz);           // optimistic, same as tap-to-tune on the spectrum
}

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
        // A full-height translucent line down the trace, Flex-style.
        lv_obj_set_size(t, 2, h);
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
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
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

    // Off-screen counts go in the BOTTOM corners: the top rows belong to the
    // callsigns, and the spectrum's top-right is the burger deadzone.
    s_edge_l = lv_label_create(lane);
    lv_obj_set_style_text_font(s_edge_l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_edge_l, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_bg_color(s_edge_l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_edge_l, LABEL_BG_OPA, 0);
    lv_obj_align(s_edge_l, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    lv_obj_add_flag(s_edge_l, LV_OBJ_FLAG_HIDDEN);

    s_edge_r = lv_label_create(lane);
    lv_obj_set_style_text_font(s_edge_r, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_edge_r, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_bg_color(s_edge_r, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_edge_r, LABEL_BG_OPA, 0);
    lv_obj_align(s_edge_r, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
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
    int re[2] = { -10000, -10000 };
    T_CHECK(pick_row(100, 50, re) == 0, "first label should take row 0");
    T_CHECK(pick_row(100, 50, re) == 1, "overlapping label should fall to row 1");
    T_CHECK(pick_row(100, 50, re) == -1, "third overlapping label should be tick-only");
    // Clear of both: back to row 0.
    T_CHECK(pick_row(400, 50, re) == 0, "well-separated label should return to row 0");
    // Exactly LABEL_PAD clear is allowed; one pixel less is not.
    int re2[2] = { -10000, -10000 };
    pick_row(0, 50, re2);
    T_CHECK(pick_row(50 + LABEL_PAD, 50, re2) == 0, "exactly LABEL_PAD clear should fit row 0");
    int re3[2] = { -10000, -10000 };
    pick_row(0, 50, re3);
    T_CHECK(pick_row(50 + LABEL_PAD - 1, 50, re3) == 1, "one px short should go to row 1");

    s_view_lo = save_lo; s_view_hi = save_hi;

    if (fails == 0) ESP_LOGI(TAG, "spots lane self-test: PASS");
    else            ESP_LOGE(TAG, "spots lane self-test: %d FAILURE(S)", fails);
}

lv_obj_t *spots_lane_obj(void) { return s_lane; }

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
