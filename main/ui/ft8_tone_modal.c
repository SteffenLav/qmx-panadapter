// TX tone picker - see ft8_tone_modal.h for why this exists.
//
// The design point that matters: a bare number entry is useless here. Asked to
// "pick a better frequency", an operator cannot know what is better without
// seeing what the band is actually doing (operator, 2026-07-28). So the modal
// leads with a live OCCUPANCY STRIP across the whole 200-2800 Hz FT8 passband,
// built from the very same bitmask ft8_find_clear_tone_hz_near() uses to make
// its automatic choice (ft8_tx_get_tone_occupancy) - so what you see is
// literally what the picker thinks, never a second opinion that could disagree.
// Free slots are also spelled out as numbers underneath, because "which ones
// are free" is a fair question to want answered in words.
//
// Three ways to land on a tone: tap the strip, -50/+50 nudge, or let "Find
// clear slot" choose. There is deliberately NO numeric entry: the whole system
// works on a 50 Hz grid (ft8_find_clear_tone_hz_near only ever returns
// FT8_TX_TONE_MIN_HZ + n*50, and guard bands are +/-1 slot), so the 52 cells of
// the strip ARE every meaningful choice - a typed 1437 is a value nothing else
// in the firmware reasons about. A plain-Hz mode was built for the app's
// frequency pad and then reverted (2026-07-28): it needed changes to shared
// ui.c code that the top-bar keypad and memory modal also depend on, which is
// real regression risk for zero added reach over tapping a cell.

#include "ft8_tone_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "ft8_qso.h"
#include "ft8_tx.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ft8_tone_modal";

// Strip geometry. 52 slots at 21 px = 1092, and the panel is sized to exactly
// that plus its 24 px padding (1140) so the strip - and everything else laid
// out relative to it - is centred in the window with no per-element offsets.
// Cells are deliberately as wide as that allows: this is a finger target in the
// field, and at the original 16 px they were too fine to hit confidently. Rough
// taps are recoverable anyway via -50/+50 once the marker shows where you landed.
#define STRIP_SLOTS_MAX  64
#define STRIP_CELL_W     21
#define STRIP_CELL_GAP    0
#define STRIP_H          56
#define MARKER_W          5
#define MARKER_H         14

// Palette lives in the header now - the FT8 pane's mini strip draws the same
// data and must use the same colours (see ft8_tone_modal.h).
#define COL_FREE      FT8_TONE_COL_FREE
#define COL_BUSY      FT8_TONE_COL_BUSY
#define COL_UNKNOWN   FT8_TONE_COL_UNKNOWN
#define COL_PICK      FT8_TONE_COL_PICK
#define COL_PARTNER   FT8_TONE_COL_PARTNER
// Light grey while a finger is down and dragging: "you are moving this, nothing
// is committed yet". Deliberately far from COL_UNKNOWN's dark grey so a drag
// can't be mistaken for an unknown slot. Kept light grey by the operator's
// explicit decision (2026-07-29) after COL_PICK became white - do not "fix"
// this to a contrasting hue; the marker also grows while dragging, so size
// carries the state even where the two greys read as similar.
#define COL_DRAG      0xD0D4DA

static lv_obj_t *s_modal      = NULL;
static lv_obj_t *s_panel      = NULL;
static lv_obj_t *s_strip      = NULL;
static lv_obj_t *s_cells[STRIP_SLOTS_MAX];
static lv_obj_t *s_readout    = NULL;   // button: shows the tone, opens the freq pad
static lv_obj_t *s_readout_lbl = NULL;
static lv_obj_t *s_partner    = NULL;
static lv_obj_t *s_freelist   = NULL;   // "Free: 250, 300, ..." in words
static lv_obj_t *s_hint       = NULL;
static lv_obj_t *s_apply_btn  = NULL;
static lv_obj_t *s_apply_lbl  = NULL;   // carries the value: "Apply 1450 Hz"
static lv_obj_t *s_cancel_btn = NULL;
static lv_obj_t *s_marker     = NULL;   // amber pointer above the picked cell
static lv_obj_t *s_hold_cb    = NULL;   // "TX Hold" - WSJT-X's Hold Tx Freq
static lv_obj_t *s_hold_note  = NULL;   // spells out what the checkbox will do

static int  s_sel_hz   = FT8_TX_CQ_DEFAULT_FREQ_HZ;  // the value being edited
static bool s_sel_hold = false;   // hold state being edited (committed by Apply)
static int  s_n_slots  = 0;
static bool s_dragging = false;   // finger down on the strip right now

static int slot_to_hz(int slot)
{
    return FT8_TX_TONE_MIN_HZ + slot * FT8_TX_TONE_STEP_HZ;
}

static int hz_to_slot(int hz)
{
    int s = (hz - FT8_TX_TONE_MIN_HZ) / FT8_TX_TONE_STEP_HZ;
    if (s < 0) s = 0;
    if (s_n_slots > 0 && s >= s_n_slots) s = s_n_slots - 1;
    return s;
}

static void hint_set(const char *msg, uint32_t colour)
{
    if (!s_hint) return;
    lv_label_set_text(s_hint, msg ? msg : "");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(colour), 0);
}

// Repaint the strip + the free-slot list + the readout from the CURRENT
// occupancy. Called on every show and after every change, so the picture can't
// drift from the band while the modal sits open.
static void refresh_view(void)
{
    int n_slots = 0, n_stations = 0;
    uint64_t occ = ft8_tx_get_tone_occupancy(&n_slots, &n_stations);
    if (n_slots > STRIP_SLOTS_MAX) n_slots = STRIP_SLOTS_MAX;
    s_n_slots = n_slots;

    int sel_slot     = hz_to_slot(s_sel_hz);
    int partner_hz   = 0;
    int partner_slot = -1;
    if (ft8_qso_get_priority_freq(&partner_hz) && partner_hz > 0)
        partner_slot = hz_to_slot(partner_hz);

    // Free-slot list, in words. Capped: all 52 would neither fit nor help, so
    // show the ones nearest the current pick - those are the ones you'd
    // actually consider moving to.
    char freebuf[256];
    int  fb = 0, n_free = 0;
    freebuf[0] = '\0';

    for (int i = 0; i < n_slots; i++) {
        if (!s_cells[i]) continue;
        bool busy = (occ >> i) & 1ULL;
        uint32_t col;
        if (i == sel_slot)           col = s_dragging ? COL_DRAG : COL_PICK;
        else if (i == partner_slot)  col = COL_PARTNER;
        else if (n_stations == 0)    col = COL_UNKNOWN;
        else                         col = busy ? COL_BUSY : COL_FREE;
        lv_obj_set_style_bg_color(s_cells[i], lv_color_hex(col), 0);
        // The picked cell is drawn FULL height while the rest are inset, so the
        // selection reads as a selection rather than just another colour among
        // 52 near-identical bars.
        bool pick = (i == sel_slot);
        lv_obj_set_size(s_cells[i], STRIP_CELL_W - 2, pick ? STRIP_H - 2 : STRIP_H - 8);
        lv_obj_set_pos(s_cells[i], i * (STRIP_CELL_W + STRIP_CELL_GAP) + 1, pick ? 0 : 3);
        if (!busy) n_free++;
    }

    // Pointer above the strip, centred on the picked cell. Goes grey and grows
    // while dragging, so the whole selection reads as "in hand" until release.
    if (s_marker) {
        int cx = sel_slot * (STRIP_CELL_W + STRIP_CELL_GAP) + STRIP_CELL_W / 2;
        int w  = s_dragging ? MARKER_W + 4 : MARKER_W;
        lv_obj_set_size(s_marker, w, MARKER_H);
        lv_obj_set_style_bg_color(s_marker,
            lv_color_hex(s_dragging ? COL_DRAG : COL_PICK), 0);
        lv_obj_set_pos(s_marker, cx - w / 2, 70 - MARKER_H - 2);
    }

    // Second pass for the text list so it's ordered outward from the pick.
    if (n_stations == 0) {
        snprintf(freebuf, sizeof(freebuf),
                 "Nothing decoded yet - occupancy unknown, so every slot shows grey");
    } else {
        fb = snprintf(freebuf, sizeof(freebuf), "%d of %d slots free near you: ",
                      n_free, n_slots);
        int listed = 0;
        for (int r = 0; r < n_slots && listed < 10; r++) {
            for (int sgn = (r == 0 ? 0 : -1); sgn <= 1 && listed < 10; sgn += 2) {
                int i = sel_slot + (r * sgn);
                if (r == 0) i = sel_slot;
                if (i < 0 || i >= n_slots) continue;
                if ((occ >> i) & 1ULL) continue;
                int used = snprintf(freebuf + fb, sizeof(freebuf) - fb,
                                    "%s%d", listed ? ", " : "", slot_to_hz(i));
                if (used <= 0 || (size_t)(fb + used) >= sizeof(freebuf)) { listed = 10; break; }
                fb += used;
                listed++;
                if (r == 0) break;   // centre slot has no mirror
            }
        }
        if (listed == 0)
            snprintf(freebuf, sizeof(freebuf), "No clear slot anywhere - band is packed");
    }
    if (s_freelist) lv_label_set_text(s_freelist, freebuf);

    bool sel_busy = (n_stations > 0) && ((occ >> sel_slot) & 1ULL);

    if (s_readout_lbl) {
        char rb[24];
        snprintf(rb, sizeof(rb), "%d Hz", s_sel_hz);
        lv_label_set_text(s_readout_lbl, rb);
        // Colour the readout by whether the operator's own choice is clear -
        // the single most useful bit of feedback in this whole modal.
        lv_obj_set_style_border_color(s_readout,
            lv_color_hex(sel_busy ? 0xFF4010 : COL_PICK), 0);
    }

    // Say the verdict in words as well as colour. Colour alone fails exactly
    // the operator who most needs this - outdoors, in sunlight, or colour-blind.
    if (n_stations == 0)
        hint_set("Nothing heard yet - can't tell if this slot is clear", 0xFFA040);
    else if (sel_busy)
        hint_set(LV_SYMBOL_WARNING " Someone is on this slot - pick a green one", 0xFF6020);
    else
        hint_set(LV_SYMBOL_OK " This slot is clear", 0x40C060);

    // Put the value ON the confirm button, so committing is an explicit
    // "apply THIS frequency" rather than a generic Apply you have to trust.
    if (s_apply_lbl) {
        char ab[28];
        snprintf(ab, sizeof(ab), "Apply %d Hz", s_sel_hz);
        lv_label_set_text(s_apply_lbl, ab);
    }

    // Say what the checkbox will actually DO, in words, and keep it in step with
    // the tone being edited - "hold" is meaningless without naming the value
    // being held.
    if (s_hold_note) {
        char nb[96];
        if (s_sel_hold)
            snprintf(nb, sizeof(nb), "Every CQ and reply goes out on %d Hz", s_sel_hz);
        else
            snprintf(nb, sizeof(nb), "TX moves to whichever slot is free");
        lv_label_set_text(s_hold_note, nb);
    }

    if (s_partner) {
        if (partner_hz > 0) {
            char pb[48];
            snprintf(pb, sizeof(pb), "Partner (purple) is on %d Hz", partner_hz);
            lv_label_set_text(s_partner, pb);
        } else {
            lv_label_set_text(s_partner, "");
        }
    }
}

static void set_sel(int hz)
{
    if (hz < FT8_TX_TONE_MIN_HZ) hz = FT8_TX_TONE_MIN_HZ;
    if (hz > FT8_TX_TONE_MAX_HZ) hz = FT8_TX_TONE_MAX_HZ;
    s_sel_hz = hz;
    refresh_view();
}

// Map the live touch point to a slot and select it. One handler on the
// container with an x->slot map rather than 52 per-cell handlers, so a drag
// across cell boundaries is continuous rather than a series of separate hits.
static void strip_pick_from_touch(void)
{
    if (!s_strip) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(s_strip, &a);
    int rel  = p.x - a.x1;
    int slot = rel / (STRIP_CELL_W + STRIP_CELL_GAP);
    if (slot < 0) slot = 0;
    if (s_n_slots > 0 && slot >= s_n_slots) slot = s_n_slots - 1;
    // Dragging past either end pins to the edge rather than doing nothing, so a
    // sloppy sweep still lands somewhere sensible.
    set_sel(slot_to_hz(slot));   // refresh_view() sets the clear/busy verdict
}

// Touch down -> pick goes grey and follows the finger; release -> it commits
// back to amber. Nothing is sent to the radio here: Apply is still the commit.
// PRESS_LOST is handled identically to RELEASED so a finger sliding off the
// strip (or an interrupting gesture) can't strand the selection in drag state.
static void strip_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
        case LV_EVENT_PRESSED:
            s_dragging = true;
            strip_pick_from_touch();
            break;
        case LV_EVENT_PRESSING:
            if (s_dragging) strip_pick_from_touch();
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            s_dragging = false;
            refresh_view();   // repaint committed colours (amber, normal marker)
            break;
        default:
            break;
    }
}

static void step_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    set_sel(s_sel_hz + delta);
}

static void find_clear_cb(lv_event_t *e)
{
    (void)e;
    // Scan outward from the current pick, not from the 1500 Hz default -
    // "something clear NEAR HERE" is what you want when you're deliberately
    // working a corner of the passband.
    int hz = ft8_find_clear_tone_hz_near(s_sel_hz);
    bool stuck = (hz == s_sel_hz);
    set_sel(hz);
    // After set_sel, so it isn't overwritten by refresh_view()'s verdict - and
    // only when the scan genuinely had nowhere to go, which is the one case the
    // verdict alone wouldn't explain.
    if (stuck) hint_set("Nothing clearer nearby - this is the best around here", 0xFFA040);
}

static void hold_cb(lv_event_t *e)
{
    s_sel_hold = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    refresh_view();   // restate the consequence in words
}

static void apply_cb(lv_event_t *e)
{
    (void)e;
    // A running CQ/QSO is moved FIRST: it's the only step that can be refused
    // (mid-burst), and refusing after having already stored the preference
    // would leave the modal half-committed. Nothing running is not a failure
    // any more - the chip is always on screen now, so setting the tone for the
    // NEXT transmission is a perfectly ordinary thing to want.
    if (ft8_qso_get_state() != FT8_QSO_IDLE) {
        char err[64];
        if (!ft8_qso_set_tx_tone_hz(s_sel_hz, err, sizeof(err))) {
            // Stay open: "try again in a moment" is the whole failure mode, so
            // closing would just force a re-open and re-pick.
            ESP_LOGW(TAG, "TX tone %d Hz rejected: %s", s_sel_hz, err);
            hint_set(err, 0xFF6020);
            return;
        }
    }

    ft8_tx_set_tone_pref_hz(s_sel_hz);
    ft8_tx_set_tone_hold(s_sel_hold);
    ESP_LOGI(TAG, "TX tone applied: %d Hz (hold %s)", s_sel_hz, s_sel_hold ? "ON" : "off");
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_btn(lv_obj_t *parent, int w, int h, lv_align_t align,
                          int dx, int dy, uint32_t bg, const char *text,
                          const lv_font_t *font, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, align, dx, dy);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_center(l);
    return b;
}

static void add_axis_label(lv_obj_t *parent, int hz, int strip_w, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    char b[12];
    snprintf(b, sizeof(b), "%d", hz);
    lv_label_set_text(l, b);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    // Position proportionally along the strip, then nudge left by half the
    // label so it reads as centred on its tick rather than starting at it.
    int frac = ((hz - FT8_TX_TONE_MIN_HZ) * strip_w)
               / (FT8_TX_TONE_MAX_HZ - FT8_TX_TONE_MIN_HZ);
    lv_obj_set_pos(l, frac - 22, y);
}

static void modal_build(void)
{
    if (s_modal) return;
    lv_obj_t *scr = lv_screen_active();

    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    s_panel = lv_obj_create(s_modal);
    lv_obj_set_size(s_panel, 1140, 660);   // = strip_w + 2*pad; see the geometry note above
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "TX tone");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_partner = lv_label_create(s_panel);
    lv_label_set_text(s_partner, "");
    lv_obj_set_style_text_color(s_partner, lv_color_hex(COL_PARTNER), 0);
    lv_obj_set_style_text_font(s_partner, &lv_font_montserrat_24, 0);
    lv_obj_align(s_partner, LV_ALIGN_TOP_RIGHT, 0, 14);

    // ---- occupancy strip ----
    const int n_max   = (FT8_TX_TONE_MAX_HZ - FT8_TX_TONE_MIN_HZ) / FT8_TX_TONE_STEP_HZ;
    const int strip_w = n_max * (STRIP_CELL_W + STRIP_CELL_GAP);

    s_strip = lv_obj_create(s_panel);
    lv_obj_set_size(s_strip, strip_w, STRIP_H);
    lv_obj_align(s_strip, LV_ALIGN_TOP_LEFT, 0, 70);
    lv_obj_set_style_bg_color(s_strip, lv_color_hex(0x11141a), 0);
    lv_obj_set_style_border_color(s_strip, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_strip, 1, 0);
    lv_obj_set_style_radius(s_strip, 4, 0);
    lv_obj_set_style_pad_all(s_strip, 0, 0);
    lv_obj_clear_flag(s_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_strip, LV_OBJ_FLAG_CLICKABLE);
    // PRESSED/PRESSING/RELEASED rather than CLICKED: a plain tap is just a
    // press+release, so this covers tapping AND dragging with one path.
    lv_obj_add_event_cb(s_strip, strip_touch_cb, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(s_strip, strip_touch_cb, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(s_strip, strip_touch_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(s_strip, strip_touch_cb, LV_EVENT_PRESS_LOST, NULL);

    for (int i = 0; i < n_max && i < STRIP_SLOTS_MAX; i++) {
        lv_obj_t *c = lv_obj_create(s_strip);
        lv_obj_set_size(c, STRIP_CELL_W - 2, STRIP_H - 8);
        lv_obj_set_pos(c, i * (STRIP_CELL_W + STRIP_CELL_GAP) + 1, 3);
        lv_obj_set_style_bg_color(c, lv_color_hex(COL_UNKNOWN), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_radius(c, 2, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        // Cells are decoration: clicks belong to the strip so the x->slot map
        // is the single source of truth for what got tapped.
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        s_cells[i] = c;
    }

    // Selection pointer. Lives on the PANEL (not the strip) so it can sit just
    // above the strip's top edge; refresh_view() moves it.
    s_marker = lv_obj_create(s_panel);
    lv_obj_set_size(s_marker, MARKER_W, MARKER_H);
    lv_obj_set_style_bg_color(s_marker, lv_color_hex(COL_PICK), 0);
    lv_obj_set_style_bg_opa(s_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_marker, 0, 0);
    lv_obj_set_style_radius(s_marker, 2, 0);
    lv_obj_set_style_pad_all(s_marker, 0, 0);
    lv_obj_clear_flag(s_marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_marker, LV_OBJ_FLAG_CLICKABLE);

    add_axis_label(s_panel, 200,  strip_w, 130);
    add_axis_label(s_panel, 1000, strip_w, 130);
    add_axis_label(s_panel, 1500, strip_w, 130);
    add_axis_label(s_panel, 2000, strip_w, 130);
    add_axis_label(s_panel, 2800, strip_w, 130);

    // Cyan MUST be in here. An earlier version listed only green/red/amber and
    // the operator immediately asked whether the cyan bar was their own tone -
    // it's the partner's. An unlabelled colour on a picker is a trap.
    lv_obj_t *legend = lv_label_create(s_panel);
    lv_label_set_text(legend, "white = YOUR tone      purple = partner's tone      "
                              "green = free      red = busy      "
                              "touch and drag to pick");
    lv_obj_set_style_text_color(legend, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(legend, &lv_font_montserrat_20, 0);
    lv_obj_align(legend, LV_ALIGN_TOP_LEFT, 0, 158);

    // ---- nudge / readout row ----
    // Row geometry derives from the strip width so it stays centred if the slot
    // count or cell width ever changes.
    const int row_y     = 226;   // legend + 30: see the note on BELOW_LEGEND_DY
    const int nudge_w   = 140;
    const int readout_w = 400;
    const int readout_x = (strip_w - readout_w) / 2;

    // Nudges sit halfway between the readout and the ends of the strip rather
    // than hard against those ends: they belong to the readout they modify, and
    // at arm's length they read as unrelated controls.
    const int nudge_lx = (readout_x - nudge_w) / 2;
    const int nudge_rx = readout_x + readout_w + nudge_lx;
    make_btn(s_panel, nudge_w, 80, LV_ALIGN_TOP_LEFT, nudge_lx, row_y, UI_COLOR_PRIMARY,
             "-50", &lv_font_montserrat_32, step_cb,
             (void *)(intptr_t)(-FT8_TX_TONE_STEP_HZ));
    make_btn(s_panel, nudge_w, 80, LV_ALIGN_TOP_LEFT, nudge_rx, row_y,
             UI_COLOR_PRIMARY, "+50", &lv_font_montserrat_32, step_cb,
             (void *)(intptr_t)FT8_TX_TONE_STEP_HZ);

    // Plain readout, NOT a button: there's nothing to type (see the file
    // header). Its border still carries the busy/clear verdict for the current
    // pick, which is the most useful single signal in here.
    s_readout = lv_obj_create(s_panel);
    lv_obj_set_size(s_readout, readout_w, 80);
    lv_obj_align(s_readout, LV_ALIGN_TOP_LEFT, readout_x, row_y);
    lv_obj_set_style_bg_color(s_readout, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(s_readout, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_readout, lv_color_hex(COL_PICK), 0);
    lv_obj_set_style_border_width(s_readout, 2, 0);
    lv_obj_set_style_radius(s_readout, 8, 0);
    lv_obj_set_style_pad_all(s_readout, 0, 0);
    lv_obj_clear_flag(s_readout, LV_OBJ_FLAG_SCROLLABLE);
    s_readout_lbl = lv_label_create(s_readout);
    lv_label_set_text(s_readout_lbl, "-- Hz");
    lv_obj_set_style_text_color(s_readout_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_readout_lbl, &lv_font_montserrat_48, 0);
    lv_obj_center(s_readout_lbl);

    make_btn(s_panel, readout_w, 64, LV_ALIGN_TOP_LEFT, readout_x, 322,
             UI_COLOR_PRIMARY, "Find clear slot", &lv_font_montserrat_28,
             find_clear_cb, NULL);

    // "TX Hold" (WSJT-X's Hold Tx Freq), deliberately placed level with "Find
    // clear slot": they are the two halves of the same question - let the
    // firmware chase a free slot, or stay exactly where I put it. Construction
    // is the textless-checkbox-plus-separate-label pattern the filter modal
    // documents (giving lv_checkbox_set_text() real text makes LVGL render a
    // larger indicator), and the checkbox is positioned before the label is
    // aligned to it.
    {
        static lv_style_t st_ind, st_ind_chk;
        static bool st_inited = false;
        if (!st_inited) {
            lv_style_init(&st_ind);
            lv_style_set_bg_color(&st_ind, lv_color_hex(UI_COLOR_SURFACE_RAISED));
            lv_style_set_border_color(&st_ind, lv_color_hex(UI_COLOR_BORDER));
            lv_style_set_border_width(&st_ind, 2);
            lv_style_set_pad_all(&st_ind, 8);
            lv_style_init(&st_ind_chk);
            lv_style_set_bg_color(&st_ind_chk, lv_color_hex(UI_COLOR_PRIMARY));
            lv_style_set_border_color(&st_ind_chk, lv_color_hex(UI_COLOR_PRIMARY_BORDER));
            st_inited = true;
        }
        s_hold_cb = lv_checkbox_create(s_panel);
        lv_checkbox_set_text(s_hold_cb, "");
        lv_obj_add_style(s_hold_cb, &st_ind, LV_PART_INDICATOR);
        lv_obj_add_style(s_hold_cb, &st_ind_chk, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_align(s_hold_cb, LV_ALIGN_TOP_LEFT, 0, 330);
        lv_obj_add_event_cb(s_hold_cb, hold_cb, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *hl = lv_label_create(s_panel);
        lv_label_set_text(hl, "TX Hold");
        lv_obj_set_style_text_color(hl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(hl, &lv_font_montserrat_28, 0);
        lv_obj_align_to(hl, s_hold_cb, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

        s_hold_note = lv_label_create(s_panel);
        lv_label_set_text(s_hold_note, "");
        lv_label_set_long_mode(s_hold_note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_hold_note, readout_x - 16);
        lv_obj_set_style_text_color(s_hold_note, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_style_text_font(s_hold_note, &lv_font_montserrat_20, 0);
        lv_obj_align(s_hold_note, LV_ALIGN_TOP_LEFT, 0, 374);
    }

    // ---- free slots, in words ----
    s_freelist = lv_label_create(s_panel);
    lv_label_set_text(s_freelist, "");
    lv_label_set_long_mode(s_freelist, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_freelist, strip_w);
    lv_obj_set_style_text_color(s_freelist, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_freelist, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_freelist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_freelist, LV_ALIGN_TOP_LEFT, 0, 430);

    s_hint = lv_label_create(s_panel);
    lv_label_set_text(s_hint, "");
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint, strip_w);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_LEFT, 0, 482);

    s_cancel_btn = lv_btn_create(s_panel);
    lv_obj_set_size(s_cancel_btn, 200, 72);
    lv_obj_align(s_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_cancel_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_border_color(s_cancel_btn, lv_color_hex(0xc04040), 0);
    lv_obj_set_style_border_width(s_cancel_btn, 2, 0);
    lv_obj_set_style_radius(s_cancel_btn, 8, 0);
    lv_obj_add_event_cb(s_cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(s_cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_center(cl);

    // Wider than Cancel: it carries the frequency, so this is the "commit THIS
    // value" button rather than a bare Apply.
    // Green, matching every other commit button in the app (identity/CQ Save,
    // the TX modal's Transmit) - this is the primary action here too.
    s_apply_btn = make_btn(s_panel, 320, 72, LV_ALIGN_BOTTOM_RIGHT, 0, 0,
                           UI_COLOR_SUCCESS, "Apply", &lv_font_montserrat_28,
                           apply_cb, NULL);
    lv_obj_set_style_border_color(s_apply_btn, lv_color_hex(UI_COLOR_SUCCESS_BORDER), 0);
    lv_obj_set_style_border_width(s_apply_btn, 2, 0);
    s_apply_lbl = lv_obj_get_child(s_apply_btn, 0);

    // Snap-on keyboard Enter/Esc is claimed in _show(), not here. Every other
    // modal claims it at build time, but those are all built eagerly at
    // ui_init; this one builds lazily on first open, so claiming here would
    // hand Enter/Esc over at an arbitrary moment (the first time the operator
    // ever taps the chip) and never give it back.

    ESP_LOGI(TAG, "TX tone modal built (%d slots, strip %d px)", n_max, strip_w);
}

void ft8_tone_modal_init(void)
{
    modal_build();
}

void ft8_tone_modal_show(void)
{
    modal_build();
    if (!s_modal) return;

    // Open on the tone actually in use, falling back to the stored preference
    // (which is what the chip shows while nothing is running) - never on a
    // constant, or the picker would appear to forget the operator's last choice.
    int cur = ft8_qso_get_tx_tone_hz();
    if (cur <= 0) cur = ft8_tx_get_tone_hz();
    if (cur <= 0) cur = ft8_tx_get_tone_pref_hz();
    if (cur <= 0) cur = FT8_TX_CQ_DEFAULT_FREQ_HZ;
    s_sel_hz   = cur;
    s_sel_hold = ft8_tx_get_tone_hold();
    if (s_hold_cb) {
        if (s_sel_hold) lv_obj_add_state(s_hold_cb, LV_STATE_CHECKED);
        else            lv_obj_remove_state(s_hold_cb, LV_STATE_CHECKED);
    }

    hint_set("", UI_COLOR_TEXT_MUTED);
    refresh_view();
    ui_kbd_set_buttons(s_apply_btn, s_cancel_btn);   // see modal_build()
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
}
