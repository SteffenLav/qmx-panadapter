// Diagram renderer for the on-device manual. Contract and rationale in the
// header - read that first; this file is the mechanics.

#include "reader_diagram.h"
#include "ui_theme.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Palette. Named in the spec so a diagram never carries a hex value, which
// means the colours stay in step with the app if the theme moves. These are
// the app's own: the EVEN-row blue, the ODD-row / VFO amber, the spectrum
// trace green, the heading gold.
typedef struct { const char *name; uint32_t rgb; } dcolor_t;
static const dcolor_t s_colors[] = {
    { "steel", 0x8AB4F8 },
    { "amber", 0xFFA040 },
    { "trace", 0x4CAF6A },
    { "gold",  0xE9C46A },
    { "dim",   0x93A4B1 },
    { "red",   0xE06A5A },
};
#define DIAG_SURFACE  0x151B23
#define DIAG_RAISED   0x1B232D
#define DIAG_LINE     0x2A3440
#define DIAG_INK      0xE6EDF3
#define DIAG_DIM      0x93A4B1
// One fill strength for BOTH timeline bands and legend chips. They were 30% and
// 100% respectively, so the colours did not map 1:1 and the legend was only
// approximately about the diagram (operator, 2026-08-08). An overlay mark is
// told apart by its geometry - a stripe along the bottom - not by being darker.
#define DIAG_FILL_OPA LV_OPA_50

static uint32_t color_by_name(const char *n)
{
    if (n) {
        for (size_t i = 0; i < sizeof(s_colors) / sizeof(s_colors[0]); i++)
            if (strncasecmp(n, s_colors[i].name, strlen(s_colors[i].name)) == 0)
                return s_colors[i].rgb;
    }
    return DIAG_DIM;
}

// ---- small text helpers ----------------------------------------------------

static const char *skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

// Trim in place; returns the (possibly advanced) start.
static char *trim(char *s)
{
    char *p = (char *)skip_ws(s);
    size_t n = strlen(p);
    while (n && (p[n-1] == ' ' || p[n-1] == '\t' || p[n-1] == '\r')) p[--n] = '\0';
    return p;
}

// "key: value" -> returns value if the line starts with key, else NULL.
static char *field(char *line, const char *key)
{
    size_t k = strlen(key);
    const char *t = skip_ws(line);
    if (strncasecmp(t, key, k) != 0) return NULL;
    const char *after = t + k;
    if (*after != ':') return NULL;
    return trim((char *)after + 1);
}

// Split "left | right" once. Returns right (or NULL) and NUL-terminates left.
static char *split_pipe(char *s)
{
    char *bar = strchr(s, '|');
    if (!bar) return NULL;
    *bar = '\0';
    trim(s);
    return trim(bar + 1);
}

// ---- widget helpers --------------------------------------------------------

static lv_obj_t *make_box(lv_obj_t *parent, uint32_t bg, uint32_t border)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_pad_all(o, 10, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt,
                            const lv_font_t *font, uint32_t color, lv_coord_t w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    if (w > 0) lv_obj_set_width(l, w);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// The outer container every diagram lives in: a flex column, so nothing inside
// can land on top of anything else.
static lv_obj_t *make_root(lv_obj_t *parent, lv_coord_t width)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_width(root, width);
    lv_obj_set_height(root, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(root, lv_color_hex(DIAG_SURFACE), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(root, lv_color_hex(DIAG_LINE), 0);
    lv_obj_set_style_border_width(root, 1, 0);
    lv_obj_set_style_radius(root, 8, 0);
    lv_obj_set_style_pad_all(root, 16, 0);
    lv_obj_set_style_margin_top(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 8, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
    return root;
}

// ---- flow ------------------------------------------------------------------
// A vertical chain of nodes with a down-arrow between them, and optional
// branches hanging off a node. Vertical because the Tab5 is 1280 px wide but
// its manual column is not, and a horizontal chain of seven boxes would have to
// shrink its text to fit - the failure mode this whole exercise started with.

static void flow_node(lv_obj_t *root, const char *text, lv_coord_t inner_w, bool first)
{
    if (!first) {
        lv_obj_t *a = make_label(root, LV_SYMBOL_DOWN, &lv_font_montserrat_18, DIAG_DIM, 0);
        lv_obj_set_style_pad_left(a, 18, 0);
    }
    lv_obj_t *box = make_box(root, DIAG_RAISED, DIAG_LINE);
    lv_obj_set_width(box, inner_w);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    make_label(box, text, &lv_font_montserrat_20, DIAG_INK, inner_w - 24);
}

static void flow_branch(lv_obj_t *root, const char *text, lv_coord_t inner_w)
{
    lv_obj_t *box = make_box(root, DIAG_SURFACE, DIAG_LINE);
    lv_obj_set_width(box, inner_w - 40);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_left(box, 40, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    // Accent stripe on the left edge marks it as a branch off the node above,
    // which is what the old drawings used a box-drawing tee for.
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(color_by_name("steel")), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    make_label(box, text, &lv_font_montserrat_18, DIAG_DIM, inner_w - 40 - 20);
}

// ---- stack -----------------------------------------------------------------
// Screen regions, drawn to scale. "row: <px> <name> | <caption>".

static void stack_row(lv_obj_t *root, int px, const char *name, const char *caption,
                      lv_coord_t inner_w, int total_px, lv_coord_t total_h)
{
    lv_obj_t *line = lv_obj_create(root);
    lv_obj_remove_style_all(line);
    lv_obj_set_width(line, inner_w);
    // The ROW grows to whatever it holds; only the BOX carries the proportional
    // height. Tying the row to the proportion clipped a two-line caption beside
    // a short band (the 60 px top bar lost the word "zoom").
    lv_coord_t h = (lv_coord_t)((long)px * total_h / (total_px ? total_px : 1));
    if (h < 26) h = 26;
    lv_obj_set_height(line, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(line, 12, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = make_box(line, DIAG_RAISED, DIAG_LINE);
    lv_obj_set_width(box, inner_w * 3 / 5);
    lv_obj_set_height(box, h);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_t *nl = make_label(box, name, &lv_font_montserrat_18, DIAG_INK, 0);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 0, 0);

    char right[96];
    snprintf(right, sizeof(right), "%d px%s%s", px, caption && caption[0] ? " - " : "",
             caption ? caption : "");
    lv_obj_t *cl = make_label(line, right, &lv_font_montserrat_18, DIAG_DIM,
                              inner_w * 2 / 5 - 24);
    lv_obj_set_flex_grow(cl, 1);
}

// ---- panel -----------------------------------------------------------------
// A mock-up of one of the app's own modals. Worth drawing rather than
// describing: both of the manual's modal pictures had drifted out of date
// (radio buttons where the real dialog has checkboxes, a title that no longer
// exists, two cycle buttons it had never heard of), and a picture that looks
// like the screen makes that kind of rot obvious.

static void panel_row(lv_obj_t *root, const char *text, lv_coord_t inner_w)
{
    // "[x] label" / "[ ] label" render as a real checkbox, anything else as a
    // plain row. The spec stays readable as text either way.
    bool has_box = (text[0] == '[' && text[2] == ']');
    bool checked = has_box && (text[1] == 'x' || text[1] == 'X');
    const char *label = has_box ? skip_ws(text + 3) : text;

    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, inner_w);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (has_box) {
        lv_obj_t *box = lv_obj_create(row);
        lv_obj_set_size(box, 22, 22);
        lv_obj_set_style_radius(box, 4, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(checked ? UI_COLOR_PRIMARY : DIAG_SURFACE), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(checked ? UI_COLOR_PRIMARY : DIAG_LINE), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_margin_top(box, 6, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        if (checked) {
            lv_obj_t *tick = lv_label_create(box);
            lv_label_set_text(tick, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(tick, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(tick, &lv_font_montserrat_14, 0);
            lv_obj_center(tick);
        }
    }
    lv_obj_t *field_box = make_box(row, DIAG_SURFACE, DIAG_LINE);
    lv_obj_set_height(field_box, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(field_box, 8, 0);
    lv_obj_set_flex_grow(field_box, 1);
    make_label(field_box, label, &lv_font_montserrat_18, DIAG_INK, inner_w - (has_box ? 80 : 46));
}

static void panel_buttons(lv_obj_t *root, char *csv, lv_coord_t inner_w)
{
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, inner_w);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    char *tok = strtok(csv, ",");
    while (tok) {
        char *label = trim(tok);
        // A trailing "!" marks the destructive one, "+" the confirming one, so
        // the mock-up carries the same colour language as the real dialog.
        uint32_t bg = UI_COLOR_PRIMARY;
        size_t len = strlen(label);
        if (len && label[len-1] == '!') { bg = 0x962020; label[len-1] = ' '; }
        else if (len && label[len-1] == '+') { bg = 0x2A6B3C; label[len-1] = ' '; }
        trim(label);
        lv_obj_t *b = lv_obj_create(row);
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_pad_all(b, 10, 0);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *l = make_label(b, label, &lv_font_montserrat_18, 0xFFFFFF, 0);
        lv_obj_center(l);
        tok = strtok(NULL, ",");
    }
}

// ---- timeline --------------------------------------------------------------
// The one type with computed coordinates. Every x comes from (t / span), so the
// only numbers a human writes are times.

typedef struct { float a, b; uint32_t col; bool overlay; char label[40]; } tl_seg_t;

static void timeline_render(lv_obj_t *root, float span, tl_seg_t *segs, int nseg,
                            float *ticks, int nticks, lv_coord_t inner_w)
{
    const lv_coord_t BAR_H = 46;
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, inner_w, BAR_H);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < nseg; i++) {
        lv_coord_t x0 = (lv_coord_t)(segs[i].a / span * inner_w);
        lv_coord_t x1 = (lv_coord_t)(segs[i].b / span * inner_w);
        if (x1 <= x0) x1 = x0 + 2;
        // An overlay ("mark") sits as a stripe along the bottom of the bar. A
        // full-height block starting at 0 read as if Capture began where TX
        // ended, when in fact TX happens DURING the first 2.8 s of capture.
        lv_coord_t sy = segs[i].overlay ? BAR_H - 14 : 0;
        lv_coord_t sh = segs[i].overlay ? 14 : BAR_H;
        lv_obj_t *s = lv_obj_create(bar);
        lv_obj_set_pos(s, x0, sy);
        lv_obj_set_size(s, x1 - x0, sh);
        lv_obj_set_style_bg_color(s, lv_color_hex(segs[i].col), 0);
        lv_obj_set_style_bg_opa(s, DIAG_FILL_OPA, 0);
        lv_obj_set_style_border_color(s, lv_color_hex(segs[i].col), 0);
        lv_obj_set_style_border_width(s, 1, 0);
        lv_obj_set_style_radius(s, 5, 0);
        lv_obj_set_style_pad_all(s, 0, 0);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
        // Only label a segment that can actually hold its text. A band too
        // narrow for its own name is what put "Decode" on top of its neighbour
        // in the mock-up; here the renderer simply declines, and the legend
        // below carries the name instead.
        if (!segs[i].overlay && segs[i].label[0] &&
            (x1 - x0) > (lv_coord_t)(strlen(segs[i].label) * 11 + 16)) {
            lv_obj_t *l = make_label(s, segs[i].label, &lv_font_montserrat_18, DIAG_INK, 0);
            lv_obj_center(l);
        }
    }

    // Tick labels on their own row, each centred on its time.
    lv_obj_t *axis = lv_obj_create(root);
    lv_obj_remove_style_all(axis);
    lv_obj_set_size(axis, inner_w, 22);
    lv_obj_clear_flag(axis, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < nticks; i++) {
        char buf[16];
        if (ticks[i] == (int)ticks[i]) snprintf(buf, sizeof(buf), "%d", (int)ticks[i]);
        else                           snprintf(buf, sizeof(buf), "%.1f", (double)ticks[i]);
        lv_obj_t *l = make_label(axis, buf, &lv_font_montserrat_14, DIAG_DIM, 0);
        lv_obj_update_layout(l);
        lv_coord_t w = lv_obj_get_width(l);
        lv_coord_t x = (lv_coord_t)(ticks[i] / span * inner_w) - w / 2;
        if (x < 0) x = 0;
        if (x + w > inner_w) x = inner_w - w;
        lv_obj_set_pos(l, x, 2);
    }
}

// ---- entry point -----------------------------------------------------------

lv_obj_t *reader_diagram_add(lv_obj_t *parent, const char *spec, lv_coord_t width)
{
    if (!parent || !spec) return NULL;

    // Work on a copy: the parser NUL-terminates fields in place.
    size_t n = strlen(spec);
    char *buf = lv_malloc(n + 1);
    if (!buf) return NULL;
    memcpy(buf, spec, n + 1);

    // First pass: the type, so we know which renderer to build into.
    char type[16] = {0};
    for (char *p = buf; p && *p; ) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';
        char *v = field(p, "type");
        if (v) { snprintf(type, sizeof(type), "%s", v); }
        if (eol) { *eol = '\n'; p = eol + 1; } else break;
        if (type[0]) break;
    }
    if (!type[0]) { lv_free(buf); return NULL; }

    lv_obj_t *root = make_root(parent, width);
    const lv_coord_t inner_w = width - 34;   // padding both sides + border

    bool is_flow = (strcasecmp(type, "flow") == 0);
    bool is_stack = (strcasecmp(type, "stack") == 0);
    bool is_time = (strcasecmp(type, "timeline") == 0);
    bool is_panel = (strcasecmp(type, "panel") == 0);

    // Stack needs the total height before it can place anything, so it is
    // measured first rather than guessed.
    int total_px = 0;
    if (is_stack) {
        char *scan = lv_malloc(n + 1);
        if (scan) {
            memcpy(scan, spec, n + 1);
            for (char *p = scan; p && *p; ) {
                char *eol = strchr(p, '\n');
                if (eol) *eol = '\0';
                char *v = field(p, "row");
                if (v) total_px += atoi(v);
                if (eol) { p = eol + 1; } else break;
            }
            lv_free(scan);
        }
        if (total_px <= 0) total_px = 1;
    }

    bool have_title = false;
    tl_seg_t segs[8]; int nseg = 0;
    float ticks[8];   int nticks = 0;
    float span = 0;
    bool first_node = true;

    for (char *p = buf; p && *p; ) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';
        char *line = p;
        char *v;

        if ((v = field(line, "title")) != NULL) {
            make_label(root, v, &lv_font_montserrat_20, color_by_name("gold"), inner_w);
            have_title = true;
        } else if (is_flow && (v = field(line, "node")) != NULL) {
            flow_node(root, v, inner_w, first_node);
            first_node = false;
        } else if (is_flow && (v = field(line, "branch")) != NULL) {
            flow_branch(root, v, inner_w);
        } else if (is_panel && (v = field(line, "row")) != NULL) {
            panel_row(root, v, inner_w);
        } else if (is_panel && (v = field(line, "buttons")) != NULL) {
            panel_buttons(root, v, inner_w);
        } else if (is_stack && (v = field(line, "row")) != NULL) {
            // "row: <px> <name> | <caption>"
            char *cap = split_pipe(v);
            int px = atoi(v);
            char *name = v;
            while (*name && *name != ' ') name++;
            name = (char *)skip_ws(name);
            stack_row(root, px, name, cap, inner_w, total_px, 300);
        } else if (is_time && (v = field(line, "span")) != NULL) {
            span = (float)atof(v);
        } else if (is_time && ((v = field(line, "seg")) != NULL ||
                               (v = field(line, "mark")) != NULL)) {
            bool overlay = (strncasecmp(skip_ws(line), "mark", 4) == 0);
            // "seg: <a>-<b> <colour> <label>"
            if (nseg < (int)(sizeof(segs)/sizeof(segs[0]))) {
                float a = (float)atof(v);
                char *dash = strchr(v, '-');
                float b = dash ? (float)atof(dash + 1) : a;
                char *rest = dash ? dash + 1 : v;
                while (*rest && *rest != ' ') rest++;
                rest = (char *)skip_ws(rest);
                char cname[16] = {0};
                size_t ci = 0;
                while (rest[ci] && rest[ci] != ' ' && ci < sizeof(cname) - 1) { cname[ci] = rest[ci]; ci++; }
                const char *lbl = skip_ws(rest + ci);
                segs[nseg].a = a; segs[nseg].b = b;
                segs[nseg].overlay = overlay;
                segs[nseg].col = color_by_name(cname);
                snprintf(segs[nseg].label, sizeof(segs[nseg].label), "%s", lbl);
                nseg++;
            }
        } else if (is_time && (v = field(line, "tick")) != NULL) {
            char *tok = strtok(v, ", ");
            while (tok && nticks < (int)(sizeof(ticks)/sizeof(ticks[0]))) {
                ticks[nticks++] = (float)atof(tok);
                tok = strtok(NULL, ", ");
            }
        } else if ((v = field(line, "note")) != NULL) {
            // "note: <colour> | <text>" - the legend, shared by every type.
            char *txt = split_pipe(v);
            lv_obj_t *row = lv_obj_create(root);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, inner_w);
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(row, 10, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *chip = lv_obj_create(row);
            lv_obj_set_size(chip, 14, 14);
            lv_obj_set_style_bg_color(chip, lv_color_hex(color_by_name(v)), 0);
            lv_obj_set_style_bg_opa(chip, DIAG_FILL_OPA, 0);
            lv_obj_set_style_border_color(chip, lv_color_hex(color_by_name(v)), 0);
            lv_obj_set_style_border_width(chip, 1, 0);
            lv_obj_set_style_radius(chip, 3, 0);
            lv_obj_set_style_margin_top(chip, 4, 0);
            lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
            make_label(row, txt ? txt : "", &lv_font_montserrat_18, DIAG_DIM, inner_w - 34);
        }

        if (eol) { p = eol + 1; } else break;
    }

    if (is_time && span > 0 && nseg > 0) {
        // The bar is built after the loop so it can be placed above the notes
        // that were collected from it.
        timeline_render(root, span, segs, nseg, ticks, nticks, inner_w);
        // Move it to the top, under any title.
        lv_obj_t *bar  = lv_obj_get_child(root, lv_obj_get_child_count(root) - 2);
        lv_obj_t *axis = lv_obj_get_child(root, lv_obj_get_child_count(root) - 1);
        // After the title, not before it - moving them to 0/1 pushed the title
        // below its own diagram.
        int at = have_title ? 1 : 0;
        lv_obj_move_to_index(bar,  at);
        lv_obj_move_to_index(axis, at + 1);
    }

    lv_free(buf);
    return root;
}
