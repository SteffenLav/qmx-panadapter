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
#include "ui.h"                 // ui_help_overlay_changed()
#include "net/rbn.h"
#include "net/pskr_self.h"
#include "net/wspr_self.h"
#include "net/band_conditions.h"
#include "util/world_map_data.h"
#include "util/maidenhead.h"
#include "util/format_freq.h"
#include "storage/settings.h"
#include "adif/adif_log.h"      // adif_log_band_for_freq() - the ONE band table, see its own comment

#include "esp_log.h"
#include "esp_attr.h"           // EXT_RAM_BSS_ATTR
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
#define SIDEBAR_W  220

static lv_obj_t *s_overlay    = NULL;
static lv_obj_t *s_map_obj    = NULL;   // Karte tab: custom-drawn world map + self-spot lines
static lv_obj_t *s_table_list = NULL;   // Tabelle tab: scrollable row list
static lv_obj_t *s_grid_warn  = NULL;   // "set my_grid" notice, shown when it's empty
static lv_obj_t *s_tabview    = NULL;   // so map_pinch_poll_cb() can tell MAP is the visible tab
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
static float s_map_zoom = 1.0f;
static float s_map_pan_dx = 0.0f, s_map_pan_dy = 0.0f;
#define MAP_ZOOM_MIN 1.0f
#define MAP_ZOOM_MAX 8.0f

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

// Refreshes s_have_me/s_my_lat/s_my_lon from storage/settings.h's my_grid.
// Called once per gather, not cached across calls - a grid the operator just
// typed in should take effect on the very next refresh tick.
static void refresh_own_position(void)
{
    qmx_settings_t s;
    settings_load_all(&s);
    s_have_me = s.my_grid[0] && maidenhead_to_latlon(s.my_grid, &s_my_lat, &s_my_lon);
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
 *   - zoom so the bounding box of every drawn point spans MAP_FIT_FRACTION of
 *     the view rather than all of it, leaving a margin so dots near the edge
 *     are not clipped;
 *   - pan so that box ends up CENTRED, because the anchor is our QTH and not
 *     the middle of the screen - without this, zooming on a European station
 *     pushes everything off the top.
 *
 * Only ever called when the map is opened. It must not run on the refresh
 * timer: the operator pinches and drags this map, and a view that re-fitted
 * itself underneath them every time a spot arrived would be unusable. */
#define MAP_FIT_FRACTION 0.72f     /* of the view the spots may occupy */
#define MAP_FIT_MAX_ZOOM 12.0f     /* a single nearby spot must not fill the world */

static void map_fit_to_spots(void)
{
    static self_spot_t spots[SELF_SPOT_MAX];
    int count = gather_self_spots(spots, SELF_SPOT_MAX);

    s_map_zoom   = 1.0f;
    s_map_pan_dx = s_map_pan_dy = 0.0f;
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

    const float spanx = x1 - x0, spany = y1 - y0;
    float zx = (spanx > 0.0001f) ? (MAP_FIT_FRACTION / spanx) : MAP_FIT_MAX_ZOOM;
    float zy = (spany > 0.0001f) ? (MAP_FIT_FRACTION / spany) : MAP_FIT_MAX_ZOOM;
    float z  = (zx < zy) ? zx : zy;
    if (z > MAP_FIT_MAX_ZOOM) z = MAP_FIT_MAX_ZOOM;
    if (z < 1.0f) z = 1.0f;                 /* project() only zooms in */

    s_map_zoom = z;

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
    if (s_map_obj) lv_obj_invalidate(s_map_obj);
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

static void map_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int32_t w = lv_area_get_width(&area);
    int32_t h = lv_area_get_height(&area);
    if (w <= 0 || h <= 0) return;

    // World outline. LVGL 9.2.2's lv_draw_line_dsc_t is a single segment
    // (p1/p2), unlike the multi-point polyline descriptor newer LVGL versions
    // have, so each ring edge (including the closing edge back to point 0) is
    // its own draw call rather than one call per ring.
    lv_draw_line_dsc_t land_dsc;
    lv_draw_line_dsc_init(&land_dsc);
    land_dsc.color = lv_palette_darken(LV_PALETTE_GREY, 2);
    land_dsc.width = 1;

    for (int i = 0; i < WORLD_MAP_RING_COUNT; i++) {
        const world_map_ring_t *ring = &WORLD_MAP_RINGS[i];
        int n = ring->point_count;
        if (n < 2) continue;
        lv_point_precise_t prev = project(&area, w, h, ring->points[0] / 10.0f, ring->points[1] / 10.0f);
        for (int j = 1; j <= n; j++) {
            int k = (j % n) * 2;   // wraps to 0 on the last iteration - closes the ring
            lv_point_precise_t cur = project(&area, w, h, ring->points[k] / 10.0f, ring->points[k + 1] / 10.0f);
            land_dsc.p1 = prev;
            land_dsc.p2 = cur;
            lv_draw_line(layer, &land_dsc);
            prev = cur;
        }
    }

    if (!s_have_me) return;   // nothing to draw a line FROM - the sidebar/table already say so

    // EXT_RAM_BSS_ATTR, not a plain static - same lesson as the rbn[]/psk[]
    // scratch buffers in gather_self_spots() above, just missed the first
    // time round because this array lives in this function instead. At
    // SELF_SPOT_MAX now 300 (three 100-entry sources) a plain static here
    // would be ~18 KB of internal .bss, PER call site, TWO call sites - the
    // exact class of self-inflicted internal-RAM exhaustion that broke MQTT
    // reconnects for 2026-09-11's whole session.
    static EXT_RAM_BSS_ATTR self_spot_t spots[SELF_SPOT_MAX];
    int count = gather_self_spots(spots, SELF_SPOT_MAX);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.width = 2;
    line_dsc.opa = LV_OPA_80;

    lv_draw_rect_dsc_t dot_dsc;
    lv_draw_rect_dsc_init(&dot_dsc);
    dot_dsc.radius = LV_RADIUS_CIRCLE;
    dot_dsc.bg_opa = LV_OPA_COVER;

    for (int i = 0; i < count; i++) {
        const self_spot_t *sp = &spots[i];
        if (!sp->has_pos || !passes_filter(sp)) continue;

        line_dsc.color = lv_color_hex(source_color(sp->src));
        draw_great_circle(layer, &line_dsc, &area, w, h, (float)s_my_lon, (float)s_my_lat, sp->lon, sp->lat);

        lv_point_precise_t p = project(&area, w, h, sp->lon, sp->lat);
        dot_dsc.bg_color = lv_color_hex(source_color(sp->src));
        lv_area_t dot_area = { p.x - 3, p.y - 3, p.x + 3, p.y + 3 };
        lv_draw_rect(layer, &dot_dsc, &dot_area);
    }

    // Own position, drawn last so it always sits on top of every line.
    lv_point_precise_t home = project(&area, w, h, (float)s_my_lon, (float)s_my_lat);
    dot_dsc.bg_color = lv_color_hex(UI_COLOR_ACCENT_GOLD);
    lv_area_t home_area = { home.x - 5, home.y - 5, home.x + 5, home.y + 5 };
    lv_draw_rect(layer, &dot_dsc, &home_area);
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
        return;
    }

    float zoom = s_map_pinch_start_zoom * ((float)dist / (float)s_map_pinch_start_dist);
    if (zoom < MAP_ZOOM_MIN) zoom = MAP_ZOOM_MIN;
    if (zoom > MAP_ZOOM_MAX) zoom = MAP_ZOOM_MAX;
    if (fabsf(zoom - s_map_zoom) > 0.01f) {
        s_map_zoom = zoom;
        if (s_map_obj) lv_obj_invalidate(s_map_obj);
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

static void map_drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_map_drag_active = false;
        return;
    }
    if (code != LV_EVENT_PRESSING) return;
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

    s_map_pan_dx = s_map_drag_start_pan_dx + (float)(p.x - s_map_drag_start_pt.x) / (float)w;
    s_map_pan_dy = s_map_drag_start_pan_dy + (float)(p.y - s_map_drag_start_pt.y) / (float)h;
    lv_obj_invalidate(s_map_obj);
}

// ---- Tabelle tab --------------------------------------------------------

#define COL_GAP 10
#define TABLE_MAX_ROWS SELF_SPOT_MAX   // the buffer is the limit, nothing clips it further

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

static void add_col(lv_obj_t *row, const char *text, int grow, uint32_t color, bool bold)
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
}

// LIST tab column sort: tap a header to cycle unsorted -> ascending ->
// descending -> unsorted for that column; tapping a DIFFERENT column while
// one is active starts that one fresh at ascending, matching the common
// spreadsheet/file-manager convention rather than remembering a per-column
// direction.
typedef enum {
    SORT_COL_NONE = 0,
    SORT_COL_CALL, SORT_COL_MODE, SORT_COL_BAND,
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
    case SORT_COL_MODE: cmp = strcasecmp(a->mode, b->mode); break;
    // Band has no numeric value of its own (it's a name derived from
    // frequency, adif_log_band_for_freq()) - sorting on the underlying
    // frequency gives the natural band order for free and needs no second
    // band-name-to-rank table to maintain.
    case SORT_COL_BAND:
    case SORT_COL_FREQ: cmp = (a->freq_hz    > b->freq_hz)    - (a->freq_hz    < b->freq_hz); break;
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
static void add_sort_header_col(lv_obj_t *row, const char *text, int grow, sort_col_t col_id)
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
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(active ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED), 0);

    lv_obj_add_event_cb(cell, header_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)col_id);
}

static void rebuild_table(void)
{
    if (!s_table_list) return;
    lv_obj_clean(s_table_list);

    lv_obj_t *hdr = make_row(s_table_list);
    add_sort_header_col(hdr, "RX",       2, SORT_COL_CALL);
    add_sort_header_col(hdr, "Mode",     1, SORT_COL_MODE);
    add_sort_header_col(hdr, "Band",     1, SORT_COL_BAND);
    add_sort_header_col(hdr, "Freq",     2, SORT_COL_FREQ);
    add_sort_header_col(hdr, "SNR",      1, SORT_COL_SNR);
    add_sort_header_col(hdr, "Distance", 1, SORT_COL_DIST);
    add_sort_header_col(hdr, "Age",      1, SORT_COL_AGE);

    static EXT_RAM_BSS_ATTR self_spot_t spots[SELF_SPOT_MAX];   // NOT internal .bss - see the note above map_draw_cb()'s copy of this array
    int count = gather_self_spots(spots, SELF_SPOT_MAX);
    if (s_sort_col != SORT_COL_NONE) qsort(spots, (size_t)count, sizeof(spots[0]), cmp_spots);
    int64_t now = (int64_t)time(NULL);

    int shown = 0;
    for (int i = 0; i < count && shown < TABLE_MAX_ROWS; i++) {
        const self_spot_t *sp = &spots[i];
        if (!passes_filter(sp)) continue;

        char freq_buf[16], age_buf[24], snr_buf[8], dist_buf[16];
        format_freq_hz(sp->freq_hz, g_freq_style, freq_buf, sizeof(freq_buf));
        format_age(sp->heard_unix, now, age_buf, sizeof(age_buf));
        snprintf(snr_buf, sizeof(snr_buf), "%d dB", sp->snr_db);
        if (sp->distance_km >= 0) snprintf(dist_buf, sizeof(dist_buf), "%ld km", (long)sp->distance_km);
        else                      snprintf(dist_buf, sizeof(dist_buf), "-");
        // The ONE band table (adif_log_band_for_freq(), adif_log.c) - do not
        // reimplement this locally, see that function's own comment.
        const char *band = adif_log_band_for_freq(sp->freq_hz);

        lv_obj_t *row = make_row(s_table_list);
        uint32_t col = source_color(sp->src);
        add_col(row, sp->call[0] ? sp->call : "-", 2, col, true);
        add_col(row, sp->mode[0] ? sp->mode : "-", 1, col, false);
        add_col(row, band[0] ? band : "-", 1, UI_COLOR_TEXT, false);
        add_col(row, freq_buf, 2, UI_COLOR_TEXT, false);
        add_col(row, snr_buf, 1, UI_COLOR_TEXT, false);
        add_col(row, dist_buf, 1, UI_COLOR_TEXT_SECONDARY, false);
        add_col(row, age_buf, 1, UI_COLOR_TEXT_SECONDARY, false);
        shown++;
    }

    if (shown == 0) {
        lv_obj_t *row = make_row(s_table_list);
        add_col(row, "Nobody has heard me yet (CW/Digi/WSPR).", 1, UI_COLOR_TEXT_MUTED, false);
    }
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
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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

static void refresh_now(void)
{
    if (s_map_obj) lv_obj_invalidate(s_map_obj);
    rebuild_table();
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

static lv_obj_t *add_filter_checkbox(lv_obj_t *parent, const char *label, uint32_t accent, lv_event_cb_t cb)
{
    lv_obj_t *box = lv_checkbox_create(parent);
    lv_checkbox_set_text(box, label);
    lv_obj_add_state(box, LV_STATE_CHECKED);
    lv_obj_set_style_text_font(box, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(box, lv_color_hex(accent), 0);
    lv_obj_set_style_text_color(box, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_add_event_cb(box, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return box;
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

static void build_sidebar(lv_obj_t *parent)
{
    lv_obj_t *sb = lv_obj_create(parent);
    lv_obj_set_size(sb, SIDEBAR_W, SCR_H - HEADER_H);
    lv_obj_set_pos(sb, 0, HEADER_H);
    lv_obj_set_style_bg_color(sb, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(sb, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_pad_all(sb, 14, 0);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(sb, 10, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(sb);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_label_set_text(lbl, "Source");

    add_filter_checkbox(sb, "CW (RBN)",    UI_COLOR_MODE_CW,   cw_cb);
    add_filter_checkbox(sb, "Digi (PSKR)", UI_COLOR_MODE_DIGI, digi_cb);
    add_filter_checkbox(sb, "WSPR",        UI_COLOR_MODE_WSPR, wspr_cb);

    s_grid_warn = lv_label_create(sb);
    lv_obj_set_style_text_font(s_grid_warn, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_grid_warn, lv_color_hex(UI_COLOR_DANGER_BORDER), 0);
    lv_label_set_long_mode(s_grid_warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_grid_warn, SIDEBAR_W - 28);
    lv_obj_set_style_pad_top(s_grid_warn, 10, 0);
    lv_label_set_text(s_grid_warn, "");   // filled in by refresh, see refresh_timer_cb

    // A one-child row of its own, centered - the sidebar's own flex cross-
    // align is START (so the checkboxes/labels above hug the left edge),
    // and that applies to every direct child alike. Centering just this one
    // button needs its own centered flex row rather than fighting the
    // sidebar's container-wide alignment.
    lv_obj_t *flush_wrap = lv_obj_create(sb);
    lv_obj_remove_style_all(flush_wrap);
    lv_obj_set_size(flush_wrap, SIDEBAR_W - 28, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(flush_wrap, 14, 0);
    lv_obj_set_flex_flow(flush_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(flush_wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(flush_wrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *flush_btn = lv_button_create(flush_wrap);
    lv_obj_set_style_bg_color(flush_btn, lv_color_hex(UI_COLOR_DANGER), 0);
    lv_obj_set_style_pad_hor(flush_btn, 16, 0);
    lv_obj_set_height(flush_btn, 42);
    lv_obj_add_event_cb(flush_btn, flush_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *flush_lbl = lv_label_create(flush_btn);
    lv_obj_set_style_text_font(flush_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(flush_lbl, "Flush");
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

    refresh_own_position();

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
        refresh_now();
    }

    if (s_grid_warn) {
        lv_label_set_text(s_grid_warn, s_have_me ? "" :
            "No home grid square set (Settings -> My Grid) - the map cannot draw any lines.");
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

    lv_obj_t *title = lv_label_create(hdr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);
    lv_label_set_text(title, "SELFSPOTTER");

    // Exit is a child of the OVERLAY, not the header bar, so its extended hit
    // area can reach below the 64 px bar - same reasoning as reader_view.c's
    // header buttons (LVGL clips a child's hit area to its parent).
    lv_obj_t *exit_btn = lv_button_create(s_overlay);
    lv_obj_align(exit_btn, LV_ALIGN_TOP_RIGHT, -24, (HEADER_H - 46) / 2);
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

    build_sidebar(s_overlay);

    lv_obj_t *tv = lv_tabview_create(s_overlay);
    s_tabview = tv;   // map_pinch_poll_cb() needs to know when MAP is the visible tab
    lv_obj_set_pos(tv, SIDEBAR_W, HEADER_H);
    lv_obj_set_size(tv, SCR_W - SIDEBAR_W, SCR_H - HEADER_H);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x0a0d10), 0);

    lv_obj_t *tab_map = lv_tabview_add_tab(tv, "MAP");
    lv_obj_set_style_pad_all(tab_map, 0, 0);
    lv_obj_clear_flag(tab_map, LV_OBJ_FLAG_SCROLLABLE);
    s_map_obj = lv_obj_create(tab_map);
    lv_obj_remove_style_all(s_map_obj);
    lv_obj_set_size(s_map_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_map_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_map_obj, map_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_map_obj, map_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_map_obj, map_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_map_obj, map_drag_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *tab_table = lv_tabview_add_tab(tv, "LIST");
    lv_obj_set_flex_flow(tab_table, LV_FLEX_FLOW_COLUMN);
    s_table_list = tab_table;

    // Rightmost, per explicit request - lv_tabview_add_tab() appends, so
    // creation order is left-to-right order.
    lv_obj_t *tab_cond = lv_tabview_add_tab(tv, "CONDITIONS");
    build_conditions_tab(tab_cond);

    // Tab bar colouring, to match the rest of the app's palette rather than
    // LVGL's stock grey - lv_tabview_add_tab() builds each tab as a plain
    // lv_button+lv_label pair (not a buttonmatrix, see lv_tabview.c), so the
    // two buttons are styled directly by index rather than through a single
    // tabview-wide style. UI_COLOR_PRIMARY for the active tab is the same
    // "this is the selected thing" blue every other segmented control in
    // this app uses (the drawer, WSPR's band buttons, ...).
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_border_side(tab_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(tab_bar, 1, 0);
    lv_obj_set_style_border_color(tab_bar, lv_color_hex(UI_COLOR_BORDER), 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(tab_bar); i++) {
        lv_obj_t *btn = lv_obj_get_child(tab_bar, i);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_PRIMARY), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_TEXT), LV_STATE_CHECKED);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
    }

    lv_obj_move_foreground(hdr);
    lv_obj_move_foreground(exit_btn);

    // 1s poll, but see refresh_timer_cb: it only pays for a redraw when the
    // underlying data actually changed.
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);
    lv_timer_pause(s_refresh_timer);

    // 40 ms poll for the MAP tab's two-finger pinch-zoom - same cadence as
    // ui.c's own pinch_poll_cb() (50 ms), close enough that a pinch feels
    // live without the raw-touch read costing anything while paused.
    s_map_touch = bsp_display_get_touch_handle();
    s_pinch_timer = lv_timer_create(map_pinch_poll_cb, 40, NULL);
    lv_timer_pause(s_pinch_timer);

    ESP_LOGI(TAG, "init");
}

void spot_map_view_show(void)
{
    if (!s_overlay) return;
    // Opt-in (settings.h, spotmap_en), so with it off there is nothing behind
    // this gesture - every feed is idle and the map would be a blank world with
    // no explanation. Say where the switch is instead of opening an empty one.
    if (!settings_get_spotmap_en()) {
        ui_toast("Spot map is off - Settings, Network, \"Spot map\"");
        return;
    }
    s_sig_rbn_n = s_sig_psk_n = s_sig_wspr_n = -1;   // force a redraw on this open
    s_sig_rbn_t = s_sig_psk_t = s_sig_wspr_t = -1;
    refresh_own_position();
    refresh_now();
    // Never reopen pre-zoomed/pre-panned from a forgotten previous session -
    // map_fit_to_spots() resets both before deciding, then frames whatever is
    // actually there rather than handing over an empty planet.
    map_fit_to_spots();
    s_map_pinch_active = false;
    s_map_drag_active = false;
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
    ui_help_overlay_changed();   // stand the top bar and edge swipes down
    ESP_LOGI(TAG, "show");
}

void spot_map_view_hide(void)
{
    if (!s_overlay) return;
    s_active = false;
    if (s_refresh_timer) lv_timer_pause(s_refresh_timer);
    if (s_pinch_timer) lv_timer_pause(s_pinch_timer);
    ui_help_overlay_changed();   // hand the top bar and edge swipes back
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "hide");
}

bool spot_map_view_is_active(void) { return s_active; }
