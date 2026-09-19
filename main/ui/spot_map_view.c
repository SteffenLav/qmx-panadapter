// Contributed by Uwe DL8UG, who wrote this module and sent it as a patch.
// Ported by him from his own rbn_monitor project. What changed on the way
// in - the spot map being opt-in rather than always running - is in the
// merge commit and in settings.h under spotmap_en.
// Full-screen spot-map overlay - see spot_map_view.h. Overlay skeleton (full-
// screen hidden/foregrounded screen child, header + Exit button, sidebar +
// lv_tabview) modelled on reader_view.c and the sibling rbn_monitor project's
// map_view; kept unchanged from the first version (operator confirmed the
// shape live on hardware, see memory feedback_qmx_panadapter_spotmap_design).
//
// PURE SELF-SPOTTING (rebuilt 2026-09-10, operator's call after seeing the
// first "all spots" version on hardware): this does NOT show other stations'
// activity (that is what net/spots.c's spot lane is for). It shows who is
// hearing OUR OWN signal, on the three networks that can answer that:
//   - RBN (net/rbn.c): reports our own CQ back to us via net/rbn.c's self-spot
//     capture, exactly like any other station's, the moment a skimmer copies
//     it - CW/RTTY only.
//   - PSK Reporter (net/pskr_self.c): a LIVE MQTT subscription, filtered
//     SERVER-SIDE on tx_call = our own callsign - FT8/FT4/digital modes.
//     Deliberately not net/psk_rx.c's periodic HTTP/XML query (used by the
//     web UI's separate "Who is hearing me" report): that one is rate-limited
//     to once per 5 minutes and answered a different question. MQTT reports
//     arrive within seconds of actually being heard, same as RBN.
//   - WSPR (net/wspr_self.c): a periodic query against wsprnet.org's public
//     "olddb" lookup, filtered on our own callsign - WSPR has no live push
//     feed the way RBN/PSK Reporter do, so this is the one source that is
//     genuinely polled rather than pushed to us.
// A great-circle line is drawn from our own QTH (storage/settings.h's my_grid)
// to each station that reported hearing us, coloured by source.

#include "spot_map_view.h"
#include "ui_theme.h"
#include "ui.h"                 // ui_help_overlay_changed(), ui_open_user_manual()
#include "help_triage.h"        // help_triage_open() - the settings drawer's "Need guidance?"
#include "net/rbn.h"
#include "net/pskr_self.h"
#include "net/wspr_self.h"
#include "net/band_conditions.h"
#include "util/world_map_data.h"
#include "util/maidenhead.h"
#include "util/geo_coords.h"    // per-prefix coordinates for placing a station
#include "util/country.h"       // country_display() - the ONE country-name rule, shared with the FT8/WSPR lists
#include "util/format_freq.h"
#include "storage/settings.h"
#include "cat.h"                // cat_get_frequency() - the header's own info line
#include "adif/adif_log.h"      // adif_log_band_for_freq() - the ONE band table, see its own comment

#include "esp_log.h"
#include "esp_attr.h"           // EXT_RAM_BSS_ATTR
#include "esp_heap_caps.h"      // the map cache's PSRAM buffer
#include "esp_timer.h"          // the Exit button's press duration (logged)
#include "esp_lcd_touch.h"      // raw multi-touch read for the MAP tab's pinch-zoom
#include <string.h>
#include <stdio.h>
#include <stdlib.h>             // qsort() - LIST tab column sort
#include <time.h>
#include <math.h>

static const char *TAG = "spot_map_view";

// Same extern ui.c's own pinch_poll_cb() uses to reach the raw touch driver -
// there is no header for it, bsp_display_get_touch_handle() is just declared
// this way at every call site.
extern esp_lcd_touch_handle_t bsp_display_get_touch_handle(void);

// Same logical landscape geometry as reader_view.c / ft8_screen_view.c.
#define SCR_W      1280
#define SCR_H      720
#define HEADER_H   64
/* Widened 220 -> 270 with the sidebar's fonts (see add_filter_checkbox):
 * "Digi (PSKR)" at montserrat_26 plus a 31 px indicator does not fit 220. Kept
 * as the width of the SETTINGS DRAWER now (2026-09-13 restructure below) -
 * same content, same font sizing, just reached by a right-edge swipe instead
 * of sitting permanently on screen.
 *
 * Widened AGAIN 270 -> 340, 2026-09-15: the User Manual/Need Guidance? pair
 * added later reused this same font at the FULL BUTTON WIDTH (SIDEBAR_W - 28,
 * no wrapping), and "Need guidance?" plus its icon does not fit 270 either -
 * the label ran off the right edge of the display, screenshot-confirmed. Same
 * class of mistake as the checkbox row above, just found later because it is
 * a different pair of widgets. */
#define SIDEBAR_W  340
/* MAP/LIST/CONDITIONS, now lv_tabview's OWN left-side tab bar rather than a
 * horizontal strip along the top. Operator, 2026-09-13: "move the buttons MAP
 * LIST and CONDITIONS to the now empty left panel as buttons - freeing up
 * that space they occupied for map estate." Narrower than SIDEBAR_W - three
 * short words need far less than a checkbox column did, and every pixel here
 * is map/list/conditions estate given back. */
#define TAB_BAR_W  140
/* Same numbers the main app's own right-edge swipe uses (ui.c), reused rather
 * than re-derived - there is no reason this gesture should feel different
 * from the one it replaces. */
#define SS_EDGE_ZONE_PX   30
#define SS_EDGE_MIN_DX    60

static lv_obj_t *s_overlay    = NULL;
static lv_obj_t *s_map_obj    = NULL;   // MAP tab: TOUCH ONLY (drag/pinch) - draws nothing, see map_cache_rebuild()
static lv_obj_t *s_map_bg_obj = NULL;   // MAP tab: lv_canvas holding the WHOLE rendered map (coastline + spots)
static void map_mark_dirty(void);       // zoom/pan/position changed: re-render the cached coastline - see map_cache_rebuild()
static void map_spots_changed(void);    // spot data/filters/age changed: re-snapshot + redraw only the spot layer
static lv_obj_t *s_table_list = NULL;   // Tabelle tab: scrollable row list
static lv_obj_t *s_grid_warn  = NULL;   // "set my_grid" notice, shown when it's empty
static lv_obj_t *s_tabview    = NULL;   // so map_pinch_poll_cb() can tell MAP is the visible tab
static lv_obj_t *s_zoom_dd    = NULL;   // greyed out on LIST/PROP - see tabview_changed_cb
static lv_obj_t *s_info_lbl   = NULL;   // callsign/date/time/freq - see update_info_line()
static lv_timer_t *s_refresh_timer = NULL;
static lv_timer_t *s_pinch_timer   = NULL;
static bool s_active = false;

// MAP tab pinch-zoom + one-finger drag-pan. Zoom is anchored on our own QTH
// (or the map's geometric centre if no grid is set) rather than the pinch
// midpoint - since every drawn line originates at the QTH anyway, zooming in
// around it is the one anchor that is never just empty ocean; panning then
// reaches everywhere else. Both reset on every show() so reopening the
// overlay never starts pre-zoomed/pre-panned from a forgotten previous
// session. s_map_pan_dx/dy are in the same 0..1 screen-fraction units
// project() already works in - see its own comment for how they combine.
/* ⭐ NEW vs OLDER on the map, 30 minutes, per the operator's own wording:
 * "the age like New: <30min Older: >30min" (2026-09-12). One number, used by
 * the map's opacity AND by the sidebar legend, so the picture and the words
 * describing it cannot drift apart. */
#define SELF_SPOT_NEW_SEC (30 * 60)

/* Older spots are shown by default - the ask was to see "all that is shown in
 * the list", and the list has never hidden them. The checkbox exists so a busy
 * map can be cut back to what is live right now, which is the earlier request
 * for "a checkbox for history data". */
static bool s_show_older = true;

/* ⭐ HAS THE OPERATOR TAKEN OVER THE VIEW?
 *
 * The auto-fit deliberately runs only when the map is OPENED - re-framing
 * underneath a finger mid-pinch would be unusable, and that is why the fit's
 * own comment forbids running it on the refresh timer.
 *
 * But that left a real gap: spots arrive live, and one further away than
 * anything present at open lands outside the frame and is never seen. The
 * operator, 2026-09-12: "some of them is then further away than the starting
 * zoom level - can you zoom out as they come in? Still same bounderies?"
 *
 * So the rule is ownership, not timing: the map keeps re-fitting itself while
 * the view is still the one IT chose, and stops the instant a pinch or a drag
 * makes the view the operator's. Nothing moves under anyone's hand, and a map
 * nobody has touched stays honest about what it is receiving. Re-opening the
 * overlay hands control back. */
static bool s_view_is_users = false;

static float s_map_zoom = 1.0f;
static void map_sync_scroll_chain(void);   // defined with the drag-pan, far below
static float s_map_pan_dx = 0.0f, s_map_pan_dy = 0.0f;
/* Screen fraction the current pinch is anchored on - see map_pinch_poll_cb. */
static float s_map_pinch_fx = 0.5f, s_map_pinch_fy = 0.5f;
/* Last touch point LVGL reported on the map, in SCREEN coordinates. Written by
 * map_drag_cb (where an indev is valid) and read by the pinch timer (where one
 * is not). See the comment at its capture. */
static lv_point_t s_map_last_pt;
static bool       s_map_have_last_pt = false;
#define MAP_ZOOM_MIN 1.0f
/* ⛔ THE CHALLENGE THE OPERATOR ASKED FOR, TWICE NOW. This was 8.0, then 10 -
 * the second time (2026-09-16, right after the coastline decimation fix
 * below started showing Italy's real boot shape instead of a box): "I would
 * like to be able to zoom in further by pinching (until it make no sense)".
 * That is a real ceiling to aim for, not "as far as possible" - the source
 * data was simplified at generation time to a 0.05 deg Douglas-Peucker
 * tolerance (tools/gen_world_map.py), so past a certain zoom every remaining
 * "detail" is just that tolerance's own straight-line segments getting large
 * enough to see, not real coastline. 50 is comfortably past that (at x50 the
 * screen covers roughly a European country's width - the segments are still
 * far smaller than that) while stopping short of zooming into visibly blocky
 * nothing. Raised again so pinch and the dropdown (below) keep sharing one
 * ceiling - whichever one you used last, the other is never surprised by it. */
#define MAP_ZOOM_MAX 50.0f

// Filter: CW (RBN), Digi (PSK Reporter) and WSPR (net/wspr_self.c) are the
// only three sources there are, so this is three checkboxes, not the eight
// the "all spots" version had.
static bool s_show_cw   = true;
static bool s_show_digi = true;
static bool s_show_wspr = true;

// Own QTH + one entry per station that reported hearing us. Rebuilt from
// net/rbn.c + net/pskr_self.c + net/wspr_self.c each refresh - each of those
// is its own ring buffer of the last 100 finds (RBN_SELF_MAX / PSKR_SELF_MAX
// / WSPR_SELF_MAX), so there is no reason to hold a second cache here; this
// just needs room for all three combined.
#define SELF_SPOT_MAX 300

// Three independent sources - see net/rbn.c (CW), net/pskr_self.c (Digi,
// live MQTT) and net/wspr_self.c (WSPR, periodic wsprnet.org query, no live
// feed exists for it). Kept as a small enum rather than a second bool
// bolted next to is_digi - a third source needs a third state, not two
// booleans hoping never to both be true.
typedef enum { SPOT_SRC_CW, SPOT_SRC_DIGI, SPOT_SRC_WSPR } spot_kind_t;

typedef struct {
    char        call[16];      // who heard us
    /* The reporter's OWN grid, exactly as they sent it - "" when the source
     * does not carry one. RBN skimmers never do (a skimmer reports a callsign,
     * not a location; its position comes from QRZ or a DXCC centroid), so that
     * column is honestly blank for CW rather than filled from a guess. */
    char        grid[7];
    char        mode[8];       // "CW" (RBN), "FT8"/"FT4"/... (PSK Reporter), or "WSPR"
    uint32_t    freq_hz;
    int         snr_db;
    int64_t     heard_unix;
    float       lat, lon;
    bool        has_pos;
    spot_kind_t src;
    int32_t     distance_km;   // -1 if either end's position is unknown
} self_spot_t;

static bool  s_have_me = false;
static double s_my_lat = 0, s_my_lon = 0;

static bool passes_filter(const self_spot_t *sp)
{
    switch (sp->src) {
    case SPOT_SRC_DIGI: return s_show_digi;
    case SPOT_SRC_WSPR: return s_show_wspr;
    default:            return s_show_cw;
    }
}

// ⛔ COLLAPSED BACK INTO ui_theme.h's SHARED PALETTE (operator, 2026-09-16).
// This used to hand-shift its own brighter CW/Digi/WSPR hues - see git
// history for the 2026-09-13 reasoning (contrast against the land/water fill
// at LV_OPA_80/LV_OPA_30) - but a project-wide colour audit turned up FOUR
// independent palettes all claiming "amber"/"green"/"CW" with different
// hexes, this one drifting from ui_theme.h's UI_COLOR_MODE_* by nothing but a
// comment (`/* was ... */`) rather than shared code. Operator's call: one
// palette, everywhere a MODE is the thing being coloured - if the map's
// traces read dim again at these hexes, that is a reason to brighten
// ui_theme.h's palette itself (so the bandplan strip and Memory Channels
// gain the same fix), not to fork a second copy back into existence here.
//
// The sidebar's three source checkboxes ARE this map's legend (add_filter_
// checkbox() below), so their swatches must stay exactly what the map draws -
// both now read the shared UI_COLOR_MODE_* constants directly, so they cannot
// drift from each other again.
//
// ⚠ NOT routed through ui_theme_mode_color(mode_string) - that helper
// substring-matches a free-text mode string and has NO case that returns
// UI_COLOR_MODE_WSPR at all (checked: only DiGi/FT8/FT4/RTTY, USB, LSB, CW
// are recognised, so "WSPR" falls through to its UI_COLOR_KEY_BG default).
// This dispatch is off spot_kind_t, an enum with an unambiguous answer for
// all three cases, so it reads the constants directly rather than going
// through string-matching built for a different, noisier input.
static uint32_t source_color(spot_kind_t src)
{
    switch (src) {
    case SPOT_SRC_DIGI: return UI_COLOR_MODE_DIGI;
    case SPOT_SRC_WSPR: return UI_COLOR_MODE_WSPR;
    default:            return UI_COLOR_MODE_CW;
    }
}

static void format_age(int64_t heard_unix, int64_t now, char *out, size_t out_sz)
{
    if (heard_unix <= 0 || now < heard_unix) { snprintf(out, out_sz, "-"); return; }
    int64_t age = now - heard_unix;
    if (age < 60)         snprintf(out, out_sz, "%llds", (long long)age);
    else if (age < 3600)  snprintf(out, out_sz, "%lldm", (long long)(age / 60));
    else                  snprintf(out, out_sz, "%lldh", (long long)(age / 3600));
}

// German-style thousands separator ('.', not ',') for the LIST tab's
// Distance column - km values into the thousands (a spot on the far side of
// the world is ~20,000 km) read faster with one. km is always >= 0 here;
// distance_km's own -1 "unknown" sentinel is handled by the caller before
// this is ever reached.
static void format_km_dotted(long km, char *out, size_t out_sz)
{
    char digits[16];
    int len = snprintf(digits, sizeof(digits), "%ld", km);
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(digits)) len = sizeof(digits) - 1;

    size_t o = 0;
    for (int i = 0; i < len && o + 1 < out_sz; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = '.';
        if (o + 1 < out_sz) out[o++] = digits[i];
    }
    out[o] = '\0';
}

// Gyula HA3HZ, 2026-09-17: "If I don't include the time and location in the
// screenshot filename, the image itself doesn't convey much information...
// I would like to see my own callsign, the date, the time, and the
// frequency displayed in the 'Selfspotter' line of the header." A
// screenshot is a self-contained record of what it shows, and callsign,
// UTC and dial frequency are exactly the three facts a bare image cannot
// otherwise carry - so this reads it back off the same sources the rest
// of the UI already trusts (settings_get_my_callsign(), time(NULL)/
// gmtime_r() - the same pair status.c uses for the bottom-bar clock,
// cat_get_frequency(), and format_freq_hz() - #302's one shared frequency
// formatter) rather than inventing new ones. Called once at build and
// every refresh_timer_cb tick (1 Hz) - a single small label's text does
// not need change-detection to stay cheap.
static void update_info_line(void)
{
    if (!s_info_lbl || !lv_obj_is_valid(s_info_lbl)) return;

    char call[16];
    settings_get_my_callsign(call, sizeof(call));

    char freq_buf[16];
    format_freq_hz(cat_get_frequency(), g_freq_style, freq_buf, sizeof(freq_buf));

    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);

    char buf[96];
    if (call[0]) {
        snprintf(buf, sizeof(buf), "%s  %s\n%04d-%02d-%02d %02d:%02d UTC",
                 call, freq_buf,
                 tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min);
    } else {
        // No callsign set yet - still show date/time/freq rather than an
        // empty line, same "something is better than a gap" reasoning as
        // s_grid_warn's own row elsewhere in this file.
        snprintf(buf, sizeof(buf), "%s\n%04d-%02d-%02d %02d:%02d UTC",
                 freq_buf,
                 tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min);
    }
    lv_label_set_text(s_info_lbl, buf);
}

// Refreshes s_have_me/s_my_lat/s_my_lon from storage/settings.h's my_grid.
// Called once per gather, not cached across calls - a grid the operator just
// typed in should take effect on the very next refresh tick.
static void refresh_own_position(void)
{
    qmx_settings_t s;
    settings_load_all(&s);
    bool   was_have = s_have_me;
    double was_lat = s_my_lat, was_lon = s_my_lon;
    s_have_me = s.my_grid[0] && maidenhead_to_latlon(s.my_grid, &s_my_lat, &s_my_lon);
    // The zoom anchor and every line start here, so the cached map is stale.
    if (s_have_me != was_have || s_my_lat != was_lat || s_my_lon != was_lon) { map_mark_dirty(); map_spots_changed(); }
}

// Pulls the three self-spot sources into one array. Returns the count.
//
// ⛔ All three scratch buffers below are `static EXT_RAM_BSS_ATTR`, NOT plain
// locals - and that is load-bearing twice over, not style. This function
// (and refresh_timer_cb(), which has its own copies) runs on taskLVGL, whose
// stack is ~8 KB (CLAUDE.md: "Task stacks on this board are TINY - a
// multi-hundred-byte local is a bug until proven otherwise").
// rbn_self_spot_t[100] + pskr_self_spot_t[100] alone is over 10 KB together -
// MORE than the whole stack - and shipped as plain locals once already: Guru
// Meditation "Stack protection fault", task taskLVGL, pinned in minutes by
// the crash record (panic_hook.c) to this exact line.
//
// The first `static`-only fix (no EXT_RAM_BSS_ATTR) traded that crash for a
// quieter one: FOUR of these buffers (two here, two in refresh_timer_cb())
// landed in plain internal .bss - ~20 KB permanently gone from a device
// whose internal heap idles at 6-7 KB free even on this board's "healthy"
// path (CLAUDE.md's "audit every malloc()/static under ~16 KB" rule applies
// to statics exactly as it does to allocations). Field-observed 2026-09-11:
// MQTT ran fine for ~16 minutes, then a keepalive PING timed out
// ("No PING_RESP, disconnected") and EVERY reconnect attempt failed
// ("Error transport connect") for the rest of the session - a new TCP
// socket needs an internal allocation, and with these four buffers eating
// the pool there was none left to give. EXT_RAM_BSS_ATTR (same as
// net/pskr_self.c's own s_store) puts them in PSRAM instead, matching every
// other buffer this feature already got right.
// Bumping either buffer size again must keep this in mind.

// Dev-only synthetic spots, so MAP/LIST/filters/sort/age-fade can be tested
// without waiting on real RBN/PSK-self/wsprnet traffic to accumulate after
// every reboot (operator, 2026-09-13: "now i have to wait wspr tx'ing every
// time you reboot to get access to any kind of list to test the map with").
// Spread across every continent, all three sources, a mix of fresh and
// >30 min "old" ages, so the age fade, per-source filter checkboxes,
// distance/bearing and every LIST column sort all have something real to
// show against. Injected in gather_self_spots() itself so the whole draw/
// layout path under test is the SAME one real spots take - no parallel
// "test mode" rendering to drift out of step with it.
static volatile bool s_test_spots_en = false;
static void refresh_now(void);   // fwd - defined below, needed by the toggle right here

void spot_map_view_set_test_spots(bool on)
{
    s_test_spots_en = on;
    // refresh_timer_cb's change-detection only watches the three REAL feeds'
    // own counts, so toggling the fake set on/off would otherwise sit unseen
    // until real traffic happened to change - exactly what this exists to
    // avoid needing. Force the one redraw directly instead.
    refresh_now();
}

static int gather_test_spots(self_spot_t *out, int max)
{
    if (!s_test_spots_en || max <= 0) return 0;
    typedef struct { const char *call, *mode; spot_kind_t src; float lat, lon; int snr; int age_s; } fake_t;
    static const fake_t FAKE[] = {
        { "W1AW",    "CW",   SPOT_SRC_CW,   41.7f,   -72.7f,   12,    45 },
        { "VK3XYZ",  "CW",   SPOT_SRC_CW,  -37.8f,   145.0f,   -8,  1200 },
        { "JA1ABC",  "CW",   SPOT_SRC_CW,   35.7f,   139.7f,    3,   300 },
        { "ZS6DEF",  "CW",   SPOT_SRC_CW,  -26.2f,    28.0f,  -14,  4000 },
        { "PY2GHI",  "FT8",  SPOT_SRC_DIGI, -23.5f,   -46.6f,   -2,   90 },
        { "G4JKL",   "FT8",  SPOT_SRC_DIGI, 51.5f,     -0.1f,   15,  600 },
        { "9V1MNO",  "FT4",  SPOT_SRC_DIGI,  1.3f,   103.8f,   -6, 2500 },
        { "VE3PQR",  "FT8",  SPOT_SRC_DIGI, 43.7f,   -79.4f,    9,  120 },
        { "OA4STU",  "WSPR", SPOT_SRC_WSPR, -12.0f,   -77.0f,  -18,  200 },
        { "4X1VWX",  "WSPR", SPOT_SRC_WSPR, 32.1f,    34.8f,  -22, 3300 },
        { "EA8YZA",  "WSPR", SPOT_SRC_WSPR, 28.3f,   -16.5f,  -10,   30 },
        { "9M2BCD",  "WSPR", SPOT_SRC_WSPR,  3.1f,   101.7f,  -25, 5400 },
    };
    int n = 0;
    int64_t now = (int64_t)time(NULL);
    for (size_t i = 0; i < sizeof(FAKE) / sizeof(FAKE[0]) && n < max; i++) {
        self_spot_t *o = &out[n++];
        snprintf(o->call, sizeof(o->call), "%s", FAKE[i].call);
        snprintf(o->mode, sizeof(o->mode), "%s", FAKE[i].mode);
        o->freq_hz    = 14074000;
        o->snr_db     = FAKE[i].snr;
        o->heard_unix = now - FAKE[i].age_s;
        o->lat        = FAKE[i].lat;
        o->lon        = FAKE[i].lon;
        o->has_pos    = true;
        o->src        = FAKE[i].src;
        o->distance_km = (s_have_me)
                       ? (int32_t)(haversine_km(s_my_lat, s_my_lon, o->lat, o->lon) + 0.5)
                       : -1;
    }
    return n;
}

static int gather_self_spots(self_spot_t *out, int max)
{
    int n = 0;

    static EXT_RAM_BSS_ATTR rbn_self_spot_t rbn[100];   // NOT internal .bss - see the note above gather_self_spots()
    int rn = rbn_self_spots_get(rbn, 100);
    for (int i = 0; i < rn && n < max; i++) {
        self_spot_t *o = &out[n++];
        // %.15s, not %s: rbn_self_spot_t.skimmer is char[16] (net/rbn.h) but
        // GCC's format-truncation checker loses that bound across the
        // accessor call and assumes an unbounded string - stating the real
        // width keeps -Werror=format-truncation happy, same pattern as
        // adif_log.c's SPIFFS directory listing.
        snprintf(o->call, sizeof(o->call), "%.15s", rbn[i].skimmer);
        snprintf(o->mode, sizeof(o->mode), "CW");
        o->freq_hz    = rbn[i].freq_hz;
        o->snr_db     = rbn[i].snr_db;
        o->heard_unix = rbn[i].heard_unix;
        o->lat        = rbn[i].lat;
        o->lon        = rbn[i].lon;
        o->has_pos    = rbn[i].has_pos;
        o->src        = SPOT_SRC_CW;
        o->distance_km = (s_have_me && o->has_pos)
                       ? (int32_t)(haversine_km(s_my_lat, s_my_lon, o->lat, o->lon) + 0.5)
                       : -1;
    }

    static EXT_RAM_BSS_ATTR pskr_self_spot_t psk[100];  // NOT internal .bss - see the note above gather_self_spots()
    int pn = pskr_self_spots_get(psk, 100);
    for (int i = 0; i < pn && n < max; i++) {
        self_spot_t *o = &out[n++];
        snprintf(o->call, sizeof(o->call), "%.15s", psk[i].call);   // see the note above
        snprintf(o->mode, sizeof(o->mode), "%.7s", psk[i].mode);
        snprintf(o->grid, sizeof(o->grid), "%.6s", psk[i].grid);
        o->freq_hz    = psk[i].freq_hz;
        o->snr_db     = psk[i].snr_db;
        o->heard_unix = psk[i].heard_unix;
        o->lat        = psk[i].lat;
        o->lon        = psk[i].lon;
        o->has_pos    = psk[i].has_pos;
        o->src        = SPOT_SRC_DIGI;
        o->distance_km = (s_have_me && o->has_pos)
                       ? (int32_t)(haversine_km(s_my_lat, s_my_lon, o->lat, o->lon) + 0.5)
                       : -1;
    }

    static EXT_RAM_BSS_ATTR wspr_self_spot_t wspr[100];  // NOT internal .bss - see the note above gather_self_spots()
    int wn = wspr_self_spots_get(wspr, 100);
    for (int i = 0; i < wn && n < max; i++) {
        self_spot_t *o = &out[n++];
        snprintf(o->call, sizeof(o->call), "%.15s", wspr[i].call);   // see the note above
        snprintf(o->mode, sizeof(o->mode), "WSPR");
        snprintf(o->grid, sizeof(o->grid), "%.6s", wspr[i].grid);
        o->freq_hz    = wspr[i].freq_hz;
        o->snr_db     = wspr[i].snr_db;
        o->heard_unix = wspr[i].heard_unix;
        o->lat        = wspr[i].lat;
        o->lon        = wspr[i].lon;
        o->has_pos    = wspr[i].has_pos;
        o->src        = SPOT_SRC_WSPR;
        o->distance_km = (s_have_me && o->has_pos)
                       ? (int32_t)(haversine_km(s_my_lat, s_my_lon, o->lat, o->lon) + 0.5)
                       : -1;
    }
    n += gather_test_spots(&out[n], max - n);
    return n;
}

// ---- Karte tab --------------------------------------------------------

/* Fit the map to what there is to see.
 *
 * The world outline is drawn edge to edge, so a station whose spots are all
 * within a thousand kilometres gets a pinhead of activity in the middle of an
 * empty planet - which is what the operator saw: every trace crammed into
 * Europe with the Pacific taking up half the screen (2026-09-12).
 *
 * Works in the same normalised world coordinates project() uses, so the zoom
 * and pan computed here are exactly what project() will apply: it scales every
 * point away from our own QTH and then shifts by the pan. Two steps:
 *   - zoom so the bounding box of every drawn point fills the view apart from a
 *     MAP_FIT_MARGIN_PX border, so dots near the edge are not clipped;
 *   - pan so that box ends up CENTRED, because the anchor is our QTH and not
 *     the middle of the screen - without this, zooming on a European station
 *     pushes everything off the top.
 *
 * Only ever called when the map is opened. It must not run on the refresh
 * timer: the operator pinches and drags this map, and a view that re-fitted
 * itself underneath them every time a spot arrived would be unusable. */
/* ⛔ THE CLEARANCE IS A DISTANCE ON THE GLASS, NOT A FRACTION OF THE VIEW.
 *
 * This was one constant, 0.72, applied to both axes - so the spots occupied 72%
 * of the pane and the operator got a wide empty border: "you do not zoom to fit
 * enough - i like a couple of mm clearance from the signal path endings - not
 * more" (2026-09-12).
 *
 * A single fraction cannot express "a couple of mm", because the pane is not
 * square: it is 1060 x ~600 px on a 110.7 mm-wide panel, i.e. 11.6 px/mm, so
 * 24 px is 4.5% of the width but 8% of the height. Asking for a pixel margin
 * and deriving the fraction per axis gives the same physical gap top, bottom
 * and sides - which is what "a couple of mm" means.
 *
 * Falls back to the old behaviour if the pane has not been laid out yet, since
 * a zero-sized read would otherwise divide the world by nothing. */
#define MAP_RING_MIN_PX  3     /* a ring smaller than this on BOTH axes is a dot */
#define MAP_SEG_MIN_PX   2     /* drop a point closer than this to the last one DRAWN */
#define MAP_FIT_MARGIN_PX 24.0f    /* ~2 mm at 11.6 px/mm, each edge */
#define MAP_FIT_FALLBACK 0.90f     /* used only before the pane has a size */
#define MAP_FIT_MAX_ZOOM 12.0f     /* a single nearby spot must not fill the world */

static void map_fit_to_spots(void)
{
    // EXT_RAM_BSS_ATTR - missed the first time round, same ~18 KB internal-.bss
    // class this file's other two copies (map_draw_cb's own and gather_self_spots'
    // rbn[]/psk[]/wspr[]) already carry the warning for. This one is a THIRD
    // call site of the identical array, so it is a THIRD ~18 KB if left plain.
    static EXT_RAM_BSS_ATTR self_spot_t spots[SELF_SPOT_MAX];
    int count = gather_self_spots(spots, SELF_SPOT_MAX);

    s_map_zoom   = 1.0f;
    s_map_pan_dx = s_map_pan_dy = 0.0f;
    map_sync_scroll_chain();
    if (!s_have_me) return;                 /* no anchor - project() no-ops anyway */

    /* Our own QTH is always in the box: the great circles start there, so a
     * fit that excluded it would cut every line off at the screen edge. */
    float x0, x1, y0, y1;
    x0 = x1 = ((float)s_my_lon + 180.0f) / 360.0f;
    y0 = y1 = (90.0f - (float)s_my_lat) / 180.0f;

    int n = 0;
    for (int i = 0; i < count; i++) {
        const self_spot_t *sp = &spots[i];
        if (!sp->has_pos || !passes_filter(sp)) continue;
        float wx = (sp->lon + 180.0f) / 360.0f;
        float wy = (90.0f - sp->lat) / 180.0f;
        if (wx < x0) x0 = wx;
        if (wx > x1) x1 = wx;
        if (wy < y0) y0 = wy;
        if (wy > y1) y1 = wy;
        n++;
    }
    if (n == 0) return;                     /* nothing heard - leave the whole world */

    /* Per-axis fraction from a fixed pixel margin - see MAP_FIT_MARGIN_PX. */
    float fx = MAP_FIT_FALLBACK, fy = MAP_FIT_FALLBACK;
    if (s_map_obj) {
        lv_area_t a;
        lv_obj_get_coords(s_map_obj, &a);
        float w = (float)lv_area_get_width(&a);
        float h = (float)lv_area_get_height(&a);
        if (w > 4.0f * MAP_FIT_MARGIN_PX) fx = (w - 2.0f * MAP_FIT_MARGIN_PX) / w;
        if (h > 4.0f * MAP_FIT_MARGIN_PX) fy = (h - 2.0f * MAP_FIT_MARGIN_PX) / h;
    }

    const float spanx = x1 - x0, spany = y1 - y0;
    float zx = (spanx > 0.0001f) ? (fx / spanx) : MAP_FIT_MAX_ZOOM;
    float zy = (spany > 0.0001f) ? (fy / spany) : MAP_FIT_MAX_ZOOM;
    float z  = (zx < zy) ? zx : zy;
    if (z > MAP_FIT_MAX_ZOOM) z = MAP_FIT_MAX_ZOOM;
    if (z < 1.0f) z = 1.0f;                 /* project() only zooms in */

    s_map_zoom = z;
    map_sync_scroll_chain();   /* the fit usually lands zoomed, so this is the
                                * state the operator actually meets */

    /* Centre the box. project() puts a point at ax + (wx - ax) * z + pan, so
     * the box centre lands at ax + (cx - ax) * z and the pan is whatever moves
     * that to the middle of the view. */
    const float ax = ((float)s_my_lon + 180.0f) / 360.0f;
    const float ay = (90.0f - (float)s_my_lat) / 180.0f;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    s_map_pan_dx = 0.5f - (ax + (cx - ax) * z);
    s_map_pan_dy = 0.5f - (ay + (cy - ay) * z);

    ESP_LOGI(TAG, "map fit: %d spot(s), zoom %.2f, pan %.3f/%.3f",
             n, z, s_map_pan_dx, s_map_pan_dy);

    /* ⛔ INVALIDATE, or a re-open of an ALREADY-VISIBLE overlay keeps the old
     * framing. spot_map_view_show() runs on every top-edge swipe and on the
     * spotmap dev action, and when the map is already on screen nothing else
     * marks it dirty - so the zoom changed underneath a picture that was never
     * redrawn. Caught 2026-09-12 only because the screenshots kept coming back
     * unzoomed while the log said "zoom 2.28": the computation was right and
     * the pixels were stale. */
    map_mark_dirty();
}

static lv_point_precise_t project(const lv_area_t *area, int32_t w, int32_t h, float lon, float lat)
{
    float wx = (lon + 180.0f) / 360.0f;
    float wy = (90.0f - lat) / 180.0f;

    // Pinch-zoom (map_pinch_poll_cb): scale every point away from the anchor,
    // then one-finger drag-pan (map_drag_cb) shifts the whole zoomed result.
    // At the default s_map_zoom == 1.0f / pan (0,0) both are no-ops, so
    // nothing here affects the unzoomed view or any of its existing callers.
    if (s_map_zoom > 1.0f) {
        float anchor_lon = s_have_me ? (float)s_my_lon : 0.0f;
        float anchor_lat = s_have_me ? (float)s_my_lat : 0.0f;
        float ax = (anchor_lon + 180.0f) / 360.0f;
        float ay = (90.0f - anchor_lat) / 180.0f;
        wx = ax + (wx - ax) * s_map_zoom + s_map_pan_dx;
        wy = ay + (wy - ay) * s_map_zoom + s_map_pan_dy;
    }

    lv_point_precise_t pt;
    pt.x = area->x1 + (int32_t)(wx * w);
    pt.y = area->y1 + (int32_t)(wy * h);
    return pt;
}

// Great-circle arc between two lat/lon points, approximated as a short
// polyline (spherical linear interpolation between the endpoints' 3D unit
// vectors) - a straight ruler line on this flat equirectangular projection is
// NOT the shortest path over the globe's real surface, e.g. a EU<->US path
// should visibly bow toward the pole. Ported from rbn_monitor's
// draw_great_circle_line, adapted to LVGL 9.2.2's single-segment
// lv_draw_line_dsc_t (see the world-outline loop below for the same
// adaptation) - each of GC_SEGMENTS legs is its own draw call rather than one
// call for the whole arc.
#define GC_SEGMENTS 24
static void draw_great_circle(lv_layer_t *layer, lv_draw_line_dsc_t *dsc,
                               const lv_area_t *area, int32_t w, int32_t h,
                               float lon1, float lat1, float lon2, float lat2)
{
    const float D2R = 3.14159265f / 180.0f, R2D = 180.0f / 3.14159265f;
    float phi1 = lat1 * D2R, lam1 = lon1 * D2R;
    float phi2 = lat2 * D2R, lam2 = lon2 * D2R;
    float x1 = cosf(phi1) * cosf(lam1), y1 = cosf(phi1) * sinf(lam1), z1 = sinf(phi1);
    float x2 = cosf(phi2) * cosf(lam2), y2 = cosf(phi2) * sinf(lam2), z2 = sinf(phi2);

    float dot = x1 * x2 + y1 * y2 + z1 * z2;
    if (dot > 1.0f) dot = 1.0f; else if (dot < -1.0f) dot = -1.0f;
    float d = acosf(dot);

    lv_point_precise_t prev = project(area, w, h, lon1, lat1);
    if (d < 0.0001f) {
        lv_point_precise_t cur = project(area, w, h, lon2, lat2);
        dsc->p1 = prev; dsc->p2 = cur;
        lv_draw_line(layer, dsc);
        return;
    }

    float sin_d = sinf(d);
    for (int i = 1; i <= GC_SEGMENTS; i++) {
        float f = (float)i / GC_SEGMENTS;
        float a = sinf((1.0f - f) * d) / sin_d;
        float b = sinf(f * d) / sin_d;
        float x = a * x1 + b * x2, y = a * y1 + b * y2, z = a * z1 + b * z2;
        float lat = atan2f(z, sqrtf(x * x + y * y)) * R2D;
        float lon = atan2f(y, x) * R2D;
        lv_point_precise_t cur = project(area, w, h, lon, lat);

        // The map wraps at +-180 deg but the path itself does not - a jump
        // over half the screen width means this leg crossed the seam, so skip
        // drawing it (both neighbouring legs still draw normally).
        if (fabsf((float)cur.x - (float)prev.x) <= (float)w / 2.0f) {
            dsc->p1 = prev; dsc->p2 = cur;
            lv_draw_line(layer, dsc);
        }
        prev = cur;
    }
}

/* ⭐⭐ SPLIT FROM THE SPOTS DRAWER, 2026-09-13 - THE WORLD OUTLINE WAS BEING
 * RE-RASTERISED ON EVERY SELF-SPOT ARRIVAL, AND THAT IS WHAT WAS COSTING
 * NEAR-100% OF taskLVGL WITH THE MAP OPEN.
 *
 * Both halves used to live in one map_draw_cb() on one object (s_map_obj),
 * so refresh_now()'s lv_obj_invalidate(s_map_obj) - fired once a second by
 * refresh_timer_cb whenever ANY self-spot count/timestamp changed - repainted
 * the ENTIRE coastline every time, not just the spot lines that actually
 * changed. Reproduced with ZERO spots present and nobody touching the
 * glass: taskLVGL pinned at 97-98% of core 0 within ~2 s of opening the MAP
 * tab, CAT/audio starved (`cdc_acm TX transfer timeout`, RX pairs/s down to
 * a few hundred), and it cleared the instant the map was hidden. The earlier
 * bounding-box + per-ring vertex budget (still below, in THIS function) cut
 * the cost of one redraw a long way - Antarctica 3,801 -> ~475 segments -
 * but a "long way" still leaves several thousand lv_draw_line calls for the
 * full 1:10m world at zoom 1, and that was being paid again every ~1 s.
 *
 * The outline never needs the spot data and only changes on an actual
 * zoom/pan change (map_fit_to_spots, pinch, drag, the Zoom dropdown) - all of
 * which already route through map_sync_scroll_chain() or map_drag_cb()'s own
 * PRESSING branch, both of which now invalidate s_map_bg_obj directly.
 * refresh_now() invalidates ONLY s_map_obj (below), so a routine spot update
 * repaints a handful of great-circle lines and dots, never the coastline. */
/* Coastline pixels go STRAIGHT into the cache buffer with a 1-px Bresenham,
 * not through lv_draw_line. LVGL's software line is anti-aliased and builds a
 * mask per segment - measured 2026-09-13 as core 0 at ~10 % idle and 2.6 fps
 * for as long as zoom/pan kept asking for rebuilds, the operator's "very long
 * latency on clicking zoom". The outline is 1 px and one colour, so a plain
 * integer line gives the same picture for a small fraction of the work. */
static uint8_t *s_map_cache_buf = NULL;   // the cached coastline image, see map_cache_rebuild()
static int32_t  s_map_cache_w = 0, s_map_cache_h = 0, s_map_cache_stride = 0;

// ---- Land FILL, not just an outline (operator, 2026-09-13, second ask -----
// "land lighter than the sea" the first time round only bought a brighter
// OUTLINE (the comment on land_c below records it), because most of any
// landmass is still the plain water colour a few pixels in from its coast.
// A scanline fill over the SAME edges already walked for the outline, at the
// SAME per-ring point budget that was tuned to stop this exact function
// freezing the device (see the ring loop's own comments) - so the added cost
// is one more O(edges) pass, not O(rows x edges): each edge is walked from
// its own y0 to y1 ONCE, depositing one x-crossing per row it spans, and the
// fill itself is a single pass over the rows at the end.
//
// ne_10m_land carries no holes (it is land-vs-water only, no lakes cut out
// of continents), and no two rings overlap, so a plain even-odd rule across
// ALL rings' crossings together - not ring-by-ring - is exactly correct: two
// separate landmasses crossing the same screen row still alternate in/out
// correctly, same as one ring with a hole would if the data ever had one.
#define MAP_SCAN_MAX_X   40   // crossings a single screen row can record - generous for a coastline; further ones on a pathological row are dropped, not a buffer overrun
static int32_t *s_scan_x = NULL;   // [row * MAP_SCAN_MAX_X + slot], PSRAM, resized with the cache
static int16_t *s_scan_n = NULL;   // crossings recorded so far, per row

static void cache_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    const int32_t w = s_map_cache_w, h = s_map_cache_h;
    // Both ends beyond the same edge: nothing of it can land on screen.
    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= w && x1 >= w) || (y0 >= h && y1 >= h)) return;
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (int guard = 0; guard < 8192; guard++) {
        if ((uint32_t)x0 < (uint32_t)w && (uint32_t)y0 < (uint32_t)h) {
            uint8_t *p = s_map_cache_buf + (size_t)y0 * (size_t)s_map_cache_stride + (size_t)x0 * 2;
            p[0] = (uint8_t)(c & 0xFF);
            p[1] = (uint8_t)(c >> 8);
        }
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Record one x-crossing of edge (x0,y0)-(x1,y1) into every integer row it
// spans, half-open [yTop, yBottom) so a vertex shared by two edges of the
// same ring is never counted twice (the standard scanline-fill rule - get
// this wrong and every row touching a vertex flips parity an extra time,
// which either leaves a 1px gap or bleeds fill past the coast there).
static void scan_record_edge(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (y0 == y1) return;                    // horizontal edge: no row crossing
    if (y0 > y1) { int32_t t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    int32_t yy0 = y0 < 0 ? 0 : y0;
    int32_t yy1 = y1 > s_map_cache_h ? s_map_cache_h : y1;   // half-open, so this may equal h
    if (yy0 >= yy1) return;                  // fully above or below the buffer
    // x at row y, linear in y along this edge - exact at the endpoints,
    // which is what keeps adjoining edges (sharing a vertex) agreeing.
    float dxdy = (float)(x1 - x0) / (float)(y1 - y0);
    for (int32_t y = yy0; y < yy1; y++) {
        int32_t x = x0 + (int32_t)((float)(y - y0) * dxdy);
        int16_t n = s_scan_n[y];
        if (n < MAP_SCAN_MAX_X) { s_scan_x[(size_t)y * MAP_SCAN_MAX_X + n] = x; s_scan_n[y] = n + 1; }
    }
}

// One pass over every row, even-odd fill between sorted crossing pairs.
// Called once per map_render_coast(), after every ring has deposited its
// edges - the sort is insertion sort, which is the right choice for the
// handful of crossings (2-8, typically) an actual coastline puts on a row;
// MAP_SCAN_MAX_X bounds the pathological case, not the common one.
static void scan_fill_rows(uint16_t land_c)
{
    for (int32_t y = 0; y < s_map_cache_h; y++) {
        int16_t n = s_scan_n[y];
        if (n < 2) continue;
        int32_t *row = &s_scan_x[(size_t)y * MAP_SCAN_MAX_X];
        for (int16_t i = 1; i < n; i++) {              // insertion sort, n is small
            int32_t v = row[i]; int16_t j = i - 1;
            while (j >= 0 && row[j] > v) { row[j + 1] = row[j]; j--; }
            row[j + 1] = v;
        }
        uint8_t *dst_row = s_map_cache_buf + (size_t)y * (size_t)s_map_cache_stride;
        for (int16_t i = 0; i + 1 < n; i += 2) {        // an odd leftover crossing is dropped, defensively
            int32_t xa = row[i]     < 0 ? 0 : row[i];
            int32_t xb = row[i + 1] > s_map_cache_w ? s_map_cache_w : row[i + 1];
            for (int32_t x = xa; x < xb; x++) {
                uint8_t *p = dst_row + (size_t)x * 2;
                p[0] = (uint8_t)(land_c & 0xFF);
                p[1] = (uint8_t)(land_c >> 8);
            }
        }
    }
}

// A ring's projected, decimated segments handed to a callback rather than
// drawn directly - map_render_coast() below walks every ring TWICE (once to
// fill, once to stroke the coast on top of that fill) and this is the one
// copy of the culling/budget logic both passes share, so it cannot drift
// between them the way two independent copies could.
typedef void (*ring_segment_cb_t)(int32_t x0, int32_t y0, int32_t x1, int32_t y1);

static void walk_ring_segments(const lv_area_t *area, int32_t w, int32_t h, ring_segment_cb_t cb)
{
    /* ⛔ REJECT A RING BY ITS BOUNDING BOX BEFORE WALKING ITS POINTS.
     *
     * One lv_draw_line per EDGE (see above), and the table is now ~16x denser
     * than the 1:110m silhouette it replaced - 19,543 points against 1,280. On
     * a board whose core 0 is already the wall that is not affordable brute
     * force, and this very map redrawing is implicated in audio-ring overflows
     * (`DROPPED=... (ring full)`), so the cull is a precondition of the finer
     * data rather than an optimisation bolted on after it.
     *
     * Two tests, both from the precomputed box, both costing two projections
     * instead of N:
     *   - entirely off-screen: nothing to draw. This is what makes ZOOMING IN
     *     cheap - at 12x over Scandinavia almost every ring on Earth fails
     *     here, so the zoomed view now costs LESS than the coarse table did.
     *   - smaller than MAP_RING_MIN_PX on both axes: a shape that would land
     *     inside a pixel or two, i.e. the several hundred small islands. They
     *     cost nothing to skip and contribute nothing but a dot. This is what
     *     keeps the ZOOMED-OUT view affordable.
     *
     * project() is monotonic in lon and inverted-monotonic in lat, so the two
     * opposite corners of the geographic box project to the two opposite
     * corners of the screen box - min/max rather than assuming which is which. */
    for (int i = 0; i < WORLD_MAP_RING_COUNT; i++) {
        const world_map_ring_t *ring = &WORLD_MAP_RINGS[i];
        int n = ring->point_count;
        if (n < 2) continue;

        lv_point_precise_t c0 = project(area, w, h,
                                        ring->lon_min / WORLD_MAP_UNITS_PER_DEG,
                                        ring->lat_min / WORLD_MAP_UNITS_PER_DEG);
        lv_point_precise_t c1 = project(area, w, h,
                                        ring->lon_max / WORLD_MAP_UNITS_PER_DEG,
                                        ring->lat_max / WORLD_MAP_UNITS_PER_DEG);
        int32_t bx0 = (int32_t)(c0.x < c1.x ? c0.x : c1.x);
        int32_t bx1 = (int32_t)(c0.x < c1.x ? c1.x : c0.x);
        int32_t by0 = (int32_t)(c0.y < c1.y ? c0.y : c1.y);
        int32_t by1 = (int32_t)(c0.y < c1.y ? c1.y : c0.y);

        if (bx1 < area->x1 || bx0 > area->x2 || by1 < area->y1 || by0 > area->y2)
            continue;                                   /* off-screen */
        if ((bx1 - bx0) < MAP_RING_MIN_PX && (by1 - by0) < MAP_RING_MIN_PX)
            continue;                                   /* sub-pixel speck */

        /* ⛔ AND NOW THE HALF THE BOUNDING BOX CANNOT DO: DROP POINTS WHEN THE
         * RING IS DRAWN SMALL. This is what froze the device, and a second cut
         * at it is what un-boxed Italy - both stories below, because the second
         * bug only exists BECAUSE of how the first one was fixed.
         *
         * v1 (2026-09-12): the box test rejects the several hundred tiny
         * islands, which is the cheap half of the bill. It can do nothing about
         * the EXPENSIVE half - at zoom 1, 248 rings still pass it and the two
         * largest are 3,801 and 3,067 points, so a single redraw of the default
         * view issued over ten thousand lv_draw_line calls. taskLVGL stopped
         * keeping up, the SELFSPOTTER screen froze solid, drag and pinch
         * stopped responding and even httpd stopped answering (operator,
         * 2026-09-12: "Selfspotter screen seems to have frozen up completely").
         * I shipped the cull claiming it made the finer data affordable; it
         * made the ZOOMED-IN case affordable and left the default view worse
         * than before. Fixed then with a per-ring pixel BUDGET (roughly one
         * segment per two px of the ring's on-screen half-perimeter) and a
         * fixed INDEX stride across the ring's point array to hit it.
         *
         * v2 (2026-09-16, operator: Italy on the map is "almost a square box"):
         * that index stride is not shape-aware, and Natural Earth is why it
         * mattered. Italy is NOT its own ring - like Denmark and Greece it is a
         * peninsula, so it ships fused into one landmass polygon with the rest
         * of Africa+Eurasia (ring 0 here, 7,707 points, bbox running from West
         * Africa to the Bering Strait). A fixed stride picks every Nth point BY
         * INDEX across that WHOLE ring, so a tightly-curved few hundred points
         * describing the boot got the identical sampling rate as thousands of
         * points along nearly-straight Siberian coastline - and because the
         * boot is a small slice of the ring's total point count, most of the
         * vertices that actually DEFINE its shape were exactly the ones a
         * fixed stride skipped over.
         *
         * Now decimated by ON-SCREEN DISTANCE instead of index: walk every
         * point (this file's own header on project() below already calls the
         * arithmetic cheap - a handful of float ops; it is the DRAW CALL that
         * is expensive) and only emit a segment once the point has moved at
         * least MAP_SEG_MIN_PX from the last point actually drawn. A stretch
         * that Douglas-Peucker already left sparse (long straight coast)
         * clears that distance in one step, same cost as before. A stretch it
         * left dense because it curves (Italy, Denmark, Greece) needs several
         * points to cover the same screen distance and now KEEPS them, because
         * nothing here is tied to a point's position in a 7,707-point ring.
         * Zoomed all the way out, where whole continents used to need the
         * budget cap to avoid the freeze above, the SAME rule self-limits: most
         * consecutive points fall within MAP_SEG_MIN_PX of each other when a
         * landmass is squeezed into a small on-screen box, so the segment count
         * still collapses on its own - not because the fixed budget said so,
         * but because that many points genuinely add nothing visible there. */
        lv_point_precise_t last_drawn = project(area, w, h,
                                                ring->points[0] / WORLD_MAP_UNITS_PER_DEG,
                                                ring->points[1] / WORLD_MAP_UNITS_PER_DEG);
        for (int j = 1; ; j++) {
            bool last = (j >= n);
            int k = (last ? 0 : j) * 2;
            lv_point_precise_t cur = project(area, w, h,
                                             ring->points[k]     / WORLD_MAP_UNITS_PER_DEG,
                                             ring->points[k + 1] / WORLD_MAP_UNITS_PER_DEG);
            int32_t dx = (int32_t)cur.x - (int32_t)last_drawn.x;
            int32_t dy = (int32_t)cur.y - (int32_t)last_drawn.y;
            if (!last && dx * dx + dy * dy < MAP_SEG_MIN_PX * MAP_SEG_MIN_PX)
                continue;   /* too close to the last drawn point to be visible */
            cb((int32_t)last_drawn.x, (int32_t)last_drawn.y, (int32_t)cur.x, (int32_t)cur.y);
            last_drawn = cur;
            if (last) break;
        }
    }
}

// The two callbacks passed to walk_ring_segments() above. Both need a colour
// (scan_record_edge() doesn't, land_c/coast_c do) that the ring_segment_cb_t
// signature has no room for, so it rides in this one file-local instead of
// widening every call site's signature for two users.
static uint16_t s_stroke_color;
static void cb_scan(int32_t x0, int32_t y0, int32_t x1, int32_t y1)   { scan_record_edge(x0, y0, x1, y1); }
static void cb_stroke(int32_t x0, int32_t y0, int32_t x1, int32_t y1) { cache_line(x0, y0, x1, y1, s_stroke_color); }

static void map_render_coast(const lv_area_t *area_in)
{
    lv_area_t area = *area_in;
    int32_t w = lv_area_get_width(&area);
    int32_t h = lv_area_get_height(&area);
    if (w <= 0 || h <= 0) return;

    // Fill colour and coastline colour are DELIBERATELY different now, and
    // both have their own hard-won reasons documented at length. Keep them
    // that way rather than collapsing back to one constant.
    const uint16_t land_c  = lv_color_to_u16(lv_color_hex(0x1E242A));
    /* Land reads LIGHTER than the sea (operator, 2026-09-12, then again
     * 2026-09-13 - the first pass only brightened the OUTLINE, and most of a
     * landmass is still several pixels of plain sea-coloured background in
     * from its coast, so "lighter land" wasn't actually visible as area).
     * The background is 0x0a0d10; land_c above is filled all the way to the
     * coast via scan_record_edge()/scan_fill_rows() below, not just stroked
     * along it.
     * ⚠ TWO CORRECTIONS ON THE SAME EVENING, from the two things that already
     * draw over this fill. 0x8FA0AD (fine for a 1px OUTLINE) read as glaring
     * daylight once it filled whole continents - operator: "I asked it to be
     * a bit lighter, not like sunlight brighter". 0x333C44 fixed that but
     * then visibly reduced the contrast the "older than 30 min" spot traces
     * depend on - they are drawn at LV_OPA_30 (map_render_spots(), the
     * age-fade design) precisely so a faded arc reads as OLD against a near-
     * black sea; a mid-brightness land background blends toward itself at
     * 30% opacity and the trace all but disappears crossing land - operator:
     * "its eating the historic traces". 0x1E242A keeps land visibly lighter
     * than the 0x0a0d10 water while staying close enough to it that the
     * traces (drawn on a layer OVER this cached image, not baked into it)
     * keep the contrast that opacity-fade needs. Screenshot before touching
     * this again - it is a three-way trade between land, sea and the traces
     * drawn over both, not a single fill colour in isolation. */
    const uint16_t coast_c = lv_color_to_u16(lv_color_hex(0x8FA0AD));
    /* The bright contour the very first version drew (before there was any
     * fill to distinguish it from) - operator, 2026-09-13, after the fill
     * landed: "the last thing missing is the white (or lighter) contour line
     * you had originally". It had not gone anywhere as a VALUE - land_c was
     * simply reused for both jobs, so once land_c was dimmed for the fill
     * (see above) the stroke dimmed right along with it and stopped reading
     * as a distinct line. Restored as its own constant, drawn in its own
     * pass (see below) so the fill can never paint over it. */
    const bool can_fill = (s_scan_x != NULL && s_scan_n != NULL);

    if (can_fill) {
        memset(s_scan_n, 0, (size_t)h * sizeof(*s_scan_n));
        walk_ring_segments(&area, w, h, cb_scan);
        scan_fill_rows(land_c);
    }
    // Stroked SECOND, always - the coastline must sit on top of the fill (or
    // be the only thing drawn, in the no-PSRAM fallback where can_fill is
    // false), never the other way round.
    s_stroke_color = coast_c;
    walk_ring_segments(&area, w, h, cb_stroke);
}

// Spots + own-QTH marker - drawn LIVE on s_map_obj (LV_EVENT_DRAW_MAIN) over
// the cached coastline, from a snapshot taken by map_spots_changed(). Keeping
// them out of the cache means a spot arrival, a filter tick or the minute's
// ageing costs a few lines, never a coastline rebuild. The snapshot means a
// frame of the drawer sliding over the map does not take three mutexes and
// copy 300 entries just to redraw a strip.
// EXT_RAM_BSS_ATTR - see the note above gather_self_spots(): ~18 KB.
static EXT_RAM_BSS_ATTR self_spot_t s_spot_snap[SELF_SPOT_MAX];
static int s_spot_snap_n = 0;

static void map_spots_changed(void)
{
    s_spot_snap_n = gather_self_spots(s_spot_snap, SELF_SPOT_MAX);
    if (s_map_obj) lv_obj_invalidate(s_map_obj);
}

static void map_render_spots(lv_layer_t *layer, const lv_area_t *area_in)
{
    lv_area_t area = *area_in;
    int32_t w = lv_area_get_width(&area);
    int32_t h = lv_area_get_height(&area);
    if (w <= 0 || h <= 0) return;

    if (!s_have_me) return;   // nothing to draw a line FROM - the sidebar/table already say so

    const self_spot_t *spots = s_spot_snap;
    int count = s_spot_snap_n;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    // 2 -> 4: the brighter MAP_SRC_COLOR_* hues (see source_color()) still
    // did not read as brighter to the operator against the land/water fill -
    // "if you cannot do them brighter then double the line thickness" - more
    // lit pixels per unit length reads as brighter even at the same colour
    // and opacity, which is the one lever left besides the hue itself.
    line_dsc.width = 4;
    line_dsc.opa = LV_OPA_80;

    lv_draw_rect_dsc_t dot_dsc;
    lv_draw_rect_dsc_init(&dot_dsc);
    dot_dsc.radius = LV_RADIUS_CIRCLE;
    dot_dsc.bg_opa = LV_OPA_COVER;

    /* ⭐ TWO DIMENSIONS, TWO CHANNELS: WHERE IT CAME FROM, AND HOW OLD IT IS.
     *
     * The operator, 2026-09-12: "in the MAP i want to be able to see all that
     * is shown in the list - coloured after where it comes from (those
     * checkboxes are already there) and the age like New: <30min Older:
     * >30min".
     *
     * So HUE stays the source - the three sidebar checkboxes are already its
     * legend - and AGE rides on OPACITY instead of on a second set of colours.
     * That ordering is deliberate: greying an old spot (the first suggestion)
     * would have cost the source, which is the thing the checkboxes name, and
     * this overlay has already been confusing once for using one palette to
     * mean two things. Dimming keeps both readable at once.
     *
     * The dot is dimmed with the line so a faded arc does not end in a
     * full-brightness point - the endpoint is the loudest mark on the map. */
    const int64_t now_u = (int64_t)time(NULL);

    for (int i = 0; i < count; i++) {
        const self_spot_t *sp = &spots[i];
        if (!sp->has_pos || !passes_filter(sp)) continue;

        bool old = sp->heard_unix > 0 && (now_u - sp->heard_unix) > SELF_SPOT_NEW_SEC;
        if (old && !s_show_older) continue;

        line_dsc.opa   = old ? LV_OPA_30 : LV_OPA_80;
        dot_dsc.bg_opa = old ? LV_OPA_40 : LV_OPA_COVER;

        line_dsc.color = lv_color_hex(source_color(sp->src));
        draw_great_circle(layer, &line_dsc, &area, w, h, (float)s_my_lon, (float)s_my_lat, sp->lon, sp->lat);

        lv_point_precise_t p = project(&area, w, h, sp->lon, sp->lat);
        dot_dsc.bg_color = lv_color_hex(source_color(sp->src));
        int r = old ? 3 : 5;   // landing dimple, bumped a tad to match the doubled line width above
        lv_area_t dot_area = { p.x - r, p.y - r, p.x + r, p.y + r };
        lv_draw_rect(layer, &dot_dsc, &dot_area);
    }

    /* Restore, or the home dot below inherits whatever the last spot set. */
    dot_dsc.bg_opa = LV_OPA_COVER;

    // Own position, drawn last so it always sits on top of every line.
    lv_point_precise_t home = project(&area, w, h, (float)s_my_lon, (float)s_my_lat);
    dot_dsc.bg_color = lv_color_hex(UI_COLOR_ACCENT_GOLD);
    lv_area_t home_area = { home.x - 5, home.y - 5, home.x + 5, home.y + 5 };
    lv_draw_rect(layer, &dot_dsc, &home_area);
}

/* ⭐⭐ THE WHOLE MAP IS ONE CACHED IMAGE NOW, 2026-09-13.
 *
 * Both layers used to be LV_EVENT_DRAW_MAIN callbacks, and LVGL redraws every
 * object under ANY dirty area - so each frame of the settings drawer sliding
 * over the map, and each frame of its breathing grip, re-walked the whole
 * coastline (thousands of lv_draw_line calls) and re-gathered all 300 spots
 * under three mutexes. That is what made the drawer lag, and why the breathing
 * grip got the blame for slowing the page.
 *
 * Now the map is rendered ONCE into a PSRAM RGB565 canvas, and only when
 * something the picture depends on changed (zoom, pan, spots, filters, own
 * grid). Everything that moves over it costs a copy of the pixels underneath.
 * Requests are coalesced by a 33 ms timer, so a drag or pinch that fires many
 * events per frame still renders at most once per frame - the same cost the
 * old per-frame redraw had during a drag, and nothing at all otherwise.
 *
 * Sized from s_map_obj's own coords (1140x656 at this layout, ~1.5 MB), so
 * project() gives identical results against the canvas-local area. */
static bool        s_map_dirty = true;
static lv_timer_t *s_map_cache_timer = NULL;

static void map_mark_dirty(void) { s_map_dirty = true; }

static void map_spots_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int64_t t0 = esp_timer_get_time();
    map_render_spots(lv_event_get_layer(e), &area);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (ms >= 20) ESP_LOGI(TAG, "spot layer draw %d ms (%d spots)", ms, s_spot_snap_n);
}

// Container size the cache was last (re)built for - separate from
// s_map_cache_w/h, which is the ACTUAL buffer resolution and can be smaller
// (see below). Comparing against this, not the buffer size, is what stops a
// reduced-resolution cache from being torn down and retried every single 33
// ms tick just because its own dimensions differ from the container's.
static int32_t s_map_cache_req_w = 0, s_map_cache_req_h = 0;
// Set after a failed allocation attempt; map_cache_rebuild() skips retrying
// until this passes. heap_caps_aligned_alloc() failing is cheap on its own,
// but without a backoff a persistently-tight PSRAM budget (WSPR's capture/
// decode buffers alone hold ~11 MB for as long as that page is open - see
// wspr_rx.c) turns this into a 30 Hz allocation-and-log storm for as long as
// the overlay stays open, for no benefit - the budget does not change tick
// to tick.
static int64_t s_map_cache_retry_after_us = 0;

static void map_cache_rebuild(void)
{
    if (!s_map_bg_obj || !s_map_obj) return;
    lv_area_t a;
    lv_obj_get_coords(s_map_obj, &a);
    int32_t w = lv_area_get_width(&a);
    int32_t h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) return;

    if (w != s_map_cache_req_w || h != s_map_cache_req_h || !s_map_cache_buf) {
        if (!s_map_cache_buf && w == s_map_cache_req_w && h == s_map_cache_req_h
            && esp_timer_get_time() < s_map_cache_retry_after_us) {
            return;   // same size as the last failure, still backed off
        }
        s_map_cache_req_w = w;
        s_map_cache_req_h = h;
        if (s_map_cache_buf) { heap_caps_free(s_map_cache_buf); s_map_cache_buf = NULL; }

        /* PSRAM on this board is a genuinely tight shared resource - WSPR
         * alone holds ~11 MB of capture/decode buffers (wspr_rx.c) for the
         * whole time that page is open, and this cache is ~1.5 MB that needs
         * ONE contiguous block. Total free can look ample (multiple MB) while
         * nothing that large is actually free, so a plain alloc-and-fail
         * leaves the map permanently blank even though there was "enough"
         * PSRAM by the free-byte count. Measured 2026-09-15: 3.4 MB total
         * free, every 1140x656 (~1.46 MB) allocation still failing.
         *
         * heap_caps_get_largest_free_block() is the on-demand path here -
         * only on an actual container resize or a fresh open, never on the
         * steady 33 ms redraw (that always takes the req_w/req_h match above
         * and returns before reaching this). Same discipline CLAUDE.md
         * documents for MALLOC_CAP_DMA: this walks the heap with interrupts
         * off and must never run on a genuinely periodic path. */
        size_t budget = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t margin = 64 * 1024;   // leave room for the land-fill scratch + alignment slop
        budget = (budget > margin) ? budget - margin : 0;

        int32_t rw = w, rh = h;
        int f = 1;
        for (; f <= 4; f++) {
            rw = w / f; rh = h / f;
            size_t need = (size_t)lv_draw_buf_width_to_stride(rw, LV_COLOR_FORMAT_RGB565) * (size_t)rh;
            if (need <= budget) break;
        }
        if (f > 4) {
            ESP_LOGE(TAG, "no PSRAM for even a quarter-size map cache (largest free block %d B)", (int)budget);
            s_map_cache_w = s_map_cache_h = 0;
            s_map_cache_retry_after_us = esp_timer_get_time() + 3000000;   // 3 s
            return;
        }

        size_t sz = (size_t)lv_draw_buf_width_to_stride(rw, LV_COLOR_FORMAT_RGB565) * (size_t)rh;
        s_map_cache_buf = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_map_cache_buf) {
            ESP_LOGE(TAG, "no PSRAM for the %dx%d map cache (tried %dx%d, largest free block %d B)",
                     (int)w, (int)h, (int)rw, (int)rh, (int)budget);
            s_map_cache_w = s_map_cache_h = 0;
            s_map_cache_retry_after_us = esp_timer_get_time() + 3000000;   // 3 s
            return;
        }
        if (f > 1) {
            ESP_LOGW(TAG, "map cache reduced to %dx%d (1/%d) - PSRAM is tight (largest free block %d B)",
                     (int)rw, (int)rh, f, (int)budget);
        }
        s_map_cache_w = rw;
        s_map_cache_h = rh;
        s_map_cache_stride = (int32_t)lv_draw_buf_width_to_stride(rw, LV_COLOR_FORMAT_RGB565);
        lv_canvas_set_buffer(s_map_bg_obj, s_map_cache_buf, rw, rh, LV_COLOR_FORMAT_RGB565);
        // Stretch the (possibly smaller) buffer to fill the same on-screen
        // area it always has. The touch/projection layer (s_map_obj) is a
        // SEPARATE object sized to the full container and never shrinks, so
        // this is a purely visual scale-up - project()/touch math is
        // untouched. Pivot at the origin so the scale expands from the same
        // (0,0) corner the canvas is positioned at, not its center.
        lv_image_set_pivot(s_map_bg_obj, 0, 0);
        lv_image_set_scale(s_map_bg_obj, (f == 1) ? LV_SCALE_NONE : (uint32_t)(256 * f));

        // Land-fill scanline scratch, sized to the ACTUAL buffer height (see
        // the comment on s_scan_x above). Resized alongside the cache
        // buffer; a failed alloc here is NOT fatal - map_render_coast()
        // falls back to outline-only (can_fill == false), same defensive
        // shape as the cache buffer above failing, just a worse-looking map
        // rather than no map.
        if (s_scan_x) { heap_caps_free(s_scan_x); s_scan_x = NULL; }
        if (s_scan_n) { heap_caps_free(s_scan_n); s_scan_n = NULL; }
        s_scan_x = heap_caps_malloc((size_t)rh * MAP_SCAN_MAX_X * sizeof(*s_scan_x), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_scan_n = heap_caps_malloc((size_t)rh * sizeof(*s_scan_n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scan_x || !s_scan_n) {
            ESP_LOGW(TAG, "no PSRAM for the map land-fill scratch (%d rows) - outline only", (int)rh);
            if (s_scan_x) { heap_caps_free(s_scan_x); s_scan_x = NULL; }
            if (s_scan_n) { heap_caps_free(s_scan_n); s_scan_n = NULL; }
        }
    }

    int64_t t0 = esp_timer_get_time();
    lv_canvas_fill_bg(s_map_bg_obj, lv_color_hex(0x0a0d10), LV_OPA_COVER);
    lv_area_t local = { 0, 0, s_map_cache_w - 1, s_map_cache_h - 1 };
    map_render_coast(&local);
    lv_obj_invalidate(s_map_bg_obj);   // the buffer was written behind LVGL's back
    s_map_dirty = false;
    // Temporary-but-harmless: rare (zoom/pan only) and the number that
    // decides whether this approach is fast enough. Drag logs at most ~1/s.
    static int64_t s_last_log_us;
    int64_t now = esp_timer_get_time();
    if (now - s_last_log_us > 1000000) {
        s_last_log_us = now;
        ESP_LOGI(TAG, "coastline rebuilt in %d ms", (int)((now - t0) / 1000));
    }
}

static void map_cache_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_active && s_map_dirty) map_cache_rebuild();
}

// Raw multi-touch poll for two-finger pinch-zoom, same technique ui.c's own
// pinch_poll_cb() uses for the panadapter spectrum (esp_lcd_touch_read_data()
// bypasses LVGL's single-point indev to see both fingers). Only the SPREAD
// between the two touch points is used, never their absolute position, which
// is what makes this immune to the raw-panel-vs-landscape rotation mess
// ui.c's own comments document at length: a 90 degree rotation is an
// isometry, so the Euclidean distance between two raw panel coordinates
// equals the distance between their landscape-rotated counterparts - no
// per-orientation transform needed here at all, flipped display included.
static bool             s_map_pinch_active = false;
static int              s_map_pinch_start_dist = 0;
static float            s_map_pinch_start_zoom = 1.0f;
static esp_lcd_touch_handle_t s_map_touch = NULL;

static void map_pinch_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active || !s_map_touch) return;
    // Only while the MAP tab is actually the one on screen - pinching on
    // LIST/CONDITIONS would otherwise silently zoom a map nobody is looking
    // at (harmless, but confusing the next time MAP is opened).
    if (!s_tabview || lv_tabview_get_tab_active(s_tabview) != 0) {
        s_map_pinch_active = false;
        return;
    }

    esp_lcd_touch_read_data(s_map_touch);
    uint8_t npts = s_map_touch->data.points;
    if (npts < 2) {
        s_map_pinch_active = false;
        return;
    }

    int dx = (int)s_map_touch->data.coords[0].x - (int)s_map_touch->data.coords[1].x;
    int dy = (int)s_map_touch->data.coords[0].y - (int)s_map_touch->data.coords[1].y;
    int dist = (int)sqrtf((float)(dx * dx + dy * dy));
    if (dist < 8) dist = 8;   // floor, same reasoning as ui.c's own pinch dead zone

    if (!s_map_pinch_active) {
        s_map_pinch_active = true;
        s_map_pinch_start_dist = dist;
        s_map_pinch_start_zoom = s_map_zoom;
        /* ⭐ WHERE THE ZOOM IS ANCHORED, captured ONCE when the pinch starts.
         *
         * This used to change s_map_zoom and nothing else, so the zoom was
         * anchored on the STATION (project()'s ax/ay) and everything else flew
         * away from it. Operator, 2026-09-18: "it does not stay centred where i
         * start pinching - so when zooming in i move fast to another place in
         * the region and need to zoom out again to orient my self".
         *
         * ⚠ He asked whether the one-finger pan was fighting it. It is not -
         * map_drag_cb() returns while s_map_pinch_active, and has since the
         * gesture was written. The pan was simply never updated to match.
         *
         * ⛔ TAKEN FROM LVGL, NOT FROM THE RAW TOUCH DRIVER. The pinch reads
         * esp_lcd_touch directly because LVGL tracks only one point, and those
         * raw coordinates are in the PANEL's portrait frame - ui.c's own pinch
         * uses `coords[].y` as a landscape x for exactly that reason. Deriving
         * the full rotation here would be a second copy of that transform, and
         * a wrong one would be invisible: the map would simply drift the wrong
         * way. LVGL's tracked point is already in screen coordinates, which is
         * what map_drag_cb() uses two functions down, so the two gestures agree
         * by construction. It is one finger of the two rather than their
         * midpoint - a few tens of pixels out, against a whole screen of drift
         * before. */
        s_map_pinch_fx = s_map_pinch_fy = 0.5f;   /* centre if nothing better */
        if (s_map_have_last_pt && s_map_obj) {
            lv_area_t a;
            lv_obj_get_coords(s_map_obj, &a);
            int32_t aw = lv_area_get_width(&a), ah = lv_area_get_height(&a);
            if (aw > 0 && ah > 0) {
                float fx = (float)(s_map_last_pt.x - a.x1) / (float)aw;
                float fy = (float)(s_map_last_pt.y - a.y1) / (float)ah;
                if (fx >= 0.0f && fx <= 1.0f && fy >= 0.0f && fy <= 1.0f) {
                    s_map_pinch_fx = fx;
                    s_map_pinch_fy = fy;
                }
            }
        }
        /* ⚠ KEPT, at DEBUG. This is the one line that distinguishes "the anchor
         * is wrong" from "the anchor never arrived", and the difference is
         * invisible on screen - the first version of this fix took the centre
         * fallback on every pinch and looked exactly like no fix at all. It is
         * DEBUG rather than INFO so it does not fill the diag log, and rather
         * than deleted because the next person to touch this gesture will want
         * it. Raise it to ESP_LOGI for one build if the map ever drifts again. */
        ESP_LOGD(TAG, "pinch anchor: %.2f,%.2f (%s)", (double)s_map_pinch_fx,
                 (double)s_map_pinch_fy, s_map_have_last_pt ? "finger" : "CENTRE FALLBACK");
        return;
    }

    float zoom = s_map_pinch_start_zoom * ((float)dist / (float)s_map_pinch_start_dist);
    if (zoom < MAP_ZOOM_MIN) zoom = MAP_ZOOM_MIN;
    if (zoom > MAP_ZOOM_MAX) zoom = MAP_ZOOM_MAX;
    if (fabsf(zoom - s_map_zoom) > 0.01f) {
        /* Hold the point under the fingers still. project() maps a world
         * fraction w to the screen as
         *      f = a + (w - a) * zoom + pan
         * so for the screen point f to name the same w after the zoom changes:
         *      w - a = (f - a - pan0) / z0
         *      pan1  = f - a - (f - a - pan0) * z1 / z0
         *
         * ⚠ z0 is read as 1.0 below MAP_ZOOM_MIN because project() skips the
         * whole transform at zoom 1 - treating the unzoomed view as
         * (zoom 1, pan 0) is what makes the first pinch out of it land right
         * rather than jumping. */
        float z0 = (s_map_zoom > 1.0f) ? s_map_zoom : 1.0f;
        float p0x = (s_map_zoom > 1.0f) ? s_map_pan_dx : 0.0f;
        float p0y = (s_map_zoom > 1.0f) ? s_map_pan_dy : 0.0f;
        float ax = ((s_have_me ? (float)s_my_lon : 0.0f) + 180.0f) / 360.0f;
        float ay = (90.0f - (s_have_me ? (float)s_my_lat : 0.0f)) / 180.0f;
        float r  = zoom / z0;

        s_map_pan_dx = s_map_pinch_fx - ax - (s_map_pinch_fx - ax - p0x) * r;
        s_map_pan_dy = s_map_pinch_fy - ay - (s_map_pinch_fy - ay - p0y) * r;

        s_map_zoom = zoom;
        s_view_is_users = true;    /* stop auto-re-fitting under their fingers */
        map_sync_scroll_chain();   /* pan, or swipe-to-tab - see its comment */
    }
}

// One-finger drag-pan, plain LVGL events this time (not a raw touch poll) -
// a single touch is exactly what LVGL's own indev already tracks correctly,
// unlike the two simultaneous points the pinch above needs. Only active
// while zoomed in (panning the unzoomed view, which already shows the whole
// world, could only ever reveal blank margin) and never while a pinch is
// in progress - s_map_pinch_active is checked every call so drag tracking
// cleanly resumes, re-baselined, the moment a finger lifts back to one.
static bool  s_map_drag_active = false;
static lv_point_t s_map_drag_start_pt;
static float s_map_drag_start_pan_dx = 0.0f, s_map_drag_start_pan_dy = 0.0f;

/* ⛔ WHILE ZOOMED, A SIDEWAYS DRAG MUST PAN - NOT CHANGE TAB.
 *
 * The map sits in a page of an lv_tabview, and a tabview changes tab by
 * scrolling its content horizontally. s_map_obj is not itself scrollable, so a
 * press on it CHAINS up to that content and the swipe went to MAP/LIST/
 * CONDITIONS instead of to the pan this file already implements. The operator:
 * "if you have zoomed map then how to you move it around on the screen when
 * dragging left or right changes between map list conditions?" (2026-09-12) -
 * i.e. the pan was unreachable in the one state where it does anything.
 *
 * Clearing LV_OBJ_FLAG_SCROLL_CHAIN_HOR is the same remedy the drawer's
 * sliders and the FT8 filter checkboxes already use vertically: stop the
 * gesture propagating to a scrolling ancestor that would swallow it.
 *
 * ⚠ Toggled WITH THE ZOOM rather than cleared once, so the swipe-between-tabs
 * gesture is only given up while it is actually competing with something. At
 * zoom 1 there is no pan to make - project() ignores it - so the swipe keeps
 * working exactly as before. While zoomed, the tab bar at the top is the way
 * to change tab, which is a button rather than a gesture and cannot conflict. */
static void map_sync_scroll_chain(void)
{
    if (!s_map_obj) return;
    if (s_map_zoom > MAP_ZOOM_MIN) lv_obj_clear_flag(s_map_obj, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    else                           lv_obj_add_flag  (s_map_obj, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    // Every caller of this function just changed zoom and/or pan (fit, pinch,
    // the Zoom dropdown). See map_cache_rebuild().
    map_mark_dirty();
}

static void map_drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_map_drag_active = false;
        s_map_have_last_pt = false;   /* never anchor the next pinch on an old touch */
        return;
    }
    if (code != LV_EVENT_PRESSING) return;

    /* ⛔ READ THE POINT BEFORE ANY EARLY RETURN - THE PINCH NEEDS IT.
     *
     * map_pinch_poll_cb() runs from an lv_timer, and lv_indev_get_act() is only
     * valid INSIDE an event callback, so from there it is always NULL. My first
     * attempt at anchoring the pinch asked for it anyway, silently fell back to
     * the screen centre every single time, and the operator reported exactly
     * that: "its still zooming around the center of the screen".
     *
     * Here the indev is the event's own and is always valid. Storing it on
     * every PRESSING - including the ones the drag itself ignores, which is why
     * this sits above the returns below - means that when a second finger lands
     * the pinch already knows where the first one is. */
    {
        lv_indev_t *iv = lv_event_get_indev(e);
        if (iv) {
            lv_indev_get_point(iv, &s_map_last_pt);
            s_map_have_last_pt = true;
        }
    }

    if (s_map_zoom <= MAP_ZOOM_MIN || s_map_pinch_active) {
        s_map_drag_active = false;   // re-baseline once dragging is valid again
        return;
    }

    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (!s_map_drag_active) {
        s_map_drag_active = true;
        s_map_drag_start_pt = p;
        s_map_drag_start_pan_dx = s_map_pan_dx;
        s_map_drag_start_pan_dy = s_map_pan_dy;
        return;
    }

    lv_area_t area;
    lv_obj_get_coords(s_map_obj, &area);
    int32_t w = lv_area_get_width(&area);
    int32_t h = lv_area_get_height(&area);
    if (w <= 0 || h <= 0) return;

    s_view_is_users = true;    /* same as the pinch: this view is theirs now */
    s_map_pan_dx = s_map_drag_start_pan_dx + (float)(p.x - s_map_drag_start_pt.x) / (float)w;
    s_map_pan_dy = s_map_drag_start_pan_dy + (float)(p.y - s_map_drag_start_pt.y) / (float)h;
    map_mark_dirty();   // coalesced to one render per frame - see map_cache_rebuild()
}

// ---- Tabelle tab --------------------------------------------------------

#define COL_GAP 10
/* How many characters the Country column can show before country_display()
 * gives up and returns the 3-letter code instead. The columns are flex-grow,
 * so this is a judgement about the RENDERED width rather than a derivation -
 * if a common country starts showing as a code, this is the number to raise
 * (and something else must give a grow unit back). */
/* The LIST tab is the widest place a country name is shown - 1280 px with only
 * a 140 px tab bar beside it - so it gets the generous limit. The FT8 and WSPR
 * decode lists are far tighter and pass their own, smaller number. */
#define LIST_COUNTRY_CHARS 18
/* ⛔ THE SAME UNBOUNDED-COST BUG THE MAP HAD, NOW FOUND IN THE LIST.
 *
 * This used to be SELF_SPOT_MAX (300) - "the buffer is the limit, nothing
 * clips it further" - on the premise that lv_obj_clean()+rebuild is cheap.
 * It is not, at this count: a real self-spot table with ~50-100 live entries
 * (one WSPR poll session on this bench, nowhere near the 300 cap) produced
 * `idle0 0.1%` `idle1 0.0%` `fps 0.6` in the capture the moment the LIST tab
 * was opened - both cores saturated, not a slow frame. Operator, 2026-09-13:
 * "pressed list and it almost froze". Same root cause as the map's freeze a
 * day earlier (LVGL draw/layout cost scaling with an unbounded row count on
 * a board where core 0 is already the wall) and the same class of fix: bound
 * the WORK, do not just relocate when it is paid - my first attempt at this
 * (forcing an early lv_obj_update_layout() call) targeted LAYOUT, and this
 * measurement shows the real cost is in DRAW/scroll compositing when the tab
 * becomes visible, which that call does nothing for. Retracted.
 *
 * 40 is a real cap now, not "the buffer size happens to allow it" - it bounds
 * object count to 40 rows x 7 columns = 280, a small fraction of the ~700
 * that produced the freeze, while still being a genuinely useful scrollable
 * list. rebuild_table() appends a "N more, not shown" row rather than
 * silently dropping them - a truncation nobody can see is worse than a
 * shorter list that says so. */
#define TABLE_MAX_ROWS 40

static lv_obj_t *make_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_pad_column(row, COL_GAP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

static void add_col(lv_obj_t *row, const char *text, int grow, uint32_t color, bool bold, bool align_right)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, 0);
    lv_obj_set_flex_grow(lbl, grow);
    /* 22/24, not 18/20. This project settled long ago that 18 is below what is
     * readable on this screen at arm's length - wspr_screen_view.c carries the
     * same note beside its own wsprnet line, where 18 had crept in too. The
     * widest cell here is a callsign like "F/SWL/PRIVAS" at grow 2, which is
     * ~144 px of montserrat_24 in a ~230 px column, so the columns still fit. */
    lv_obj_set_style_text_font(lbl, bold ? &lv_font_montserrat_24 : &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    // SNR/Distance are numeric and right-aligned (2026-09-14) so their digits
    // line up column-wise instead of ragging left like the text columns.
    if (align_right) lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
}

// LIST tab column sort: tap a header to cycle unsorted -> ascending ->
// descending -> unsorted for that column; tapping a DIFFERENT column while
// one is active starts that one fresh at ascending, matching the common
// spreadsheet/file-manager convention rather than remembering a per-column
// direction.
typedef enum {
    SORT_COL_NONE = 0,
    SORT_COL_CALL, SORT_COL_GRID, SORT_COL_MODE, SORT_COL_BAND, SORT_COL_ISO,
    SORT_COL_FREQ, SORT_COL_SNR, SORT_COL_DIST, SORT_COL_AGE,
} sort_col_t;
typedef enum { SORT_ASC, SORT_DESC } sort_dir_t;

static sort_col_t s_sort_col = SORT_COL_NONE;
static sort_dir_t s_sort_dir = SORT_ASC;

// qsort has no user-data parameter, so this reads s_sort_col/s_sort_dir
// directly - same pattern the rest of this file already uses for filter
// state (s_show_cw etc.).
static int cmp_spots(const void *pa, const void *pb)
{
    const self_spot_t *a = (const self_spot_t *)pa;
    const self_spot_t *b = (const self_spot_t *)pb;

    // Distance is the one column with a real "no value" case (either end's
    // position unknown, -1). Unknown always sorts to the bottom, in EITHER
    // direction - flipping it to the top on descending would read as "these
    // are the furthest", which is backwards for a value that isn't there.
    if (s_sort_col == SORT_COL_DIST) {
        bool va = a->distance_km >= 0, vb = b->distance_km >= 0;
        if (va != vb) return va ? -1 : 1;
        if (!va) return 0;
    }

    int cmp;
    switch (s_sort_col) {
    case SORT_COL_CALL: cmp = strcasecmp(a->call, b->call); break;
    case SORT_COL_GRID: cmp = strcasecmp(a->grid, b->grid); break;
    case SORT_COL_MODE: cmp = strcasecmp(a->mode, b->mode); break;
    // Band has no numeric value of its own (it's a name derived from
    // frequency, adif_log_band_for_freq()) - sorting on the underlying
    // frequency gives the natural band order for free and needs no second
    // band-name-to-rank table to maintain.
    case SORT_COL_BAND:
    case SORT_COL_FREQ: cmp = (a->freq_hz    > b->freq_hz)    - (a->freq_hz    < b->freq_hz); break;
    // ISO is derived from the callsign's prefix (geo_coords_iso_for_call()),
    // same as the column's own render below - not stored on self_spot_t, so
    // it is looked up here rather than compared as a field.
    case SORT_COL_ISO: {
        /* Sorted on what is SHOWN, so the order matches the column the operator
         * is reading - country_display() spells the name out where it fits and
         * falls back to the 3-letter code where it does not, and sorting on the
         * underlying ISO would have put "Spain" and "ESP" in different places. */
        const char *ia = country_display(a->call, LIST_COUNTRY_CHARS);
        const char *ib = country_display(b->call, LIST_COUNTRY_CHARS);
        if (!ia || !ib) { cmp = (!ia == !ib) ? 0 : (ia ? -1 : 1); break; }
        cmp = strcmp(ia, ib);
        break;
    }
    case SORT_COL_SNR:  cmp = (a->snr_db     > b->snr_db)     - (a->snr_db     < b->snr_db); break;
    case SORT_COL_DIST: cmp = (a->distance_km > b->distance_km) - (a->distance_km < b->distance_km); break;
    // "Ascending age" means smallest age (most recent) first, i.e. LARGEST
    // heard_unix first - comparing b against a here, not a against b, is
    // what makes plain ascending/descending below read correctly as
    // "youngest first" / "oldest first" without a separate special case.
    case SORT_COL_AGE:  cmp = (b->heard_unix > a->heard_unix) - (b->heard_unix < a->heard_unix); break;
    default: return 0;
    }
    return (s_sort_dir == SORT_DESC) ? -cmp : cmp;
}

static void rebuild_table(void);   // fwd - header_click_cb() re-renders on every sort change

static void header_click_cb(lv_event_t *e)
{
    sort_col_t col = (sort_col_t)(intptr_t)lv_event_get_user_data(e);
    if (s_sort_col != col)       { s_sort_col = col;          s_sort_dir = SORT_ASC; }
    else if (s_sort_dir == SORT_ASC) { s_sort_dir = SORT_DESC; }
    else                          { s_sort_col = SORT_COL_NONE; }   // third tap: back to unsorted
    rebuild_table();
}

// A clickable header cell - occupies the same flex_grow slot add_col()'s
// label would, so column boundaries stay pixel-aligned with the data rows
// below, but wraps the label in its own lv_obj so it can be tapped
// independently of the (deliberately non-clickable) header row itself.
// Appends an up/down glyph when this is the active sort column.
static void add_sort_header_col(lv_obj_t *row, const char *text, int grow, sort_col_t col_id, bool align_right)
{
    lv_obj_t *cell = lv_obj_create(row);
    lv_obj_remove_style_all(cell);
    lv_obj_set_width(cell, 0);
    lv_obj_set_height(cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(cell, grow);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(cell, 8);
    lv_obj_set_style_bg_color(cell, lv_color_hex(UI_COLOR_PRIMARY), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cell, LV_OPA_30, LV_STATE_PRESSED);

    bool active = (s_sort_col == col_id);
    char buf[24];
    if (active) snprintf(buf, sizeof(buf), "%s %s", text, s_sort_dir == SORT_ASC ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    else        snprintf(buf, sizeof(buf), "%s", text);

    lv_obj_t *lbl = lv_label_create(cell);
    lv_label_set_text(lbl, buf);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    /* Matches the rows it labels (22 normal / 24 bold) rather than sitting a
     * size below them - the operator, 2026-09-12: "please bump up headers as
     * well it all needs to match". 24 so a header still reads as a header. */
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(active ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED), 0);
    // Matches add_col()'s own SNR/Distance right-align - the label needs a
    // real width (the cell's own, not its content size) before "align right
    // within it" means anything.
    if (align_right) {
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_add_event_cb(cell, header_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)col_id);
}

static void rebuild_table(void)
{
    if (!s_table_list) return;
    /* ⚠ RETRACTED THEORY, KEPT AS A RECORD OF WHAT WAS WRONG.
     *
     * This comment used to say the LIST tap was slow because lv_tabview keeps
     * a never-shown tab HIDDEN and defers its layout - and that forcing
     * lv_obj_update_layout() here (still below) would fix it. Both halves
     * were wrong: lv_tabview_set_active() (lv_tabview.c) switches tabs by
     * SCROLLING its content container, not by a hidden-flag toggle - every
     * tab page exists, laid out, at all times, so there was never a deferred
     * layout to force.
     *
     * The real cost, confirmed from the capture after the "fix" shipped and
     * the freeze recurred (`idle0 0.1% idle1 0.0% fps 0.6`, both cores
     * saturated, not a slow frame): building up to SELF_SPOT_MAX (300) rows
     * of 7 flex children each is expensive to LAY OUT AND DRAW regardless of
     * when that happens, and a real self-spot table on this bench (~50-100
     * entries, nowhere near 300) was already enough to do it. TABLE_MAX_ROWS
     * is now a real cap (40, see its own comment) - the fix that actually
     * bounds the work, rather than moving an unbounded cost from one moment
     * to another. lv_obj_update_layout() below is kept as ordinary hygiene
     * (this function already changed the content, so paying for layout here
     * rather than on whatever runs next is still reasonable) - it is no
     * longer claimed to be what fixed anything. */
    lv_obj_clean(s_table_list);

    lv_obj_t *hdr = make_row(s_table_list);
    // Column weights doubled from the original 2/1/1/2/1/1/1/1 (2026-09-14) so
    // RX/Freq could shrink and Distance grow by whole-number steps and still
    // land on the same total (20 vs 10) - see add_col()'s own comment for why
    // SNR/Distance also right-align.
/* ⭐ CAPITALS AND ONE COMMON ORDER ACROSS ALL THREE LISTS (operator,
     * 2026-09-19). FT8, this list and WSPR now read left to right as
     * WHO - WHERE - WHAT - HOW WELL - HOW FAR - HOW LONG AGO, so the eye
     * lands in the same place on every screen:
     *   FT8   CALL MESSAGE COUNTRY SNR TONE DT KM AGE
     *   LIST  RECEIVER GRID COUNTRY MODE BAND FREQUENCY SNR KM AGE
     *   WSPR  S UTC CALL GRID COUNTRY BND PWR SNR TONE DR DT KM
     *
     * ⚠ TONE AND DT ARE NOT HERE, AND THAT IS THE DATA'S FAULT, NOT AN
     * OVERSIGHT. The operator asked for both. No feed reports the audio tone
     * we were heard on - PSK Reporter and RBN give a dial frequency, wsprnet
     * gives ours as they measured it, none gives an offset within a passband -
     * and DT exists only in the wsprnet scrape, i.e. one source of three. A
     * column that is a dash on two thirds of the rows is worse than no column,
     * and inventing either would be the "never fabricate a measurement" rule
     * again. Add them the day a feed actually carries them.
     *
     * Weights total 24 (was 21): GRID takes 3 and RECEIVER gives up one, since
     * "RECEIVER" is a wider heading than "RX" but the CALLSIGNS under it did
     * not change length. */
/* ⚠ WEIGHTS DOUBLED so the columns can be tuned in HALF steps. At the old
     * 2/3/4 granularity the smallest change was ~4 % of the table width, which
     * is why GRID ended up a character short of a 6-character locator and
     * FREQUENCY clipped (operator, 2026-09-19: "GRID and FREQUENCY columns are
     * not wide enough - look at last line in GRID").
     *
     * What the values have to hold, which is what these are sized from:
     *   RECEIVER  a callsign, up to ~10 with a portable suffix
     *   GRID      SIX characters - a 6-char locator is normal, not an edge case
     *   FREQUENCY "14.095.600" - ten characters in the dotted style
     *   SNR       "-19"   KM "12.345"   AGE "5m"
     * SNR, KM and AGE are the three narrowest things in the table and were
     * each as wide as GRID; they give up the room. */
    add_sort_header_col(hdr, "RECEIVER",  8, SORT_COL_CALL,  false);
    add_sort_header_col(hdr, "GRID",      6, SORT_COL_GRID,  false);
    /* Country BEFORE Distance - operator, 2026-09-19. Reads better: the country
     * names it, the distance qualifies it, and the two numeric columns (Freq,
     * SNR) no longer have a text column wedged between them and Distance.
     *
     * LEFT-aligned, like its values. */
    add_sort_header_col(hdr, "COUNTRY",   6, SORT_COL_ISO,   false);
    add_sort_header_col(hdr, "MODE",      4, SORT_COL_MODE,  true);
    add_sort_header_col(hdr, "BAND",      4, SORT_COL_BAND,  true);
    add_sort_header_col(hdr, "FREQUENCY",10, SORT_COL_FREQ,  true);
    add_sort_header_col(hdr, "SNR",       3, SORT_COL_SNR,   true);
    /* The UNIT lives in the heading now, not on every row - the same trade the
     * FT8 and WSPR lists already make. */
    add_sort_header_col(hdr, settings_get_distance_in_miles() ? "MI" : "KM",
                                          4, SORT_COL_DIST,  true);
    add_sort_header_col(hdr, "AGE",       3, SORT_COL_AGE,   true);

    static EXT_RAM_BSS_ATTR self_spot_t spots[SELF_SPOT_MAX];   // NOT internal .bss - see the note above map_draw_cb()'s copy of this array
    int count = gather_self_spots(spots, SELF_SPOT_MAX);
    if (s_sort_col != SORT_COL_NONE) qsort(spots, (size_t)count, sizeof(spots[0]), cmp_spots);
    int64_t now = (int64_t)time(NULL);

    int shown = 0, passed = 0;
    for (int i = 0; i < count; i++) {
        const self_spot_t *sp = &spots[i];
        if (!passes_filter(sp)) continue;
        passed++;
        if (shown >= TABLE_MAX_ROWS) continue;   /* keep counting `passed` for the notice below */

        char freq_buf[16], age_buf[24], snr_buf[8], dist_buf[24];
        format_freq_hz(sp->freq_hz, g_freq_style, freq_buf, sizeof(freq_buf));
        format_age(sp->heard_unix, now, age_buf, sizeof(age_buf));
        snprintf(snr_buf, sizeof(snr_buf), "%d", sp->snr_db);
        if (sp->distance_km >= 0) {
            // Randy N4OPI, 2026-09-16: the FT8 decode list already honours
            // distance_in_miles (ft8_screen_view.c) - this table never did,
            // and always showed km regardless of the setting. Same conversion
            // as that screen (km * 0.621371), formatted with the same
            // thousands-dotted style either way.
            char dist_dotted[16];
            bool mi = settings_get_distance_in_miles();
            long shown = mi ? (long)(sp->distance_km * 0.621371 + 0.5) : (long)sp->distance_km;
            format_km_dotted(shown, dist_dotted, sizeof(dist_dotted));
            snprintf(dist_buf, sizeof(dist_buf), "%s", dist_dotted);
        } else {
            snprintf(dist_buf, sizeof(dist_buf), "-");
        }
        // The ONE band table (adif_log_band_for_freq(), adif_log.c) - do not
        // reimplement this locally, see that function's own comment.
        const char *band = adif_log_band_for_freq(sp->freq_hz);
        /* ⛔ COUNTRY, NOT ISO. This column showed a 3-letter code while the FT8
         * and WSPR lists spell the name out - two answers to the same question
         * in one firmware. Operator, 2026-09-19: "In LIST tap we have ISO....
         * in all other pages we have COUNTRY". country_display() is the single
         * rule those pages already use: the name where it fits, the 3-letter
         * code where it does not, and NEVER a name chopped in half. */
        const char *iso = country_display(sp->call, LIST_COUNTRY_CHARS);

        lv_obj_t *row = make_row(s_table_list);
        /* ⛔ THE MODE COLUMN IS COLOURED BY THE MODE, NOT BY THE SOURCE.
         *
         * Both used source_color(), which returns UI_COLOR_MODE_CW/_DIGI/_WSPR
         * - the palette the bandplan and the FT8 screen use to mean MODE. So
         * the mode text was painted in a mode colour that did not describe it,
         * and the operator read the two as one scheme: "the Source coloures
         * (CW Digi WSPR) on the map is mixed up with the coloures in the list:
         * same coloures bit different meaning" (2026-09-12).
         *
         * They genuinely disagree: PSK Reporter carries CW spots, so such a
         * station is SPOT_SRC_DIGI (amber) while its mode says CW, which is
         * blue everywhere else in this firmware.
         *
         * So the mode now goes through ui_theme_mode_color(), the one rule the
         * rest of the app uses. The CALLSIGN keeps the source colour, which is
         * what the sidebar's three checkboxes are the legend for - one column
         * per meaning, and no colour describing something it is not. */
        uint32_t col = source_color(sp->src);
        add_col(row, sp->call[0] ? sp->call : "-", 8, col, true, false);
        /* ⛔ ui_theme_mode_color() WAS THE WRONG FIX. Operator, 2026-09-12:
         * "The Mode text in the LIST tap is almost invisible - make text same
         * colour as Band". Cause found rather than guessed: that helper has no
         * case for "WSPR" (only DiGi/FT8/FT4/RTTY/USB/LSB/CW substrings), so
         * every WSPR spot's mode cell fell to its UNRECOGNISED-mode fallback,
         * UI_COLOR_KEY_BG (0x2a2a2a) - nearly the same as this panel's own
         * background. WSPR is the only source with any data this session, so
         * every mode cell on screen was that colour. There is no house "WSPR
         * mode colour" to add instead - it is a protocol on top of DiGi, not a
         * QMX CAT mode - so the plain, correct answer is the one asked for:
         * the same neutral UI_COLOR_TEXT the Band column already uses. */
        add_col(row, sp->grid[0] ? sp->grid : "-", 6, UI_COLOR_TEXT_SECONDARY, false, false);
        add_col(row, iso ? iso : "-", 6, UI_COLOR_TEXT_SECONDARY, false, false);
        add_col(row, sp->mode[0] ? sp->mode : "-", 4, UI_COLOR_TEXT, false, true);
        add_col(row, band[0] ? band : "-", 4, UI_COLOR_TEXT, false, true);
        add_col(row, freq_buf, 10, UI_COLOR_TEXT, false, true);
        add_col(row, snr_buf, 3, UI_COLOR_TEXT, false, true);
        add_col(row, dist_buf, 4, UI_COLOR_TEXT_SECONDARY, false, true);
        add_col(row, age_buf, 3, UI_COLOR_TEXT_SECONDARY, false, true);
        shown++;
    }

    if (shown == 0) {
        lv_obj_t *row = make_row(s_table_list);
        add_col(row, "Nobody has heard me yet (CW/Digi/WSPR).", 1, UI_COLOR_TEXT_MUTED, false, false);
    } else if (passed > shown) {
        /* A truncation nobody can see is worse than a shorter list that says
         * so - see TABLE_MAX_ROWS's own comment for why there is a cap at
         * all now. */
        char more[48];
        snprintf(more, sizeof(more), "... %d more not shown", passed - shown);
        lv_obj_t *row = make_row(s_table_list);
        add_col(row, more, 1, UI_COLOR_TEXT_MUTED, false, false);
    }

    /* Pay the layout cost NOW, not on the tap that first reveals this tab -
     * see the comment at the top of this function. */
    lv_obj_update_layout(s_table_list);
}

// ---- Conditions tab -------------------------------------------------------
// HF band conditions (net/band_conditions.c), ported from the sibling
// rbn_monitor project's own CONDITION tab - same hamqsl.com feed, same two
// tables (Band Conditions + Solar/Geomagnetic), same Good/Fair/Poor cell
// colouring. Deliberately just the propagation half of rbn_monitor's tab -
// its INFO sub-tab (airport weather, world-city clocks) answers a different
// question and has no home in a self-spotting map.

static lv_obj_t *s_bands_table = NULL;
static lv_obj_t *s_solar_table = NULL;
static int64_t   s_sig_cond_fetched_ms = -1;   // change-detection, see refresh_timer_cb

// Same panel look as build_sidebar()'s own lv_obj_create() (UI_COLOR_SURFACE
// fill, UI_COLOR_BORDER hairline) - this overlay has no other themed table to
// copy, so the sidebar panel is the closest existing precedent.
static void style_conditions_table(lv_obj_t *table)
{
    lv_obj_set_style_bg_color(table, lv_color_hex(UI_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, lv_color_hex(UI_COLOR_SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(table, lv_color_hex(UI_COLOR_BORDER), LV_PART_ITEMS);
    lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &lv_font_montserrat_26, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 10, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 10, LV_PART_ITEMS);
}

// Per-cell text colouring: header row (0) and the row-label column (0) get
// the same muted tone the LIST tab's own column headers use, so this table
// reads as part of the same UI rather than a bare LVGL widget dropped in.
// Everything else defaults to plain text - EXCEPT a Day/Night rating cell in
// the bands table, which is colour-coded green/amber/red for Good/Fair/Poor
// (the one place colour carries real meaning here, so it overrides the
// muted/plain default rather than the other way round).
static void conditions_table_draw_cb(lv_event_t *e)
{
    lv_obj_t *table = (lv_obj_t *)lv_event_get_target(e);
    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    if (base_dsc->part != LV_PART_ITEMS || lv_draw_task_get_type(draw_task) != LV_DRAW_TASK_TYPE_LABEL) return;
    uint16_t row = (uint16_t)base_dsc->id1;
    uint16_t col = (uint16_t)base_dsc->id2;
    lv_draw_label_dsc_t *label_dsc = (lv_draw_label_dsc_t *)base_dsc;

    if (row == 0 || col == 0) {
        label_dsc->color = lv_color_hex(UI_COLOR_TEXT_MUTED);
        return;
    }
    const char *text = lv_table_get_cell_value(table, row, col);
    if (!text) return;
    if      (strcmp(text, "Good") == 0) label_dsc->color = lv_color_hex(0x4CAF50);
    else if (strcmp(text, "Fair") == 0) label_dsc->color = lv_color_hex(0xFF9800);
    else if (strcmp(text, "Poor") == 0) label_dsc->color = lv_color_hex(0xF44336);
}

// Pushes a fetched band_conditions_t into both tables. Called only when
// refresh_timer_cb() notices fetched_ms actually changed - the feed updates
// roughly hourly, so there is nothing to redraw on most of the 1 Hz ticks.
static void update_conditions_tables(const band_conditions_t *c)
{
    if (s_bands_table) {
        for (int i = 0; i < BAND_COND_GROUP_COUNT; i++) {
            lv_table_set_cell_value(s_bands_table, (uint32_t)(i + 1), 1, c->day[i]);
            lv_table_set_cell_value(s_bands_table, (uint32_t)(i + 1), 2, c->night[i]);
        }
    }
    if (s_solar_table) {
        char val[16];
        snprintf(val, sizeof(val), "%d", c->solar_flux);
        lv_table_set_cell_value(s_solar_table, 0, 1, val);
        snprintf(val, sizeof(val), "%d", c->a_index);
        lv_table_set_cell_value(s_solar_table, 1, 1, val);
        snprintf(val, sizeof(val), "%d", c->k_index);
        lv_table_set_cell_value(s_solar_table, 2, 1, val);
        snprintf(val, sizeof(val), "%d", c->sunspots);
        lv_table_set_cell_value(s_solar_table, 3, 1, val);
        lv_table_set_cell_value(s_solar_table, 4, 1, c->geomag_field[0] ? c->geomag_field : "?");
        lv_table_set_cell_value(s_solar_table, 5, 1, c->signal_noise[0] ? c->signal_noise : "?");
    }
}

static void build_conditions_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    // Cross-axis (vertical, since flow is ROW) was CENTER - each column
    // centred independently within its own height, so the shorter table
    // (Solar/Geomagnetic, 6 plain rows) and the taller one (HF Band
    // Conditions, a header row + BAND_COND_GROUP_COUNT rows) had their TOPS
    // at different y - operator, 2026-09-13: "the two tables need to be
    // aligned so top of table is the same". START top-aligns both; only the
    // horizontal centering of the pair as a whole is meant to stay CENTER.
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_set_style_pad_gap(tab, 32, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    // Each column is a plain vertical flex stack (title, then table) sized to
    // its own content and centred as a block within the tab - rather than a
    // fixed-width half that pins the (narrower) table to its left edge, which
    // read as lopsided against the wide empty margin beside it.
    lv_obj_t *bands_area = lv_obj_create(tab);
    lv_obj_remove_style_all(bands_area);
    lv_obj_set_size(bands_area, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bands_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bands_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(bands_area, 12, 0);
    lv_obj_clear_flag(bands_area, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *solar_area = lv_obj_create(tab);
    lv_obj_remove_style_all(solar_area);
    lv_obj_set_size(solar_area, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(solar_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(solar_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(solar_area, 12, 0);
    lv_obj_clear_flag(solar_area, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bands_title = lv_label_create(bands_area);
    lv_label_set_text(bands_title, "HF Band Conditions");
    lv_obj_set_style_text_font(bands_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(bands_title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);

    s_bands_table = lv_table_create(bands_area);
    lv_table_set_column_count(s_bands_table, 3);
    lv_table_set_column_width(s_bands_table, 0, 170);
    lv_table_set_column_width(s_bands_table, 1, 130);
    lv_table_set_column_width(s_bands_table, 2, 130);
    lv_table_set_cell_value(s_bands_table, 0, 0, "Band");
    lv_table_set_cell_value(s_bands_table, 0, 1, "Day");
    lv_table_set_cell_value(s_bands_table, 0, 2, "Night");
    for (int i = 0; i < BAND_COND_GROUP_COUNT; i++) {
        lv_table_set_cell_value(s_bands_table, (uint32_t)(i + 1), 0, BAND_COND_GROUP_NAMES[i]);
        lv_table_set_cell_value(s_bands_table, (uint32_t)(i + 1), 1, "--");
        lv_table_set_cell_value(s_bands_table, (uint32_t)(i + 1), 2, "--");
    }
    style_conditions_table(s_bands_table);
    lv_obj_add_flag(s_bands_table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(s_bands_table, conditions_table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

    lv_obj_t *solar_title = lv_label_create(solar_area);
    lv_label_set_text(solar_title, "Solar / Geomagnetic");
    lv_obj_set_style_text_font(solar_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(solar_title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);

    s_solar_table = lv_table_create(solar_area);
    lv_table_set_column_count(s_solar_table, 2);
    lv_table_set_column_width(s_solar_table, 0, 240);
    lv_table_set_column_width(s_solar_table, 1, 200);
    lv_table_set_cell_value(s_solar_table, 0, 0, "Solar Flux Index");
    lv_table_set_cell_value(s_solar_table, 1, 0, "A-Index");
    lv_table_set_cell_value(s_solar_table, 2, 0, "K-Index");
    lv_table_set_cell_value(s_solar_table, 3, 0, "Sunspots");
    lv_table_set_cell_value(s_solar_table, 4, 0, "Geomag Field");
    lv_table_set_cell_value(s_solar_table, 5, 0, "Signal Noise");
    for (int i = 0; i < 6; i++) {
        lv_table_set_cell_value(s_solar_table, (uint32_t)i, 1, "--");
    }
    style_conditions_table(s_solar_table);
    lv_obj_add_flag(s_solar_table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(s_solar_table, conditions_table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

    // Seed once at build time in case a fetch already landed before this tab
    // was ever built (band_conditions_start() runs from app_main, this
    // overlay only when the operator first swipes it open).
    band_conditions_t c;
    if (band_conditions_get(&c)) {
        update_conditions_tables(&c);
        s_sig_cond_fetched_ms = c.fetched_ms;
    }
}

// ---- filter sidebar -----------------------------------------------------

// ⛔ rebuild_table() ALONE WAS NOT THE FIX - TABLE_MAX_ROWS BOUNDED THE
// ONE-TIME COST OF OPENING THE LIST, NOT THE RECURRING COST OF STAYING
// SOMEWHERE ELSE. refresh_now() used to call rebuild_table()
// unconditionally, every time refresh_timer_cb (1 Hz) saw a self-spot
// count or timestamp change - which is to say, every time RBN, the PSK
// self-spot MQTT feed or the wsprnet scrape heard something, REGARDLESS
// of which tab was on screen. That is lv_obj_clean() plus up to 41 row
// objects of several flex children each, paid in full while parked on
// MAP - live 2026-09-13: taskLVGL 96.9% of core 0 sustained, audio
// collapsed to ~3-4k pairs/s, repeated `cdc_acm TX transfer timeout`,
// buttons and the Zoom dropdown reading as "dead" - not a fresh freeze,
// the SAME cost this file already measured, just paid on a 1 Hz loop
// instead of once on open.
//
// Fix: rebuild only while LIST is the tab actually being looked at;
// otherwise remember that it is stale and pay for it once, lazily, the
// moment the operator switches TO it (tabview_event_cb below) - same
// "bound the WORK, don't just relocate when it is paid" rule the
// TABLE_MAX_ROWS comment above already states.
static bool s_table_dirty = true;

static void refresh_now(void)
{
    map_spots_changed();
    if (s_tabview && lv_tabview_get_tab_active(s_tabview) == 1) {
        rebuild_table();
        s_table_dirty = false;
    } else {
        s_table_dirty = true;
    }
}

// Pays the deferred rebuild exactly once, at the moment LIST actually
// becomes the visible tab - not before, and not repeated while it stays
// visible (nothing here marks it dirty again; the next real change does
// that through refresh_now() above).
static void tabview_changed_cb(lv_event_t *e)
{
    (void)e;
    if (!s_tabview) return;
    uint32_t active = lv_tabview_get_tab_active(s_tabview);

    /* ⛔ NO SWIPE OFF THE MAP. The whole map page is a pan surface, so a drag
     * across it is nearly always someone moving the map - and a tabview changes
     * tab by scrolling its own content sideways, so the two gestures are the
     * same gesture. Operator, 2026-09-18: "I want to zoom into New Zealand and
     * while panning down there the page tends to swipe to LIST instead - its
     * fighting the swipe feature".
     *
     * ⚠ map_sync_scroll_chain() already gave up the HORIZONTAL chain while
     * zoomed, and it was not enough: it only covers a press that lands on
     * s_map_obj itself, and only the horizontal axis, while a mostly-vertical
     * drag still carries enough sideways motion for the tabview to claim it.
     * Turning the content's own scrollability off is the whole-page answer and
     * does not depend on which child was pressed.
     *
     * ⭐ ONE DIRECTION ONLY, which is what makes this safe: it is switched off
     * while MAP is showing and back on everywhere else, so LIST -> CONDITIONS,
     * CONDITIONS -> LIST and LIST -> MAP all still swipe exactly as before.
     * The only journey that loses its gesture is the one that was fighting the
     * map, and the tab bar on the left is right there for it - a button, which
     * cannot be confused with a pan. */
    lv_obj_t *content = lv_tabview_get_content(s_tabview);
    if (content) {
        if (active == 0) lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        else             lv_obj_add_flag  (content, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Zoom only means anything on MAP - operator, 2026-09-13: "the zoom
    // dropdown shall be greyed out when not useful in LIST and CONDITIONS".
    // DISABLED blocks the tap; the opacity is what actually reads as
    // "inactive" at a glance, same convention top_bar_apply_mode() uses
    // elsewhere in this app for a control that means nothing right now.
    if (s_zoom_dd) {
        if (active == 0) {
            lv_obj_clear_state(s_zoom_dd, LV_STATE_DISABLED);
            lv_obj_set_style_opa(s_zoom_dd, LV_OPA_COVER, 0);
        } else {
            lv_obj_add_state(s_zoom_dd, LV_STATE_DISABLED);
            lv_obj_set_style_opa(s_zoom_dd, LV_OPA_40, 0);
        }
    }

    if (active == 1 && s_table_dirty) {
        rebuild_table();
        s_table_dirty = false;
    }
}

static void cw_cb(lv_event_t *e)
{
    s_show_cw = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    refresh_now();
}

static void digi_cb(lv_event_t *e)
{
    s_show_digi = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    refresh_now();
}

static void wspr_cb(lv_event_t *e)
{
    s_show_wspr = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    refresh_now();
}

static void older_cb(lv_event_t *e)
{
    s_show_older = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    refresh_now();
}

#define FILTER_ROW_BOX_SZ 31
#define FILTER_ROW_H      44

/* ⛔ HOUSE COLOURS AND AN ACTUAL CHECKMARK, NOT A COLOURED SWATCH.
 *
 * Operator, 2026-09-12: "The Source checkboxes need checkmarks not colours -
 * the box frame is also white - do like all over in the app please". A real
 * lv_checkbox gets its tick and its border/fill from the ACTIVE LVGL THEME
 * automatically applied to LV_PART_INDICATOR - this codebase never draws that
 * glyph itself (grepped for LV_SYMBOL_OK against every checkbox in the app:
 * none of them add one). Building the box as a plain lv_obj, as the "move it
 * to the right" fix did, opted out of that theme application and lost the
 * checkmark along with it - exactly what got reported.
 *
 * So this now matches ft8_filter_modal.c's make_checkbox() colour-for-colour
 * (UI_COLOR_SURFACE_RAISED/UI_COLOR_BORDER unchecked, UI_COLOR_PRIMARY/
 * UI_COLOR_PRIMARY_BORDER checked - the same pair the drawer and the FT8
 * filter modal use) and draws the tick itself via a child label. The per-
 * source ACCENT still lives on the row's own text label, which is the part
 * that pairs with a map dot's colour - the box was never what carried that
 * meaning, it just happened to be coloured the same way. */
static void filter_box_paint(lv_obj_t *box, bool checked)
{
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    if (checked) {
        lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
    } else {
        lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
        lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_BORDER), 0);
    }
    lv_obj_t *tick = lv_obj_get_child(box, 0);   /* the checkmark label, see below */
    if (tick) {
        if (checked) lv_obj_clear_flag(tick, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
    }
}

/* One row IS the checkbox - a plain lv_obj carrying LV_STATE_CHECKED like any
 * other stateful widget, rather than lv_checkbox. LVGL 9.2.2's checkbox draws
 * its indicator at a hard-coded position INSIDE lv_checkbox_draw() (verified
 * against managed_components/lvgl__lvgl/src/widgets/checkbox/lv_checkbox.c -
 * the marker is computed relative to the object's own bounds, always before
 * the text, not as a repositionable child), so "indicator on the right" has
 * no style-only answer for that widget. Patching that draw function would add
 * a NINETEENTH standing patch purely for one panel's layout; building the row
 * by hand keeps the change local to this file instead. */
static void filter_row_clicked_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_t *box = lv_obj_get_child(row, -1);   /* box is added last, below */
    bool now_checked = !lv_obj_has_state(row, LV_STATE_CHECKED);
    if (now_checked) lv_obj_add_state(row, LV_STATE_CHECKED);
    else             lv_obj_clear_state(row, LV_STATE_CHECKED);
    filter_box_paint(box, now_checked);
    /* The caller's cb (cw_cb/digi_cb/...) reads LV_STATE_CHECKED off the event
     * TARGET - sending VALUE_CHANGED on `row` itself keeps every one of them
     * unchanged; they never knew they were reading a real lv_checkbox. */
    lv_obj_send_event(row, LV_EVENT_VALUE_CHANGED, NULL);
}

static lv_obj_t *add_filter_checkbox(lv_obj_t *parent, const char *label, uint32_t accent, lv_event_cb_t cb)
{
    /* ⛔ HOUSE SIZE, NOT LVGL'S DEFAULT, AND THE BOX ON THE RIGHT.
     *
     * Three rounds of operator feedback, 2026-09-12, on this one row:
     *  1. "the panel text and checkboxes are far too small" - montserrat_20
     *     where the settings drawer uses 28, and a stock-size indicator with
     *     no enlarged hit area (the same fix Don WB0LQW got applied twice
     *     elsewhere - see ft8_filter_modal.c's own comment).
     *  2. "checkboxes do not react to touches - too close to the edge - also
     *     need more space between them" - the sidebar's own pad_all put the
     *     indicator ~14 px from the physical screen edge where
     *     ext_click_area cannot help (LVGL clips a child's hit area to its
     *     parent), and adjacent 28 px halos on a 10 px row gap OVERLAPPED, so
     *     a tap between two boxes went to whichever hit-tested first.
     *  3. "move checkboxes to right (with more space in between)" - settled
     *     the question the second comment above had left open.
     *
     * The row IS the click target (full sidebar width), so the edge and
     * overlap problems from round 2 are gone by construction: there is
     * nothing between rows for a tap to land on ambiguously, and the row's
     * own width already reaches the panel's inner edge with the sidebar's
     * pad_all as the only margin - no ext_click_area needed on the label side
     * at all. The box keeps a small halo for the case where a tap lands just
     * past its edge. */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), FILTER_ROW_H);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN_VER);   /* see round 2, same reasoning */
    lv_obj_add_state(row, LV_STATE_CHECKED);                /* all three default ON */

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(accent), 0);
    /* Width capped rather than left auto: an absolute-positioned box on the
     * right does not push the label, so a long label ("Older >30 min") with no
     * cap could grow under it instead of stopping short. LONG_DOT truncates
     * rather than overlaps if it ever does run long. */
    lv_obj_set_width(lbl, SIDEBAR_W - 28 - FILTER_ROW_BOX_SZ - 16);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, FILTER_ROW_BOX_SZ, FILTER_ROW_BOX_SZ);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);   /* the ROW takes the tap, not the box */
    lv_obj_align(box, LV_ALIGN_RIGHT_MID, 0, 0);

    /* The tick itself - child 0, which is what filter_box_paint() looks up by
     * index rather than a stored handle. White on the filled PRIMARY
     * background, same as a real lv_checkbox's theme-drawn indicator. */
    lv_obj_t *tick = lv_label_create(box);
    lv_label_set_text(tick, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(tick, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(tick);

    filter_box_paint(box, true);   /* matches the row's initial CHECKED state */

    lv_obj_set_ext_click_area(row, 8);
    lv_obj_add_event_cb(row, filter_row_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(row, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}

// Empties all three ring buffers immediately - net/rbn.c, net/pskr_self.c and
// net/wspr_self.c keep receiving new self-spots afterward as normal, nothing
// here touches the RBN session, the MQTT subscription, or the wsprnet poll.
static void flush_btn_cb(lv_event_t *e)
{
    (void)e;
    rbn_self_spots_clear();
    pskr_self_clear();
    wspr_self_spots_clear();
    refresh_now();
}

// ⛔ WAS THE LEFT SIDEBAR, NOW A RIGHT-EDGE SETTINGS DRAWER (2026-09-13).
//
// Four operator asks, all about this one panel:
//   3. "all breathing swipe handles are gone but one: a settings drawer to
//      the right as usual" - sync_nav_affordances() already hides every MAIN
//      APP edge strip while this overlay is active; this file gets its OWN
//      right-edge swipe, local to the overlay, for a settings panel local to
//      the overlay.
//   4. "The drawer will contain: User Manual, Need Guidance - then have those
//      settings (checkboxes) moved from the left panel as it is now (CW Digi
//      WSPR + Age)" - so the content below is exactly the OLD sidebar's
//      content, unchanged, with two new buttons above it.
//   5. Moving the checkboxes out is what frees the left panel for
//      MAP/LIST/CONDITIONS (see spot_map_view_init() - lv_tabview's own tab
//      bar, moved to LV_DIR_LEFT, replaces this whole object visually).
//
// Flush has no explicit home in the operator's list (User Manual, Need
// Guidance, checkboxes) - it stays here, at the bottom, because "clear the
// self-spot data" is a settings-panel action the same way the checkboxes
// are, not a MAP/LIST/CONDITIONS destination. Easy to move if that reading is
// wrong.
static lv_obj_t *s_settings_drawer = NULL;
static void settings_close(void);   // fwd - defined with the scrim, below
static void settings_add_swipe(lv_obj_t *obj);   // fwd - same place

static void settings_user_manual_cb(lv_event_t *e)
{
    (void)e;
    settings_close();
    ui_open_user_manual();
}

static void settings_need_guidance_cb(lv_event_t *e)
{
    (void)e;
    settings_close();
    help_triage_open();
}

static void settings_grip_cb(lv_event_t *e)
{
    (void)e;
    settings_close();
}


static void build_settings_drawer(lv_obj_t *parent)
{
    lv_obj_t *sb = lv_obj_create(parent);
    s_settings_drawer = sb;
    lv_obj_set_size(sb, SIDEBAR_W, SCR_H - HEADER_H);
    /* Starts OFF-SCREEN (x = SCR_W, flush with the right edge and extending
     * further right, entirely outside the display) rather than HIDDEN - same
     * as ui.c's own drawer_open()/drawer_close(), which never touch a hidden
     * flag on s_drawer at all. This is what makes a SLIDING animation
     * possible: settings_open()/settings_close() animate x between here and
     * SCR_W - SIDEBAR_W, and an object entirely outside the screen area is
     * naturally neither drawn nor hit-tested, so nothing extra is needed to
     * keep it out of the way while "closed". */
    lv_obj_set_pos(sb, SCR_W, HEADER_H);
    lv_obj_set_style_bg_color(sb, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(sb, LV_BORDER_SIDE_LEFT, 0);   /* was RIGHT, sat on the left edge before */
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_pad_all(sb, 14, 0);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(sb, 20, 0);
    // ⛔ THIS WAS THE SAME "PANEL HEIGHT IS ONE BUDGET" BUG CLAUDE.md ALREADY
    // WARNS ABOUT, IN A NEW PLACE. Scrolling was cleared here, and by the time
    // the Age section (label + checkbox + the "bright/faded" key) was added
    // below the three Source checkboxes, the drawer's own content ran to
    // roughly 718 px against 656 px of actual height (SCR_H - HEADER_H, minus
    // top/bottom padding) - a ~62 px overrun that landed almost exactly on
    // Flush, at the very bottom. With scrolling off there was no scrollbar to
    // even hint that more content existed: Flush was not hidden, not moved,
    // just silently clipped and permanently unreachable. Operator, 2026-09-16,
    // looking straight at a screenshot ending at "faded = older": "I might be
    // blind - but where is the flush button right now?" He was not blind -
    // scrolling was OFF, so there was truly nothing further to find.
    // Re-enabled rather than shrinking content to fit THIS session's row
    // count: the next Source/Age row added would only reopen the same gap.
    // settings_swipe_cb's open/close gesture (above) tracks horizontal drag
    // distance only (dx) and fires from PRESSED/PRESSING/RELEASED events that
    // LVGL delivers to this object regardless of its own scroll state, so a
    // sideways swipe to close the drawer and an up/down scroll inside it
    // cannot conflict - and add_filter_checkbox() already clears
    // LV_OBJ_FLAG_SCROLL_CHAIN_VER on each row, so a tap ON a checkbox still
    // cannot be mistaken for a drag on the drawer around it.
    lv_obj_set_scrollbar_mode(sb, LV_SCROLLBAR_MODE_AUTO);

    // User Manual + Need Guidance - same two doors the main drawer offers,
    // same order, so this panel reads as a drawer rather than a stranger.
    {
        lv_obj_t *btn = lv_button_create(sb);
        lv_obj_set_size(btn, SIDEBAR_W - 28, 56);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, settings_user_manual_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(btn);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_26, 0);
        lv_label_set_text(l, LV_SYMBOL_FILE "  User Manual");
        lv_obj_center(l);
    }
    {
        lv_obj_t *btn = lv_button_create(sb);
        lv_obj_set_size(btn, SIDEBAR_W - 28, 56);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a3138), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, settings_need_guidance_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(btn);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_26, 0);
        lv_label_set_text(l, LV_SYMBOL_LIST "  Need guidance?");
        lv_obj_center(l);
    }

    lv_obj_t *lbl = lv_label_create(sb);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_label_set_text(lbl, "Source");

    add_filter_checkbox(sb, "CW (RBN)",    UI_COLOR_MODE_CW,   cw_cb);
    add_filter_checkbox(sb, "Digi (PSKR)", UI_COLOR_MODE_DIGI, digi_cb);
    add_filter_checkbox(sb, "WSPR",        UI_COLOR_MODE_WSPR, wspr_cb);

    // "Age" heading REMOVED (operator, 2026-09-16: "remove the text line Age
    // and move up the Older >30 min") - it filters a different axis than the
    // three Source rows above it (WHEN a spot arrived, not WHERE it came
    // from), but that distinction did not need its own row once the panel was
    // already tight on vertical room. The checkbox's own label still says
    // "Older >30 min", which carries the same meaning on its own.
    add_filter_checkbox(sb, "Older >30 min", UI_COLOR_TEXT_SECONDARY, older_cb);

    /* Says what the dimming MEANS. The map draws older spots at a third of the
     * opacity in the same source colour, which is only readable as "older" if
     * something on screen says so. */
    lv_obj_t *age_key = lv_label_create(sb);
    lv_label_set_text(age_key, "bright = new <30 min\nfaded = older");
    lv_obj_set_style_text_font(age_key, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(age_key, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);

    s_grid_warn = lv_label_create(sb);
    lv_obj_set_style_text_font(s_grid_warn, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_grid_warn, lv_color_hex(UI_COLOR_DANGER_BORDER), 0);
    lv_label_set_long_mode(s_grid_warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_grid_warn, SIDEBAR_W - 28);
    lv_obj_set_style_pad_top(s_grid_warn, 10, 0);
    lv_label_set_text(s_grid_warn, "");   // filled in by refresh, see refresh_timer_cb
    // HIDDEN when empty (the common case - a grid square is normally set),
    // not just empty-texted. A flex child still claims a row for its own
    // empty-string line height plus this label's own 10 px pad_top even with
    // nothing to show, and that is exactly the blank line the operator saw
    // sitting between the "bright/faded" key text above and Flush below
    // ("remove the line space between the helping grey text and the Flush
    // button") - LVGL's flex layout skips a HIDDEN object entirely, which a
    // merely-empty one does not get. refresh_timer_cb toggles this flag in
    // the same place it sets the text, so a grid square typed in later still
    // makes the real warning reappear and claim its row back.
    lv_obj_add_flag(s_grid_warn, LV_OBJ_FLAG_HIDDEN);

    /* ⛔ THE DRAWER'S OWN FLUSH BUTTON IS GONE (2026-09-17, operator's call).
     * Uwe DL8UG's header button does the same job - it calls this same
     * flush_btn_cb() - and is reachable without opening the drawer at all,
     * which was his whole reason for adding it. Two entry points to one
     * destructive action is one more than the screen needs.
     *
     * flush_btn_cb() itself stays: it is now the header button's callback. */

    /* The handle, a CHILD of the drawer on its LEFT edge - the edge that
     * travels - copied from ui.c's s_drawer_grip. ⛔ Not on the screen edge:
     * that was tried here (parented to the overlay, breathing) and the
     * operator rejected it - this is a drawer handle, so it comes out WITH
     * the drawer and you push it back the way it came. No breathing: that
     * means "hidden gesture here", and a visible handle is not hidden.
     * -14 cancels the drawer's own pad_all(14); FLOATING keeps it out of the
     * column flex layout. */
    lv_obj_t *grip = lv_obj_create(sb);
    lv_obj_set_size(grip, 10, 120);
    lv_obj_add_flag(grip, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(grip, LV_ALIGN_LEFT_MID, -14, 0);
    lv_obj_set_style_bg_color(grip, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_30, 0);
    lv_obj_set_style_border_width(grip, 0, 0);
    lv_obj_set_style_radius(grip, 5, 0);
    lv_obj_set_style_pad_all(grip, 0, 0);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(grip, 12);
    lv_obj_add_event_cb(grip, settings_grip_cb, LV_EVENT_PRESSED, NULL);   // on touch, not on lift

    // A rightward drag on the drawer's own background closes it too.
    settings_add_swipe(sb);
}

// Right-edge swipe (drag left) toggles the settings drawer above - same
// gesture shape as the main app's own right-edge swipe (ui.c), local to this
// overlay because sync_nav_affordances() already hides the main app's strip
// while this overlay is on screen (spot_map_view_is_active()).
//
// No mouse-click affordance (grip_mouse_click() in ui.c is private to that
// file, and a BLE mouse on this screen is a secondary path) - a known,
// deliberate simplification, not a silent omission.
static lv_obj_t *s_settings_strip = NULL;
static lv_obj_t *s_settings_scrim = NULL;
static int       s_settings_swipe_start_x = -1;

/* ⛔ THREE REAL BUGS FOUND FROM ONE REPORT, 2026-09-13, and the first pass at
 * fixing them was itself wrong twice over. Operator: "make the drawer go
 * away by either taping outside it or swiping back... Now the drawer do not
 * close the right way - going in like a drawer... or even open like a
 * drawer... The drawers in any other page has a strict process."
 *
 * 1. No tap-outside-to-close existed - fixed below, settings_scrim_cb,
 *    same close-on-PRESS pattern as ui.c's own drawer_scrim_cb.
 * 2. ⛔ THE GRIP IS TWO OBJECTS, NOT ONE - exactly as on the main drawer, and
 *    getting that wrong cost four rounds. CLOSED: a breathing 4x120 bar on the
 *    SCREEN edge (ui.c's s_burger_btn), parented to the overlay and drawn
 *    under the drawer so the drawer covers it when open - build_settings_grip().
 *    OPEN: a still 10x120 handle that is a CHILD of the drawer on its
 *    travelling left edge (ui.c's s_drawer_grip) - end of
 *    build_settings_drawer(). Removing either one is the regression; the
 *    operator has rejected each half on its own.
 * 3. This one was never actually diagnosed before now: s_settings_drawer
 *    toggled LV_OBJ_FLAG_HIDDEN, an instant snap with no motion at all,
 *    where ui.c's drawer_open()/drawer_close() SLIDE it (250 ms, ease
 *    out/in) and never touch a hidden flag on the drawer itself - "closed"
 *    is just x = SCR_W (off-screen). settings_open()/settings_close() now
 *    do the same slide, via s_settings_open tracking state since HIDDEN no
 *    longer can. */
static bool s_settings_open = false;

/* ⛔ 180 ms, and the gesture fires MID-SWIPE. Operator, 2026-09-13: "please
 * make sure it opens as soon as i swipe - and closes same speed". Two costs
 * were stacked: the swipe was only judged on RELEASE (so nothing moved until
 * the finger lifted), then a 250 ms slide whose every frame re-drew the whole
 * map underneath. The map is a cached image now (map_cache_rebuild), the swipe
 * fires the moment it has travelled SS_EDGE_MIN_DX while still down, and open
 * and close share one duration so they feel the same. */
#define SETTINGS_SLIDE_MS 180

static void settings_drawer_anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void settings_slide(int32_t to_x)
{
    lv_anim_delete(s_settings_drawer, settings_drawer_anim_x_cb);   // reversing mid-slide starts from where it IS
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_settings_drawer);
    lv_anim_set_exec_cb(&a, settings_drawer_anim_x_cb);
    lv_anim_set_values(&a, lv_obj_get_x(s_settings_drawer), to_x);
    lv_anim_set_time(&a, SETTINGS_SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void settings_open(void)
{
    if (s_settings_open) return;
    if (s_settings_scrim) {
        lv_obj_clear_flag(s_settings_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_settings_scrim);
    }
    lv_obj_move_foreground(s_settings_drawer);
    settings_slide(SCR_W - SIDEBAR_W);
    s_settings_open = true;
}

static void settings_close(void)
{
    if (!s_settings_open) return;
    if (s_settings_scrim) lv_obj_add_flag(s_settings_scrim, LV_OBJ_FLAG_HIDDEN);
    settings_slide(SCR_W);
    s_settings_open = false;
}

static void settings_scrim_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    settings_close();
}

/* One handler for both directions: a leftward drag from the screen edge opens,
 * a rightward drag on the drawer or its handle closes. Fires once per press,
 * as soon as the distance is reached - never waits for the finger to lift. */
static bool s_settings_swipe_fired = false;

static void settings_swipe_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_settings_swipe_start_x = (int)p.x;
        s_settings_swipe_fired = false;
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_settings_swipe_start_x = -1;
        return;
    }
    if (code != LV_EVENT_PRESSING || s_settings_swipe_fired || s_settings_swipe_start_x < 0) return;
    int dx = (int)p.x - s_settings_swipe_start_x;
    if (!s_settings_open && dx <= -SS_EDGE_MIN_DX) { s_settings_swipe_fired = true; settings_open(); }
    else if (s_settings_open && dx >= SS_EDGE_MIN_DX) { s_settings_swipe_fired = true; settings_close(); }
}

static void settings_add_swipe(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, settings_swipe_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, settings_swipe_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, settings_swipe_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, settings_swipe_cb, LV_EVENT_PRESS_LOST, NULL);
}

static void grip_breathe_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

// The CLOSED-state grip: byte-for-byte ui.c's s_burger_btn and its
// grip_start_breathing() (both static there, hence the copy). Non-clickable -
// the press lands on the edge strip beneath it, exactly as on the main screen.
static void build_settings_grip(lv_obj_t *parent)
{
    lv_obj_t *grip = lv_obj_create(parent);
    lv_obj_set_size(grip, 4, 120);
    lv_obj_align(grip, LV_ALIGN_RIGHT_MID, 0, HEADER_H / 2);   // centred on the area below the header
    lv_obj_set_style_bg_color(grip, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_30, 0);
    lv_obj_set_style_border_width(grip, 0, 0);
    lv_obj_set_style_radius(grip, 5, 0);
    lv_obj_set_style_shadow_width(grip, 0, 0);
    lv_obj_set_style_pad_all(grip, 0, 0);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, grip);
    lv_anim_set_exec_cb(&a, grip_breathe_anim_cb);
    lv_anim_set_values(&a, LV_OPA_10, LV_OPA_60);
    lv_anim_set_time(&a, 1400);
    lv_anim_set_playback_time(&a, 1400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// Covers the tabview (MAP/LIST/PROP) area to the LEFT of the drawer -
// everything the drawer does NOT occupy - same footprint rule as ui.c's own
// s_drawer_scrim. Hidden until settings_open() shows it; a press anywhere on
// it closes the drawer (settings_scrim_cb).
static void build_settings_scrim(lv_obj_t *parent)
{
    lv_obj_t *scrim = lv_obj_create(parent);
    lv_obj_set_size(scrim, SCR_W - SIDEBAR_W, SCR_H - HEADER_H);
    lv_obj_set_pos(scrim, 0, HEADER_H);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);   /* only while the drawer is open */
    lv_obj_add_event_cb(scrim, settings_scrim_cb, LV_EVENT_PRESSED, NULL);
    s_settings_scrim = scrim;
}

static void build_settings_edge_strip(lv_obj_t *parent)
{
    lv_obj_t *strip = lv_obj_create(parent);
    lv_obj_set_size(strip, SS_EDGE_ZONE_PX, SCR_H - HEADER_H);
    lv_obj_set_pos(strip, SCR_W - SS_EDGE_ZONE_PX, HEADER_H);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    settings_add_swipe(strip);
    s_settings_strip = strip;
    /* No grip child - build_settings_grip() is the separate closed-state bar,
     * see the ⛔ note above s_settings_open. */
}

// ---- refresh timer + overlay lifecycle -----------------------------------

// Cheap change-detection instead of a blind per-tick redraw: the first
// version invalidated the map (and so redrew the whole ~1500-segment world
// outline) once a second unconditionally, which is what made the device feel
// out of headroom on hardware. Self-spot data changes far less often than
// that - all three sources' self-spots are individually rare events - so
// this only pays the redraw cost when something actually changed. The
// signature is deliberately coarse (count + newest timestamp per source):
// good enough to catch "a new spot arrived", cheap enough to check every tick.
static int s_sig_rbn_n = -1, s_sig_psk_n = -1, s_sig_wspr_n = -1;
static int64_t s_sig_rbn_t = -1, s_sig_psk_t = -1, s_sig_wspr_t = -1;

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    update_info_line();
    refresh_own_position();

    // A spot fades when it passes 30 min even if nothing new arrives - redraw
    // the spot layer (not the coastline) once a minute for the ageing.
    {
        static int64_t s_last_age_min = -1;
        int64_t m = (int64_t)time(NULL) / 60;
        if (m != s_last_age_min) { s_last_age_min = m; if (s_map_obj) lv_obj_invalidate(s_map_obj); }
    }

    static EXT_RAM_BSS_ATTR rbn_self_spot_t rbn[100];   // NOT internal .bss - see the note above gather_self_spots()
    int rn = rbn_self_spots_get(rbn, 100);
    int64_t rbn_newest = 0;
    for (int i = 0; i < rn; i++) if (rbn[i].heard_unix > rbn_newest) rbn_newest = rbn[i].heard_unix;

    static EXT_RAM_BSS_ATTR pskr_self_spot_t psk[100];  // NOT internal .bss - see the note above gather_self_spots()
    int pn = pskr_self_spots_get(psk, 100);
    int64_t psk_newest = 0;
    for (int i = 0; i < pn; i++) if (psk[i].heard_unix > psk_newest) psk_newest = psk[i].heard_unix;

    static EXT_RAM_BSS_ATTR wspr_self_spot_t wspr[100]; // NOT internal .bss - see the note above gather_self_spots()
    int wn = wspr_self_spots_get(wspr, 100);
    int64_t wspr_newest = 0;
    for (int i = 0; i < wn; i++) if (wspr[i].heard_unix > wspr_newest) wspr_newest = wspr[i].heard_unix;

    bool changed = (rn != s_sig_rbn_n) || (rbn_newest != s_sig_rbn_t) ||
                   (pn != s_sig_psk_n) || (psk_newest != s_sig_psk_t) ||
                   (wn != s_sig_wspr_n) || (wspr_newest != s_sig_wspr_t);
    if (changed) {
        s_sig_rbn_n = rn; s_sig_rbn_t = rbn_newest;
        s_sig_psk_n = pn; s_sig_psk_t = psk_newest;
        s_sig_wspr_n = wn; s_sig_wspr_t = wspr_newest;
        /* Re-frame for anything that has arrived since - but ONLY while the
         * view still belongs to the map. See s_view_is_users. A spot further
         * away than everything present at open would otherwise be drawn
         * outside the frame and never seen. */
        if (!s_view_is_users) map_fit_to_spots();
        refresh_now();
    }

    if (s_grid_warn) {
        lv_label_set_text(s_grid_warn, s_have_me ? "" :
            "No home grid square set (Settings -> My Grid) - the map cannot draw any lines.");
        // See its own comment at creation - hidden (not just empty-texted)
        // so it claims no flex row when there is nothing to warn about.
        if (s_have_me) lv_obj_add_flag(s_grid_warn, LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_clear_flag(s_grid_warn, LV_OBJ_FLAG_HIDDEN);
    }

    // Band conditions update roughly hourly - fetched_ms is the same cheap
    // change-detection signature as the self-spot counts above, just for a
    // feed that changes far less often still.
    band_conditions_t c;
    if (band_conditions_get(&c) && c.fetched_ms != s_sig_cond_fetched_ms) {
        s_sig_cond_fetched_ms = c.fetched_ms;
        update_conditions_tables(&c);
    }
}

/* Logged with where and how long, for the same reason the top-edge gesture that
 * opens this map is (see top_edge_swipe_cb in ui.c): on 2026-09-11 this button
 * closed the map at 17:10:08 UTC with nobody touching the Tab5. */
static int64_t s_exit_press_us;
static lv_point_t s_exit_press_pt;

static void exit_btn_press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) lv_indev_get_point(indev, &s_exit_press_pt);
    s_exit_press_us = esp_timer_get_time();
}

static void exit_btn_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t p = { 0, 0 };
    if (indev) lv_indev_get_point(indev, &p);
    ESP_LOGI(TAG, "Exit: (%d,%d) -> (%d,%d) in %d ms",
             (int)s_exit_press_pt.x, (int)s_exit_press_pt.y, (int)p.x, (int)p.y,
             (int)((esp_timer_get_time() - s_exit_press_us) / 1000));
    spot_map_view_hide();
}

/* Zoom dropdown - centered in the header. Operator, 2026-09-13: "a labelled
 * Zoom dropdown centered between SELFSPOTTER and Exit: Fit (like when we
 * enter the MAP first time), x1, x2, x3, x4, x5, x10 (challenge me on those
 * zoom levels please)". Two real findings from taking up that challenge:
 *
 *  - x10 needed MAP_ZOOM_MAX raised from 8 to 10 (see that define's own
 *    comment) so pinch and this dropdown agree on a ceiling.
 *  - "Fit" is not a fixed number - it is exactly what map_fit_to_spots()
 *    already computes on every fresh open, so selecting it just re-runs that
 *    same function rather than jumping to some particular zoom value.
 *
 * Extended to x20/x50, 2026-09-16, matching MAP_ZOOM_MAX's own second raise -
 * same reasoning, this list must never fall short of what pinching can reach.
 *
 * Picking x1..x50 marks the view as the operator's (s_view_is_users = true),
 * same as a pinch would - a deliberate zoom choice should not be silently
 * overridden the next time a new spot arrives (see s_view_is_users's own
 * comment). It does NOT track live pinch zoom back onto itself - the
 * dropdown is a quick-jump, not a synchronized readout, and trying to keep a
 * discrete control in step with a continuous one is not what was asked for. */
static void zoom_dropdown_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t idx = lv_dropdown_get_selected(dd);
    static const float kZoom[] = { 0.0f /* Fit */, 1, 2, 3, 4, 5, 10, 20, 50 };
    if (idx >= sizeof(kZoom) / sizeof(kZoom[0])) return;
    if (idx == 0) {
        s_view_is_users = false;   /* map_fit_to_spots() sets it back anyway - explicit for clarity */
        map_fit_to_spots();
    } else {
        s_map_zoom = kZoom[idx];
        s_view_is_users = true;
        map_sync_scroll_chain();
    }
    map_mark_dirty();
}

static void zoom_dropdown_open_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (!list) return;
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, 0);
    /* ⛔ ONLY x1-x5 WERE VISIBLE. Operator, 2026-09-13: "zoom dropdown and saw
     * only x1 - x5? then scrolling dropdown and dropdown list became all
     * white and froze". LVGL's dropdown list has a default max-height that
     * clips a 7-option list at montserrat_28 and shows a scrollbar for the
     * rest - exactly ui.c's own sleep-dropdown comment already describes
     * ("LVGL caps the option-list height by default, which forces a
     * scrollbar; remove the cap and size to content"). That fix was written
     * once, for that dropdown, and never applied here. Same recipe.
     *
     * This may also be the whole story behind "became all white and froze":
     * with no cap there is nothing left to scroll, so that interaction can
     * no longer happen at all. Recorded as a plausible explanation, not a
     * confirmed one - the freeze could equally have been fallout from the
     * LIST tab's own CPU-saturation event landing moments earlier (see
     * TABLE_MAX_ROWS's comment); nothing pins down which. */
    lv_obj_set_style_max_height(list, LV_COORD_MAX, 0);
    lv_obj_set_height(list, LV_SIZE_CONTENT);
}

void spot_map_view_init(lv_obj_t *parent)
{
    if (s_overlay) return;

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0a0d10), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);   // swallow touches so gestures behind it can't fire

    // Header strip - title + Exit, same visual language as reader_view.c.
    lv_obj_t *hdr = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, SCR_W, HEADER_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- THE HEADER IS ONE BUDGET, NOT FOUR ANCHORS -----------------------
     * Zoom was anchored to the header's CENTRE while Flush and Exit were
     * anchored to its RIGHT edge and the ID line to its LEFT - so inserting
     * Flush moved nothing and simply drew over the ID line (Gyula HA3HZ's
     * callsign/time/frequency, added in v1.14.2). Three origins, no shared
     * budget, and the collision was invisible until a screenshot with a
     * callsign actually set.
     *
     * All four now sit on ONE run with EQUAL gaps, laid out left to right
     * from the title's right edge to the right margin. Widths are measured,
     * not guessed - Exit and Flush from a pixel scan of a live screenshot.
     *
     *   run   = HDR_RUN_R - HDR_RUN_L            = 1256 - 256 = 1000
     *   items = 254 (zoom) + 193 + 138 + 102     = 687
     *   gap   = (1000 - 687) / 4                 = 78
     *
     * Add or resize anything here and the gap recomputes itself. */
    #define HDR_RUN_L     256   /* right edge of "SELFSPOTTER" (montserrat_32) */
    #define HDR_RUN_R    1256   /* SCR_W - 24 right margin */
    #define ZOOM_GRP_W    254   /* ZOOM_LBL_W + ZOOM_GAP + ZOOM_DD_W, below */
    #define INFO_GAP_W    193
    #define FLUSH_BTN_W   138   /* measured: trash glyph + "Flush" + 2*20 pad */
    #define EXIT_BTN_W    102   /* measured: x=1147..1249 on a live screenshot */
    #define HDR_GAP  (((HDR_RUN_R - HDR_RUN_L) - (ZOOM_GRP_W + INFO_GAP_W +                        FLUSH_BTN_W + EXIT_BTN_W)) / 4)
    #define ZOOM_X   (HDR_RUN_L + HDR_GAP)
    #define INFO_GAP_X (ZOOM_X + ZOOM_GRP_W + HDR_GAP)
    #define FLUSH_X  (INFO_GAP_X + INFO_GAP_W + HDR_GAP)
    #define EXIT_X   (FLUSH_X + FLUSH_BTN_W + HDR_GAP)

    lv_obj_t *title = lv_label_create(hdr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);
    lv_label_set_text(title, "SELFSPOTTER");

    // Exit is a child of the OVERLAY, not the header bar, so its extended hit
    // area can reach below the 64 px bar - same reasoning as reader_view.c's
    // header buttons (LVGL clips a child's hit area to its parent).
    lv_obj_t *exit_btn = lv_button_create(s_overlay);
    lv_obj_align(exit_btn, LV_ALIGN_TOP_LEFT, EXIT_X, (HEADER_H - 46) / 2);
    lv_obj_set_style_bg_color(exit_btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_pad_hor(exit_btn, 20, 0);
    lv_obj_set_height(exit_btn, 46);
    lv_obj_set_ext_click_area(exit_btn, 44);
    lv_obj_add_event_cb(exit_btn, exit_btn_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(exit_btn, exit_btn_cb, LV_EVENT_CLICKED, NULL);
    ui_kbd_set_buttons(NULL, exit_btn);   // Esc leaves the map, same as the Reader
    lv_obj_t *exit_lbl = lv_label_create(exit_btn);
    lv_obj_set_style_text_font(exit_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(exit_lbl, LV_SYMBOL_CLOSE "  Exit");

    // Flush - same header-bar treatment as Exit (child of the OVERLAY for the
    // same extended-hit-area reason), sat directly left of it via align_to so
    // it never depends on Exit's own content-sized width. Reuses
    // flush_btn_cb() - the same "empty all three self-spot ring buffers"
    // action the settings-drawer Flush button already has; this is a second,
    // quicker-to-reach way to trigger it, not a replacement for that one.
    lv_obj_t *flush_hdr_btn = lv_button_create(s_overlay);
    // align_to(exit_btn, OUT_LEFT_MID) was tried first and measured wrong on
    // real hardware TWICE, overlapping Exit both times (operator: "muss
    // weiter nach links geschoben werden ... etwas höher align mit dem exit
    // button") - a pixel-level screenshot check (/ss.bmp, scanning for the
    // button's own background colour) found exit_btn's rendered box at
    // x=1147..1249, y=9..56, while align_to had placed flush's right edge at
    // x=1219 and its vertical band at y=19..64 - both badly off, and adding
    // lv_obj_update_layout(exit_btn) right before the align_to call (the
    // fix adif_view_modal.c/ft8_filter_modal.c use for a lazily-sized
    // sibling) made no measurable difference on a re-flash, so the problem
    // isn't (only) that. Rather than keep guessing against align_to's
    // timing, this uses the SAME fixed TOP_RIGHT-of-s_overlay recipe
    // exit_btn itself uses - identical y formula, so vertical match is
    // exact instead of inferred, and an x offset sized from exit_btn's
    // MEASURED width (-24 right edge, ~109 px wide) plus a 20 px gap.
    lv_obj_align(flush_hdr_btn, LV_ALIGN_TOP_LEFT, FLUSH_X, (HEADER_H - 46) / 2);
    lv_obj_set_style_bg_color(flush_hdr_btn, lv_color_hex(UI_COLOR_DANGER), 0);
    lv_obj_set_style_pad_hor(flush_hdr_btn, 20, 0);
    lv_obj_set_height(flush_hdr_btn, 46);
    lv_obj_set_ext_click_area(flush_hdr_btn, 44);
    lv_obj_add_event_cb(flush_hdr_btn, flush_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *flush_hdr_lbl = lv_label_create(flush_hdr_btn);
    lv_obj_set_style_text_font(flush_hdr_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(flush_hdr_lbl, LV_SYMBOL_TRASH "  Flush");

    // Zoom - centered in the header, same lv_dropdown + montserrat_28 +
    // "fix the popup list's own font" recipe every other dropdown in this
    // app uses (e.g. ui.c's waterfall-colour-map dropdown). A child of the
    // header bar, not the overlay - it needs no extended hit area below the
    // bar the way Exit does.
    //
    // "Zoom:" label to its left, both centred as ONE pair - operator,
    // 2026-09-13: "no label next to it to tell what it does like: 'Zoom:'".
    // A bare dropdown reading "Fit" names its CURRENT value, not what the
    // control IS, which is exactly the ambiguity every other labelled control
    // in this app avoids.
    #define ZOOM_DD_W    160
    #define ZOOM_LBL_W    86
    #define ZOOM_GAP       8
    lv_obj_t *zoom_lbl = lv_label_create(hdr);
    lv_label_set_text(zoom_lbl, "Zoom:");
    lv_obj_set_style_text_font(zoom_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(zoom_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(zoom_lbl, LV_ALIGN_LEFT_MID, ZOOM_X, 0);

    lv_obj_t *zoom_dd = lv_dropdown_create(hdr);
    lv_dropdown_set_options(zoom_dd, "Fit\nx1\nx2\nx3\nx4\nx5\nx10\nx20\nx50");
    lv_obj_set_size(zoom_dd, ZOOM_DD_W, 46);
    lv_obj_align(zoom_dd, LV_ALIGN_TOP_LEFT, ZOOM_X + ZOOM_LBL_W + ZOOM_GAP, (HEADER_H - 46) / 2);
    lv_obj_set_style_text_font(zoom_dd, &lv_font_montserrat_24, 0);
    lv_dropdown_set_selected(zoom_dd, 0);   /* "Fit" - what a fresh open already does */
    lv_obj_add_event_cb(zoom_dd, zoom_dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(zoom_dd, zoom_dropdown_open_cb, LV_EVENT_CLICKED, NULL);
    s_zoom_dd = zoom_dd;   // tabview_changed_cb greys this out off the MAP tab

    // Callsign/freq/UTC info line - see update_info_line()'s own header
    // comment. Sits in the free space between the Zoom group and Exit
    // (there is no room for a second row within HEADER_H's 64 px without
    // growing it, but there is horizontal slack here: title ends well
    // before the centred Zoom group at x=767, and Exit starts around
    // x=1141 - see the constants below).
    //
    // ⛔ FIRST VERSION used lv_obj_align_to(..., exit_btn, LV_ALIGN_OUT_LEFT_MID,
    // ...) with the label's text set AFTER the align call, on an initially-EMPTY
    // label. lv_obj_align_to() computes its offset from the object's size AT THE
    // MOMENT OF THE CALL, so it anchored against a near-zero-width box; setting
    // the real (much wider) text afterwards only grew the box to the right from
    // that same top-left corner - landing it UNDER Exit instead of left of it
    // (operator screenshot, 2026-09-17: text fragments visible peeking out from
    // behind the Exit button). Fixed by giving this a FIXED width and centring
    // text within it, positioned at a fixed gap between the two neighbours
    // instead of measuring either one at build time - same width regardless of
    // whether the callsign is set, so it cannot drift again if the content's
    // own size changes later. LV_ALIGN_LEFT_MID centres it vertically in the
    // header for free (this is a direct child of hdr, whose height IS
    // HEADER_H, so "centred on the banner" falls out of that alignment
    // without a separate y calculation).
    // ⛔ THIS WIDTH IS DERIVED FROM THE FLUSH BUTTON, not chosen. Uwe DL8UG's
    // header Flush button (added 2026-09-17) lands at x=989..1126 - measured on
    // a live screenshot, and it matches its own geometry: right-aligned at -153
    // in a 1280 px header, 138 px wide. At the previous width of 340 this label
    // ran to 1120 and the button was drawn straight over its right-hand third,
    // clipping the callsign and the time - which is the whole content Gyula
    // HA3HZ asked for in v1.14.2 ("a screenshot with no callsign/time/freq in
    // it doesn't convey much information"). One contributor's feature silently
    // ate another's, and only a screenshot with a callsign SET shows it, which
    // is why it reached me and not him.
    //
    // 193 = 989 (Flush's left edge) - 16 (gap) - 780 (this label's own x). Move
    // the button and this must move with it.
    /* INFO_GAP_X / INFO_GAP_W come from the header budget above. */
    lv_obj_t *info_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(info_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(info_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info_lbl, INFO_GAP_W);
    /* Wrap, never clip - the same rule the Calibrate Power modal needed on the
     * same day. A long compound callsign must push a line, not lose one. */
    lv_label_set_long_mode(info_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(info_lbl, LV_ALIGN_LEFT_MID, INFO_GAP_X, 0);
    s_info_lbl = info_lbl;
    update_info_line();

    build_settings_drawer(s_overlay);
    build_settings_scrim(s_overlay);
    build_settings_edge_strip(s_overlay);

    lv_obj_t *tv = lv_tabview_create(s_overlay);
    s_tabview = tv;   // map_pinch_poll_cb() needs to know when MAP is the visible tab
    lv_obj_add_event_cb(tv, tabview_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* ⛔ AND RUN IT ONCE NOW. VALUE_CHANGED has not fired yet, but MAP is
     * already the tab on screen - so without this the FIRST swipe off the map
     * still worked and the fix only took effect after the operator had changed
     * tab by hand at least once. Same shape as the top-bar bug this project has
     * now had three times: a state that must hold for a whole mode belongs in
     * something re-derived, and the entry path with no transition is the one
     * that gets missed. */
    tabview_changed_cb(NULL);
    /* ⛔ FULL WIDTH NOW - the checkbox sidebar is gone (build_settings_drawer()
     * above is a separate, hidden-by-default panel reached by the edge swipe),
     * so lv_tabview's OWN tab bar, moved to the left, is the only thing
     * occupying that space. See LV_DIR_LEFT below. */
    lv_obj_set_pos(tv, 0, HEADER_H);
    lv_obj_set_size(tv, SCR_W, SCR_H - HEADER_H);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x0a0d10), 0);
    /* Operator, 2026-09-13: "move the buttons MAP LIST and CONDITIONS to the
     * now empty left panel as buttons - freeing up that space they occupied
     * for map estate." lv_tabview supports this natively - LV_DIR_LEFT makes
     * its own tab bar a left-side column (lv_tabview.c stacks the buttons
     * COLUMN-flow automatically for LEFT/RIGHT), so this reuses the existing
     * tab-switching machinery instead of hand-rolling three buttons plus a
     * manual page-swap. Must be set BEFORE lv_tabview_set_tab_bar_size(),
     * which sizes WIDTH for a horizontal position and HEIGHT for a vertical
     * one and reads tab_pos to know which. */
    lv_tabview_set_tab_bar_position(tv, LV_DIR_LEFT);
    lv_tabview_set_tab_bar_size(tv, TAB_BAR_W);

    lv_obj_t *tab_map = lv_tabview_add_tab(tv, "MAP");
    lv_obj_set_style_pad_all(tab_map, 0, 0);
    lv_obj_clear_flag(tab_map, LV_OBJ_FLAG_SCROLLABLE);

    // Created FIRST so it draws BEHIND s_map_obj - the world outline, redrawn
    // only on an actual zoom/pan change, never on a routine spot update. See
    // the split comment above map_bg_draw_cb. Not clickable: s_map_obj sits
    // in the exact same area and is checked first by LVGL's hit-test (reverse
    // creation order), so this never needs to see a touch.
    s_map_bg_obj = lv_canvas_create(tab_map);   // buffer attached on first render - see map_cache_rebuild()
    lv_obj_set_pos(s_map_bg_obj, 0, 0);
    lv_obj_clear_flag(s_map_bg_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_map_bg_obj, LV_OBJ_FLAG_CLICKABLE);

    s_map_obj = lv_obj_create(tab_map);
    lv_obj_remove_style_all(s_map_obj);
    lv_obj_set_size(s_map_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_map_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_map_obj, map_spots_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_map_obj, map_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_map_obj, map_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_map_obj, map_drag_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *tab_table = lv_tabview_add_tab(tv, "LIST");
    lv_obj_set_flex_flow(tab_table, LV_FLEX_FLOW_COLUMN);
    s_table_list = tab_table;

    // Rightmost, per explicit request - lv_tabview_add_tab() appends, so
    // creation order is left-to-right order.
    // "CONDITIONS" wrapped ugly in the TAB_BAR_W column (2 lines, hyphenated
    // mid-word - "CONDITIO"/"NS"). PROP is the standard ham shorthand for
    // propagation - exactly what this tab shows (band conditions + solar/
    // geomagnetic) - and short enough to sit on one line like MAP/LIST.
    lv_obj_t *tab_cond = lv_tabview_add_tab(tv, "PROP");
    build_conditions_tab(tab_cond);

    // Tab bar colouring, to match the rest of the app's palette rather than
    // LVGL's stock grey - lv_tabview_add_tab() builds each tab as a plain
    // lv_button+lv_label pair (not a buttonmatrix, see lv_tabview.c), so the
    // two buttons are styled directly by index rather than through a single
    // tabview-wide style. UI_COLOR_PRIMARY for the active tab is the same
    // "this is the selected thing" blue every other segmented control in
    // this app uses (the drawer, WSPR's band buttons, ...).
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tv);
    /* Operator, 2026-09-12: "Please increase the text font of the taps (MAP
     * LIST CONDITIONS)" - montserrat_20 was the same undersized default this
     * whole panel shipped with.
     *
     * ⛔ 2026-09-13: the bar MOVED from a thin strip along the TOP to a
     * TAB_BAR_W (140 px) column down the LEFT - the operator's own next
     * request ("move the buttons MAP LIST and CONDITIONS to the now empty
     * left panel"). "CONDITIONS" at montserrat_28 is ~180 px wide, wider than
     * the whole column, so it is wrapped to two lines at montserrat_22
     * instead of clipping - this needs each button's own LABEL child, not
     * just the button, since long_mode/width are label properties.
     *
     * No explicit set_tab_bar_size(HEIGHT) call here any more - it was
     * already done in WIDTH terms right after LV_DIR_LEFT was set, above;
     * calling it again with a height would silently overwrite that width. */
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_border_side(tab_bar, LV_BORDER_SIDE_RIGHT, 0);   /* was BOTTOM, for a top strip */
    lv_obj_set_style_border_width(tab_bar, 1, 0);
    lv_obj_set_style_border_color(tab_bar, lv_color_hex(UI_COLOR_BORDER), 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(tab_bar); i++) {
        lv_obj_t *btn = lv_obj_get_child(tab_bar, i);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_PRIMARY), LV_STATE_CHECKED);
        /* ⚠ TEXT COLOUR STAYS ON THE BUTTON, NOT THE LABEL. A state selector
         * (LV_STATE_CHECKED) only ever matches the object it is set ON - the
         * label itself never becomes "checked", only the button does. Setting
         * it here works because text_color is an INHERITABLE property: LVGL
         * resolves it against the BUTTON's real state and the label inherits
         * that resolved value. Putting it on the label instead would silently
         * never apply the checked colour - caught before it shipped, not
         * after; almost repeated the exact bug this file's own "one colour
         * describing something it is not" fixes elsewhere warn against. */
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_TEXT), LV_STATE_CHECKED);
        lv_obj_t *btn_lbl = lv_obj_get_child(btn, 0);
        if (btn_lbl) {
            lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_22, 0);
            lv_obj_set_style_text_align(btn_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(btn_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(btn_lbl, TAB_BAR_W - 20);
        }
    }

    lv_obj_move_foreground(hdr);
    lv_obj_move_foreground(exit_btn);
    lv_obj_move_foreground(flush_hdr_btn);   // same reason as exit_btn just above - built
                                              // before the tabview, so without this the
                                              // tabview content draws OVER it and it's
                                              // invisible - caught on hardware (operator:
                                              // "ich sehe den button nicht"), not by reading
                                              // the code, exactly like the exit_btn/hdr case
                                              // the comment below already describes.
    /* The settings drawer + its edge strip must sit above the tabview content
     * for the same reason hdr/exit_btn do - built after it, so without this
     * they would be UNDER it in the child list and lose every hit test. The
     * strip has to win the touch; the drawer has to win the DRAW, since it
     * covers part of the map when open. */
    lv_obj_move_foreground(s_settings_strip);
    // Closed-state breathing grip: above the tabview, BELOW the drawer, so the
    // drawer covers it when open - same order ui_init() uses for s_burger_btn.
    build_settings_grip(s_overlay);
    lv_obj_move_foreground(s_settings_drawer);

    // 1s poll, but see refresh_timer_cb: it only pays for a redraw when the
    // underlying data actually changed.
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);
    lv_timer_pause(s_refresh_timer);

    // Renders the cached map at most once per frame, only when marked dirty.
    s_map_cache_timer = lv_timer_create(map_cache_timer_cb, 33, NULL);
    lv_timer_pause(s_map_cache_timer);

    // 40 ms poll for the MAP tab's two-finger pinch-zoom - same cadence as
    // ui.c's own pinch_poll_cb() (50 ms), close enough that a pinch feels
    // live without the raw-touch read costing anything while paused.
    s_map_touch = bsp_display_get_touch_handle();
    s_pinch_timer = lv_timer_create(map_pinch_poll_cb, 40, NULL);
    lv_timer_pause(s_pinch_timer);

    /* ⛔ THE FEEDS RUN FROM BOOT NOW - operator, 2026-09-13: "The list starts
     * populating like 3min after opening the map.... how about opening and
     * sync the list as soon as the whole app boots?" This REVERSES the same
     * day's "free the RAM while not on SelfSpotter" rule, on his instruction,
     * and it has to: PSK Reporter and RBN are live pushes with NO history, so
     * a session opened when the map opens can only ever show what arrives
     * afterwards. Collecting from boot is the only way the list is already
     * full when he looks. Cost: the MQTT client task (internal RAM, see
     * pskr_self.c) for every unit, all the time. show()/hide() no longer
     * touch this. */
    settings_set_spotmap_en(true);

    ESP_LOGI(TAG, "init");
}

void spot_map_view_show(void)
{
    if (!s_overlay) return;
    /* ⛔ THIS IS THE SWITCH NOW - THERE IS NO CHECKBOX LEFT TO GATE ON.
     *
     * Operator, 2026-09-13: "The Spot Map checkbox in all other Drawers
     * should be deleted and instead implement the following: Whenever the
     * user is not on the SelfSpotter it will free up ram usage as if the
     * former Spot map was UNCHECKED. Then when entering SelfSpotter it of
     * course act like it WAS checked."
     *
     * So this call turns the three feeds (MQTT to PSK Reporter, RBN telnet,
     * the wsprnet poller) on itself, rather than refusing to open because
     * something else had not been turned on first - see settings.h's
     * spotmap_en for what reads this. spot_map_view_hide() is the exact
     * mirror image, below. */
    s_sig_rbn_n = s_sig_psk_n = s_sig_wspr_n = -1;   // force a redraw on this open
    s_sig_rbn_t = s_sig_psk_t = s_sig_wspr_t = -1;
    refresh_own_position();
    refresh_now();
    // Never reopen pre-zoomed/pre-panned from a forgotten previous session -
    // map_fit_to_spots() resets both before deciding, then frames whatever is
    // actually there rather than handing over an empty planet.
    s_view_is_users = false;   /* a fresh open is the map's view again */
    map_fit_to_spots();
    s_map_pinch_active = false;
    s_map_drag_active = false;
    // The settings drawer (right-edge swipe) always starts CLOSED on a fresh
    // open too - same reasoning as the zoom/pan reset just above: nothing
    // about a previous session should linger into this one. settings_close()
    // rather than a bare flag set, so the scrim resets with it.
    if (s_settings_drawer) settings_close();
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    // Raise it. Built once at init, so anything created/foregrounded after
    // that (every screen mode's own containers) sits above it in the
    // screen's child list otherwise - the exact WSPR/Reader drawing bug
    // CLAUDE.md records under "LVGL hit-tests children in reverse creation
    // order". An overlay has to raise itself every time it is shown.
    lv_obj_move_foreground(s_overlay);
    s_active = true;
    if (s_refresh_timer) lv_timer_resume(s_refresh_timer);
    if (s_pinch_timer) lv_timer_resume(s_pinch_timer);
    map_mark_dirty();
    if (s_map_cache_timer) lv_timer_resume(s_map_cache_timer);
    ui_help_overlay_changed();   // stand the top bar and edge swipes down
    ESP_LOGI(TAG, "show");
}

void spot_map_view_hide(void)
{
    if (!s_overlay) return;
    s_active = false;
    if (s_refresh_timer) lv_timer_pause(s_refresh_timer);
    if (s_pinch_timer) lv_timer_pause(s_pinch_timer);
    if (s_map_cache_timer) lv_timer_pause(s_map_cache_timer);
    /* Give the ~1.5 MB cache back while nobody is looking - PSRAM free was
     * measured at 1.9 MB with it held. Rebuilt on the next show(). Detach the
     * image source BEFORE freeing, so nothing can draw from freed memory. */
    if (s_map_cache_buf) {
        lv_image_set_src(s_map_bg_obj, NULL);
        heap_caps_free(s_map_cache_buf);
        s_map_cache_buf = NULL;
        s_map_cache_w = s_map_cache_h = s_map_cache_stride = 0;
        s_map_dirty = true;
    }
    if (s_scan_x) { heap_caps_free(s_scan_x); s_scan_x = NULL; }
    if (s_scan_n) { heap_caps_free(s_scan_n); s_scan_n = NULL; }
    ui_help_overlay_changed();   // hand the top bar and edge swipes back
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "hide");
}

bool spot_map_view_is_active(void) { return s_active; }
int  spot_map_view_spot_count(void) { return s_spot_snap_n; }
