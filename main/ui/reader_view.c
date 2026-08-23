// On-device docs Reader page — see reader_view.h and
// docs/reader-page-and-update-check-plan.md.
//
// Renders a markdown *subset* (no HTML engine on this device) read from the
// SPIFFS cache file that reader_net.c populates from tab5.lav.dk. The renderer
// is deliberately forgiving: anything it doesn't understand (mkdocs/pymdownx
// admonitions, tabbed blocks, snippet includes, front-matter, HTML) degrades to
// readable plain text rather than erroring — because it reads the site's real
// source markdown, not a purpose-stripped feed.

#include "reader_view.h"
#include "esp_attr.h"
#include "reader_net.h"
#include "ui_theme.h"
#include "reader_diagram.h"
#include "ui.h"                 // ui_help_overlay_changed()
#include "net/manual_embed.h"   // pages are read straight from the embedded manual
#include "storage/sd_archive.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static const char *TAG = "reader_view";

// Logical landscape geometry (post-rotation), same convention as
// ft8_screen_view.c which uses 1280x720 literals.
#define SCR_W          1280
#define SCR_H          720
#define HEADER_H       64    // modest bar; big touch handled by ext_click_area
#define BANNER_H       34
#define BODY_PAD_X     60
#define BODY_PAD_Y     22

#define READER_CACHE_PATH  "/spiffs/reader.md"
#define TOC_CACHE_PATH     "/spiffs/reader_toc.json"
#define MD_MAX_BYTES       (96 * 1024)   // cache-file read cap

// Table of contents (parsed from toc.json published by mkdocs_reader_export.py).
#define TOC_MAX  64
typedef struct { char title[48]; char path[96]; int level; } toc_entry_t;
// 9,472 bytes, and COLD - only touched while the Reader overlay is open.
// It was sitting in internal .bss, which on this board is the scarcest
// resource there is: measured during an OTA download in FT8 mode, internal
// free fell to 5 KB and the update could not allocate its own 8 KB task
// stack. Nothing here needs the speed or the DMA-reachability of internal
// RAM. See the #239 investigation.
EXT_RAM_BSS_ATTR static toc_entry_t s_toc[TOC_MAX];
static int  s_toc_n = 0;

// Drag-to-pick state for the Contents list: the finger drags a highlight bar
// over the page cells and releasing navigates to the highlighted one.
static lv_obj_t *s_toc_cells[TOC_MAX];   // the clickable page cells (in draw order)
static int       s_toc_cell_idx[TOC_MAX];// s_toc[] index for each cell
static int       s_toc_cell_n = 0;
static int       s_toc_hi = -1;          // currently highlighted cell (index into s_toc_cells)
static char s_current_path[96] = "index.md";     // page currently shown
static char s_page_title[64]   = "Documentation"; // title shown when TOC is closed

// ---- LVGL objects (LVGL thread only) ----
static lv_obj_t *s_overlay      = NULL;   // full-screen opaque page
static lv_obj_t *s_title_lbl    = NULL;   // header: page title
static lv_obj_t *s_back_btn     = NULL;   // hidden when there is nothing to go back to
static lv_obj_t *s_exit_btn     = NULL;   // slides left into Back's slot when it is hidden
static lv_obj_t *s_status_lbl   = NULL;   // header: right-aligned status
static lv_obj_t *s_banner       = NULL;   // update-available bar (hidden unless set)
static lv_obj_t *s_banner_lbl   = NULL;
static lv_obj_t *s_body         = NULL;   // scrollable flex column of content
static lv_obj_t *s_toc_btn      = NULL;   // header "Contents" button
static lv_obj_t *s_toc_panel    = NULL;   // scrollable contents overlay (hidden unless open)

// Context help: the heading we were asked to land on, and the rendered label for
// it once found. Set by reader_view_open_help() before the page loads; consumed
// (and cleared) by render_markdown(). Empty = land at the top, as before.
static char      s_want_anchor[80] = {0};
static lv_obj_t *s_anchor_obj      = NULL;

// Case-insensitive substring search. Deliberately not strcasestr(): that is a
// GNU extension and not reliably available here. Substring rather than exact
// match so an anchor survives small heading edits ("Tap to Tune" still finds
// "### 2. Tap to Tune") - the anchors are checked at build time by
// tools/pack_manual.py, so a genuinely broken one fails the build rather than
// silently landing on the page top.
static bool strcasestr_local(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return true;
    }
    return false;
}
static lv_timer_t *s_timer      = NULL;

static bool s_active = false;

// ---- Cross-task state (guarded by s_lock) ----
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_reload_pending = false;
static volatile bool s_toc_reload_pending = false;
static bool s_from_cache = false;
// Whether the "heading not found" warning has already been logged for the current
// anchor request. One request can render the same page more than once; the operator
// only needs telling once.
static bool s_anchor_reported = false;
static char s_status[64]  = {0};
static char s_update_ver[24] = {0};

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// Set the current page's title. Reflected on the top bar only while the TOC
// panel is closed (when it's open the bar shows "Contents").
static void fold_copy(char *dst, size_t dstsz, const char *src);   // defined below

static void set_page_title(const char *t)
{
    // Folded on the way in: the title comes from the page's own H1, which is as
    // free to contain a dash or an arrow as any other line.
    char clean[sizeof(s_page_title)];
    fold_copy(clean, sizeof(clean), (t && t[0]) ? t : "Documentation");
    t = clean;
    snprintf(s_page_title, sizeof(s_page_title), "%s", (t && t[0]) ? t : "Documentation");
    if (s_title_lbl && (!s_toc_panel || lv_obj_has_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN)))
        lv_label_set_text(s_title_lbl, s_page_title);
}

// ============================ markdown rendering ============================

// If `s` points at a UTF-8 sequence we fold to ASCII, return the number of input
// bytes consumed and set *rep to the replacement; else return 0. Covers the
// General Punctuation block (dashes/quotes/ellipsis/nbsp) and the Box Drawing
// block (─│┌┐└┘├… -> -|+) — the montserrat fonts have glyphs for none of these,
// so unfolded they render as tofu boxes (the ASCII-art layout diagrams in the
// docs were the worst offender).
// The montserrat fonts only carry ASCII + a handful of symbols (degree, the
// LV_SYMBOL glyphs). EVERYTHING else renders as a tofu box, so we transliterate
// what we can to ASCII and drop the rest. `s` points at a UTF-8 sequence; on a
// non-ASCII lead byte, decode the codepoint and return (bytes-consumed, *rep).
// Returns 0 for ASCII (caller copies the byte as-is).
static int fold_seq(const char *s, const char **rep)
{
    unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) return 0;   // ASCII

    // decode length + codepoint
    uint32_t cp; int len;
    if      ((c0 & 0xE0) == 0xC0) { len = 2; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { len = 3; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { len = 4; cp = c0 & 0x07; }
    else { *rep = ""; return 1; }   // stray continuation/invalid: drop 1 byte
    for (int k = 1; k < len; k++) {
        unsigned char cc = (unsigned char)s[k];
        if ((cc & 0xC0) != 0x80) { *rep = ""; return k; }   // truncated: drop what we saw
        cp = (cp << 6) | (cc & 0x3F);
    }

    switch (cp) {
        case 0x00B0: *rep = "\xC2\xB0"; return len;   // ° — KEEP (font has it)
        case 0x00A0: *rep = " ";        return len;   // nbsp
        case 0x2010: case 0x2011: case 0x2012:
        case 0x2013: case 0x2014: case 0x2015: *rep = "-";   return len; // hyphens/dashes
        case 0x2018: case 0x2019: case 0x201A: case 0x201B: *rep = "'";  return len; // single quotes
        case 0x201C: case 0x201D: case 0x201E: case 0x201F: *rep = "\""; return len; // double quotes
        case 0x2026: *rep = "..."; return len;   // ellipsis
        case 0x2022: case 0x00B7: case 0x2219: *rep = "-"; return len;   // bullet/middot
        case 0x2190: *rep = "(left)";  return len;   // clear words - "^"/"v" were ambiguous
        case 0x2191: *rep = "(up)";    return len;
        case 0x2192: *rep = "(right)"; return len;
        case 0x2193: *rep = "(down)";  return len;
        case 0x2194: *rep = "(left/right)"; return len;
        case 0x21D2: *rep = "=>";  return len;
        case 0x00A9: *rep = "(c)"; return len;
        case 0x00AE: *rep = "(r)"; return len;
        case 0x2122: *rep = "(tm)";return len;
        case 0x00D7: *rep = "x";   return len;   // multiplication
        case 0x00F7: *rep = "/";   return len;   // division
        case 0x00B1: *rep = "+/-"; return len;
        case 0x00B5: case 0x03BC: *rep = "u"; return len;   // micro / mu
        // Decorative emoji/symbols with no clean ASCII: drop rather than emit a
        // stray-looking "!" or box (⚠ was rendering as a lone "!").
        case 0x2714: case 0x2713: case 0x2717: case 0x2718: case 0x2716:
        case 0x26A0: case 0x2705: case 0x274C: case 0xFE0F: *rep = ""; return len;
        default: break;
    }
    // Box Drawing U+2500-257F -> -|+ , with the two diagonals kept as slashes
    // (they appear in the manual's antenna/signal sketches, where "+" loses the
    // sense of the line).
    if (cp >= 0x2500 && cp <= 0x257F) {
        if      (cp == 0x2571)                 *rep = "/";
        else if (cp == 0x2572)                 *rep = "\\";
        else if (cp == 0x2500 || cp == 0x2501) *rep = "-";
        else if (cp == 0x2502 || cp == 0x2503) *rep = "|";
        else                                   *rep = "+";
        return len;
    }
    // Block Elements U+2580-259F -> ASCII shading. These are the waterfall and
    // occupancy-strip sketches in the manual (U+2591 alone appears 128 times), and
    // they used to fall through to the drop-everything default - so the art did not
    // render as tofu, it silently VANISHED, leaving captioned blank space.
    if (cp >= 0x2580 && cp <= 0x259F) {
        if      (cp == 0x2591) *rep = ".";    // light shade
        else if (cp == 0x2592) *rep = ":";    // medium shade
        else                   *rep = "#";    // dark shade / full blocks
        return len;
    }
    // Geometric Shapes: the manual uses these as UI pointers and state dots, so a
    // word or punctuation carries the meaning where dropping them loses it.
    switch (cp) {
        case 0x25B2: *rep = "^"; return len;   // up triangle
        case 0x25BC: *rep = "v"; return len;   // down triangle
        case 0x25B6: case 0x25BA: *rep = ">"; return len;
        case 0x25C0: case 0x25C4: *rep = "<"; return len;
        case 0x25CB: case 0x25E6: *rep = "o"; return len;   // hollow dot
        case 0x25CF: case 0x25AA: *rep = "*"; return len;   // filled dot
        case 0x2248: *rep = "~";     return len;   // almost equal
        case 0x2264: *rep = "<=";    return len;
        case 0x2265: *rep = ">=";    return len;
        case 0x2260: *rep = "!=";    return len;
        case 0x00BD: *rep = "1/2";   return len;
        case 0x00BC: *rep = "1/4";   return len;
        case 0x00BE: *rep = "3/4";   return len;
        case 0x2715: case 0x2716: *rep = "x"; return len;   // multiplication X
        // Greek used as units/symbols in the docs - spell them, since the font has
        // no Greek and a dropped tau turns "tau = 1 s" into "= 1 s".
        case 0x03C4: *rep = "tau";   return len;
        case 0x03B1: *rep = "alpha"; return len;
        case 0x03A9: case 0x2126: *rep = "ohm"; return len;
        case 0x00B2: *rep = "^2";    return len;
        case 0x00B3: *rep = "^3";    return len;
        default: break;
    }
    // Latin-1 accented letters -> unaccented base (avoids mangling names into
    // tofu; a docs reader in a proportional font can't show the accents anyway)
    static const struct { uint32_t lo, hi; char base; } acc[] = {
        {0x00C0,0x00C5,'A'},{0x00C8,0x00CB,'E'},{0x00CC,0x00CF,'I'},
        {0x00D2,0x00D6,'O'},{0x00D9,0x00DC,'U'},{0x00E0,0x00E5,'a'},
        {0x00E8,0x00EB,'e'},{0x00EC,0x00EF,'i'},{0x00F2,0x00F6,'o'},
        {0x00F9,0x00FC,'u'},
    };
    static char one[2] = {0,0};
    for (unsigned i = 0; i < sizeof(acc)/sizeof(acc[0]); i++) {
        if (cp >= acc[i].lo && cp <= acc[i].hi) { one[0] = acc[i].base; *rep = one; return len; }
    }
    switch (cp) {
        case 0x00C7: one[0]='C'; *rep=one; return len;
        case 0x00E7: one[0]='c'; *rep=one; return len;
        case 0x00D1: one[0]='N'; *rep=one; return len;
        case 0x00F1: one[0]='n'; *rep=one; return len;
        case 0x00DD: case 0x00FD: case 0x00FF: one[0]='y'; *rep=one; return len;
        case 0x00DF: *rep="ss"; return len;
        case 0x00C6: *rep="AE"; return len;
        case 0x00E6: *rep="ae"; return len;
        case 0x00D8: one[0]='O'; *rep=one; return len;
        case 0x00F8: one[0]='o'; *rep=one; return len;
        default: break;
    }
    *rep = "";   // anything else: drop rather than render a tofu box
    return len;
}

// Append the bytes in [from, end) to dst at offset o, FOLDING as it goes; returns
// the new offset.
//
// This exists because both markdown walkers had the same hole: their link and image
// branches copied the label text out of "[...]" byte-for-byte, bypassing the fold
// they perform on every other character. So every arrow OUTSIDE a link rendered
// correctly while "[Web UI -> LoTW Upload]" put a tofu box on screen - which is
// exactly what the operator found after I had reported the glyphs all handled. The
// lesson: auditing the fold TABLE's coverage says nothing about code paths that
// never call it. Any new place that copies source text must come through here.
// Fold a short string for immediate use. The buffer is a file-local STATIC, not a
// local: this runs on taskLVGL, where a few hundred bytes of stack is a real hazard
// on this board. Safe because every caller is on that one task and hands the result
// straight to lv_label_set_text(), which copies.
static const char *fold_static(const char *s)
{
    static char buf[192];
    fold_copy(buf, sizeof(buf), s ? s : "");
    return buf;
}

static size_t append_folded(char *dst, size_t dstsz, size_t o,
                            const char *from, const char *end)
{
    for (const char *q = from; q < end; ) {
        const char *r; int a = fold_seq(q, &r);
        if (a) { while (*r && o + 1 < dstsz) dst[o++] = *r++; q += a; }
        else   { if (o + 1 < dstsz) dst[o++] = *q; q++; }
    }
    return o;
}

// Fold UTF-8 punctuation/box-drawing to ASCII into a SEPARATE bounded buffer.
// (Must NOT be done in place: some folds EXPAND — e.g. an arrow "→" (3 bytes)
// -> "(right)" (7) — so an in-place write would overrun the source. That bug
// corrupted the heap. Used for verbatim code blocks, which bypass the
// markdown-stripping md_inline_clean.)
static void fold_copy(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dstsz; ) {
        const char *rep; int adv = fold_seq(&src[i], &rep);
        if (adv) { while (*rep && o + 1 < dstsz) dst[o++] = *rep++; i += (size_t)adv; }
        else     { dst[o++] = src[i++]; }
    }
    dst[o] = '\0';
}

// Trim leading/trailing spaces/tabs in place; returns the trimmed start.
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    return s;
}

// Strip inline emphasis markers and reduce links to their visible text, so the
// text drops cleanly into a plain LVGL label. (LVGL labels are single-font; we
// don't attempt bold/italic runs in v1 — see the plan.)
//   **x** __x__ *x* _x* `x`  -> x
//   [text](url)              -> text
//   ![alt](url)              -> "[image] alt"
static void md_inline_clean(const char *src, char *dst, size_t dstsz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dstsz; ) {
        char c = src[i];
        // Fold UTF-8 punctuation/box-drawing to ASCII (no font glyphs otherwise).
        const char *rep;
        int adv = fold_seq(&src[i], &rep);
        if (adv) {
            for (const char *r = rep; *r && o + 1 < dstsz; r++) dst[o++] = *r;
            i += (size_t)adv;
            continue;
        }
        // image ![alt](url)
        if (c == '!' && src[i+1] == '[') {
            const char *close = strchr(src + i + 2, ']');
            if (close && close[1] == '(') {
                const char *paren = strchr(close, ')');
                if (paren) {
                    o += (size_t)snprintf(dst + o, dstsz - o, "[image] ");
                    o = append_folded(dst, dstsz, o, src + i + 2, close);
                    i = (size_t)(paren - src) + 1;
                    continue;
                }
            }
        }
        // link [text](url) -> text
        if (c == '[') {
            const char *close = strchr(src + i + 1, ']');
            if (close && close[1] == '(') {
                const char *paren = strchr(close, ')');
                if (paren) {
                    o = append_folded(dst, dstsz, o, src + i + 1, close);
                    i = (size_t)(paren - src) + 1;
                    continue;
                }
            }
        }
        // emphasis / code markers: drop them
        if (c == '*' || c == '_' || c == '`') {
            // collapse a run of the same marker (** __ ``) to nothing
            char m = c;
            while (src[i] == m) i++;
            continue;
        }
        dst[o++] = c;
        i++;
    }
    dst[o] = '\0';
}

// Append a text label to the body. width = 100% of the content area so text
// wraps; caller picks font, colour, and top gap.
static lv_obj_t *add_label(const char *text, const lv_font_t *font,
                           uint32_t color, int top_gap, int left_indent)
{
    if (!s_body) return NULL;
    lv_obj_t *l = lv_label_create(s_body);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_pad_top(l, top_gap, 0);
    if (left_indent) lv_obj_set_style_pad_left(l, left_indent, 0);

    // FOLD HERE, not (only) in the callers. Most callers pass text that has already
    // been through md_inline_clean(), but the heading and blockquote paths passed
    // their raw slice straight in - so any character the font lacks reached LVGL and
    // rendered as a tofu box. Folding at the single point where text becomes a label
    // makes that impossible to get wrong again; re-folding already-folded text is a
    // no-op, since fold_seq() returns 0 for ASCII.
    const char *src = (text && text[0]) ? text : " ";
    size_t need = strlen(src) * 5 + 16;          // folds can EXPAND ("(left/right)")
    char *folded = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (folded) {
        fold_copy(folded, need, src);
        lv_label_set_text(l, folded[0] ? folded : " ");
        heap_caps_free(folded);
    } else {
        lv_label_set_text(l, src);
    }
    return l;
}

// Rich inline text via an lv_spangroup: renders **bold**/*em* runs in gold and
// `code` runs in green within one wrapping block (the montserrat fonts have no
// bold face, so weight is conveyed by COLOUR). Folds UTF-8 punctuation and
// reduces links/images to their visible text, same as md_inline_clean. Returns
// the spangroup (so callers can add a left border etc.).
#define RICH_CODE_COLOR  0x8fd98f   // green for `code`
#define RICH_BOLD_COLOR  0x66c8ff   // light cyan for **bold** — distinct from gold headings
static lv_obj_t *add_rich_span(lv_obj_t *parent, const char *src,
                               const lv_font_t *font, uint32_t base_color)
{
    if (!parent) return NULL;
    lv_obj_t *sg = lv_spangroup_create(parent);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);   // wrap to width

    char seg[1024]; size_t o = 0;
    int emph = 0, code = 0;

    #define FLUSH_SPAN() do {                                                   \
        if (o) { seg[o] = '\0';                                                 \
            lv_span_t *sp = lv_spangroup_new_span(sg);                          \
            lv_span_set_text(sp, seg);                                          \
            lv_style_t *st = lv_span_get_style(sp);                             \
            lv_style_set_text_font(st, font);                                   \
            uint32_t col = code ? RICH_CODE_COLOR : (emph ? RICH_BOLD_COLOR : base_color); \
            lv_style_set_text_color(st, lv_color_hex(col));                     \
            o = 0; }                                                            \
    } while (0)

    for (size_t i = 0; src[i]; ) {
        const char *rep; int adv = fold_seq(&src[i], &rep);
        if (adv) { while (*rep && o + 1 < sizeof(seg)) seg[o++] = *rep++; i += (size_t)adv; continue; }
        char c = src[i];
        if (c == '`') { FLUSH_SPAN(); code = !code; i++; continue; }
        if (c == '*' || c == '_') { FLUSH_SPAN(); emph = !emph; char m = c; while (src[i] == m) i++; continue; }
        if (c == '!' && src[i+1] == '[') {                       // image ![alt](url)
            const char *close = strchr(src + i + 2, ']');
            if (close && close[1] == '(') {
                const char *paren = strchr(close, ')');
                if (paren) {
                    const char *pfx = "[image] ";
                    while (*pfx && o + 1 < sizeof(seg)) seg[o++] = *pfx++;
                    o = append_folded(seg, sizeof(seg), o, src + i + 2, close);
                    i = (size_t)(paren - src) + 1; continue;
                }
            }
        }
        if (c == '[') {                                          // link [text](url) -> text
            const char *close = strchr(src + i + 1, ']');
            if (close && close[1] == '(') {
                const char *paren = strchr(close, ')');
                if (paren) {
                    o = append_folded(seg, sizeof(seg), o, src + i + 1, close);
                    i = (size_t)(paren - src) + 1; continue;
                }
            }
        }
        if (o + 1 < sizeof(seg)) seg[o++] = c;
        i++;
    }
    FLUSH_SPAN();
    #undef FLUSH_SPAN
    return sg;
}

// Rich text as a full-width body block (paragraph / quote).
static lv_obj_t *add_rich(const char *src, const lv_font_t *font,
                          uint32_t base_color, int top_gap, int left_indent)
{
    lv_obj_t *sg = add_rich_span(s_body, src, font, base_color);
    if (!sg) return NULL;
    lv_obj_set_width(sg, LV_PCT(100));
    lv_obj_set_style_pad_top(sg, top_gap, 0);
    if (left_indent) lv_obj_set_style_pad_left(sg, left_indent, 0);
    return sg;
}

// A code / preformatted block: distinct background, smaller font, no emphasis
// stripping (rendered verbatim).
static void add_code_block(const char *text)
{
    if (!s_body) return;
    // Box hugs its content (only as wide as the code needs), capped so a very
    // long line wraps instead of overflowing the screen.
    lv_obj_t *box = lv_obj_create(s_body);
    lv_obj_set_width(box, LV_SIZE_CONTENT);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(box, LV_PCT(100), 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_KEY_BG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_margin_top(box, 8, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *l = lv_label_create(box);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(l, SCR_W - 2 * BODY_PAD_X - 24, 0);
    // REVERTED to montserrat 2026-08-08, same day it was tried. unscii_16 is a
    // bitmap pixel font and the operator's verdict was "super ugly and not
    // readable" - and it did NOT fix the drawings anyway.
    //
    // Why monospace was never going to be enough: the drawings are built from
    // UTF-8 box characters, arrows and an emoji, and this reader folds UTF-8 to
    // ASCII before drawing. Some folds CHANGE THE LENGTH - an arrow becomes
    // "(right)", one character turning into seven. A line whose character count
    // grew after the author aligned it cannot line up in any font, monospace or
    // not, and it overruns the box and wraps.
    //
    // The fix is to stop drawing pictures out of characters. See TODO #96.
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    // Fold UTF-8 -> ASCII into a PSRAM temp (fold can expand, so not in place);
    // code is verbatim otherwise (no markdown stripping). Label copies the text.
    size_t need = strlen(text ? text : "") * 4 + 16;
    char *folded = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (folded) { fold_copy(folded, need, text ? text : ""); lv_label_set_text(l, folded[0] ? folded : " "); heap_caps_free(folded); }
    else        { lv_label_set_text(l, text && text[0] ? text : " "); }
}

// Render a markdown table (accumulated rows, newline-separated, mutated in
// place) as one bordered block: header row + data rows, cells separated, the
// header and each row's first cell in gold (via LVGL label recolor). Far nicer
// than the old one-bordered-box-per-row degrade. LVGL thread only.
static void add_table_block(char *rows)
{
    if (!s_body) return;

    // Outer bordered box, full width, one flex-column of row containers. Each
    // row is a flex ROW of equal-width (flex-grow 1) cells, so columns line up
    // vertically across rows and cell text wraps within its own column.
    lv_obj_t *box = lv_obj_create(s_body);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 4, 0);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_set_style_margin_top(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

    int rownum = 0;
    char *p = rows;
    while (*p) {
        char *line = p;
        char *eol = strchr(p, '\n');
        if (eol) { *eol = '\0'; p = eol + 1; } else { p = line + strlen(line); }
        char *tt = trim(line);
        if (!*tt) continue;
        if (strspn(tt, "|-: ") == strlen(tt)) continue;   // header/body separator row

        // split into cells on '|' (drop the outer pipes)
        char *cells[10]; int nc = 0;
        char *s = tt; if (*s == '|') s++;
        char *tok = s;
        while (nc < 10) {
            char *bar = strchr(tok, '|');
            if (bar) *bar = '\0';
            cells[nc++] = trim(tok);
            if (!bar) break;
            tok = bar + 1;
        }
        if (nc > 0 && cells[nc-1][0] == '\0') nc--;   // trailing empty from closing '|'
        if (nc == 0) continue;

        lv_obj_t *rowc = lv_obj_create(box);
        lv_obj_remove_style_all(rowc);
        lv_obj_set_width(rowc, LV_PCT(100));
        lv_obj_set_height(rowc, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rowc, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(rowc, 16, 0);
        lv_obj_set_style_pad_ver(rowc, 8, 0);
        lv_obj_set_style_pad_hor(rowc, 8, 0);
        lv_obj_set_scrollbar_mode(rowc, LV_SCROLLBAR_MODE_OFF);
        if (rownum > 0) {   // separator line under the header
            lv_obj_set_style_border_side(rowc, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(rowc, (rownum == 1) ? 1 : 0, 0);
            lv_obj_set_style_border_color(rowc, lv_color_hex(UI_COLOR_BORDER), 0);
        }

        for (int c = 0; c < nc; c++) {
            char cbuf[320];
            md_inline_clean(cells[c], cbuf, sizeof(cbuf));   // fold + strip into a SEPARATE buffer
            lv_obj_t *cl = lv_label_create(rowc);
            lv_obj_set_flex_grow(cl, 1);                 // equal columns -> aligned
            lv_obj_set_width(cl, 0);                     // let flex-grow drive width
            lv_label_set_long_mode(cl, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(cl, &lv_font_montserrat_22, 0);
            lv_obj_set_style_text_color(cl, lv_color_hex(rownum == 0 ? UI_COLOR_ACCENT_GOLD : UI_COLOR_TEXT), 0);
            lv_label_set_text(cl, cbuf[0] ? cbuf : " ");
        }
        rownum++;
    }
}

// Number of leading spaces (tabs count as 4), used for list indent level.
static int leading_indent(const char *s)
{
    int n = 0;
    for (; *s == ' ' || *s == '\t'; s++) n += (*s == '\t') ? 4 : 1;
    return n;
}

static const char *skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

// Work buffers for render_markdown. Allocated from PSRAM per render, NOT on the
// stack: render runs on the LVGL task (~8 KB stack) via the reader tick timer,
// and ~6.4 KB of stack arrays here overflowed it the instant real content
// rendered (Stack protection fault inside lv_label_create). Same rule as the
// FT8 pounce path — keep kB-scale buffers off LVGL-thread stacks.
#define MD_PARA_SZ     1024
#define MD_CODE_SZ     4096
#define MD_CLEANED_SZ  1024
#define MD_TITLE_SZ    MD_CLEANED_SZ   // holds a cleaned heading line in full
#define MD_LINE_SZ     (MD_CLEANED_SZ + 40)  // bullet/number prefix + cleaned text
typedef struct {
    char title[MD_TITLE_SZ];
    char para[MD_PARA_SZ];
    char code[MD_CODE_SZ];
    char cleaned[MD_CLEANED_SZ];   // shared scratch for md_inline_clean output
} md_scratch_t;

// A list item: a flex row of [marker][text]. The text label flex-grows and
// wraps within its own (indented) box, so wrapped lines hang under the text
// rather than sliding back under the marker.
static void add_list_item(const char *marker, const char *text, int indent)
{
    if (!s_body) return;
    lv_obj_t *row = lv_obj_create(s_body);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 6, 0);
    lv_obj_set_style_pad_left(row, 8 + indent * 24, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *m = lv_label_create(row);
    lv_obj_set_width(m, 34);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(m, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_label_set_text(m, marker);

    // Text as a rich span (so **bold** shows), flex-growing so wrapped lines
    // hang under the text.
    lv_obj_t *sg = add_rich_span(row, text && text[0] ? text : " ",
                                 &lv_font_montserrat_24, UI_COLOR_TEXT);
    if (sg) lv_obj_set_flex_grow(sg, 1);
}

// Render one markdown document (mutated in place: line terminators are consumed
// by the walker) into the body container. `buf` is NUL-terminated.
static void render_markdown(char *buf)
{
    lv_obj_clean(s_body);

    md_scratch_t *S = heap_caps_malloc(sizeof(md_scratch_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!S) {
        add_label("Out of memory rendering documentation.",
                  &lv_font_montserrat_24, UI_COLOR_TEXT_MUTED, 0, 0);
        return;
    }
    char *title   = S->title;   title[0] = '\0';
    char *para    = S->para;    size_t para_len = 0;   // accumulated paragraph text
    char *code    = S->code;    size_t code_len = 0;   // accumulated code-block text
    char *cleaned = S->cleaned;                        // reused per block (single-threaded)
    bool in_code = false;
    bool in_diagram = false;   // the open fence is a ```qmxdiagram
    bool in_table = false;   // accumulating consecutive '|' rows into `code`
    bool in_frontmatter = false;
    int  block_count = 0;

    char *p = buf;
    bool first_line = true;

    #define FLUSH_PARA() do {                                              \
        if (para_len) {                                                    \
            add_rich(para, &lv_font_montserrat_24, UI_COLOR_TEXT,          \
                     block_count ? 14 : 0, 0);                             \
            block_count++; para_len = 0; para[0] = '\0';                   \
        } } while (0)

    while (*p) {
        // isolate one line [line, eol)
        char *line = p;
        char *eol = strchr(p, '\n');
        if (eol) { *eol = '\0'; p = eol + 1; } else { p = line + strlen(line); }
        // strip a trailing CR
        size_t ll = strlen(line);
        if (ll && line[ll-1] == '\r') line[--ll] = '\0';

        // YAML front-matter: leading '---' fence at very top
        if (first_line && strcmp(line, "---") == 0) { in_frontmatter = true; first_line = false; continue; }
        first_line = false;
        if (in_frontmatter) { if (strcmp(line, "---") == 0 || strcmp(line, "...") == 0) in_frontmatter = false; continue; }

        const char *t = skip_ws(line);

        // A table run ends the moment a non-'|' line arrives (blank line,
        // heading, fence, prose, ...). Flush the accumulated rows as one block.
        if (in_table && !in_code && t[0] != '|') {
            code[code_len] = '\0';
            add_table_block(code);
            code_len = 0; in_table = false; block_count++;
        }

        // fenced code block ``` or ~~~ (pymdownx superfences: language/opts follow)
        if (strncmp(t, "```", 3) == 0 || strncmp(t, "~~~", 3) == 0) {
            if (in_code) {
                code[code_len] = '\0';
                // A ```qmxdiagram fence is a drawing, not code - see
                // ui/reader_diagram.h for why the manual's pictures stopped
                // being made of characters. If the spec is unusable the
                // renderer returns NULL and we show the raw text, so a bad
                // diagram degrades to something readable rather than to a gap.
                lv_obj_t *drawn = NULL;
                if (in_diagram) {
                    drawn = reader_diagram_add(s_body, code, SCR_W - 2 * BODY_PAD_X);
                }
                if (!drawn) add_code_block(code);
                code_len = 0; block_count++; in_code = false; in_diagram = false;
            } else {
                FLUSH_PARA();
                in_code = true; code_len = 0;
                in_diagram = (strncasecmp(t + 3, "qmxdiagram", 10) == 0);
            }
            continue;
        }
        if (in_code) {
            size_t need = strlen(line) + 1;
            if (code_len + need < MD_CODE_SZ) { code_len += (size_t)snprintf(code + code_len, MD_CODE_SZ - code_len, "%s\n", line); }
            continue;
        }

        // skip mkdocs snippet includes and single-line HTML comments
        if (strncmp(t, "--8<--", 6) == 0) continue;
        if (strncmp(t, "<!--", 4) == 0 && strstr(t, "-->")) continue;

        // blank line -> paragraph break
        if (t[0] == '\0') { FLUSH_PARA(); continue; }

        // headings
        if (t[0] == '#') {
            int level = 0; const char *h = t;
            while (*h == '#' && level < 6) { level++; h++; }
            if (*h == ' ') {
                FLUSH_PARA();
                h = skip_ws(h);
                md_inline_clean(h, cleaned, MD_CLEANED_SZ);
                const lv_font_t *f = (level == 1) ? &lv_font_montserrat_32 :
                                     (level == 2) ? &lv_font_montserrat_28 :
                                     (level == 3) ? &lv_font_montserrat_24 :
                                                    &lv_font_montserrat_22;
                uint32_t col = (level <= 3) ? UI_COLOR_ACCENT_GOLD : UI_COLOR_TEXT;
                lv_obj_t *hl = add_label(cleaned, f, col, block_count ? 22 : 4, 0);
                block_count++;
                // Context help: remember the first heading that matches the
                // requested anchor, so the render can finish and THEN scroll to
                // it (its y is only known once the flex column is laid out).
                if (hl && !s_anchor_obj && s_want_anchor[0] &&
                    strcasestr_local(cleaned, s_want_anchor)) {
                    s_anchor_obj = hl;
                }
                if (!title[0] && level == 1) { snprintf(title, MD_TITLE_SZ, "%s", cleaned); }
                continue;
            }
        }

        // pymdownx content-tab marker:  === "Windows"   (renders as a gold
        // sub-heading; the indented tab body follows as normal paragraphs).
        if (strncmp(t, "===", 3) == 0) {
            FLUSH_PARA();
            const char *q = strchr(t, '"');
            if (q) {
                const char *q2 = strchr(q + 1, '"');
                size_t n = q2 ? (size_t)(q2 - q - 1) : strlen(q + 1);
                char head[80];
                if (n >= sizeof(head)) n = sizeof(head) - 1;
                memcpy(head, q + 1, n); head[n] = '\0';
                add_label(head, &lv_font_montserrat_24, UI_COLOR_ACCENT_GOLD, 14, 0);
                block_count++;
            }
            continue;   // no quoted title -> a bare === separator, just drop it
        }

        // horizontal rule (also folds long runs like ------ / ******)
        if (strlen(t) >= 3 && (strspn(t, "-") == strlen(t) ||
                               strspn(t, "*") == strlen(t) ||
                               strspn(t, "_") == strlen(t))) {
            FLUSH_PARA();
            lv_obj_t *hr = lv_obj_create(s_body);
            lv_obj_set_size(hr, LV_PCT(100), 2);
            lv_obj_set_style_bg_color(hr, lv_color_hex(UI_COLOR_BORDER), 0);
            lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(hr, 0, 0);
            lv_obj_set_style_margin_top(hr, 12, 0);
            lv_obj_set_style_margin_bottom(hr, 4, 0);
            block_count++;
            continue;
        }

        // mkdocs/pymdownx admonition marker: "!!! note \"Title\"" / "??? tip"
        if (strncmp(t, "!!!", 3) == 0 || strncmp(t, "???", 3) == 0) {
            FLUSH_PARA();
            const char *rest = skip_ws(t + 3);
            char head[120] = {0};
            // pull the quoted title if present, else the admonition type word
            const char *q = strchr(rest, '"');
            if (q) {
                const char *q2 = strchr(q + 1, '"');
                size_t n = q2 ? (size_t)(q2 - q - 1) : strlen(q + 1);
                if (n >= sizeof(head)) n = sizeof(head) - 1;
                memcpy(head, q + 1, n); head[n] = '\0';
            } else {
                size_t n = 0; while (rest[n] && rest[n] != ' ' && n < sizeof(head)-1) { head[n] = (char)toupper((unsigned char)rest[n]); n++; }
                head[n] = '\0';
            }
            add_label(head[0] ? head : "NOTE", &lv_font_montserrat_22, UI_COLOR_ACCENT_GOLD, 14, 0);
            block_count++;
            continue;   // the indented body lines that follow render as normal paragraphs
        }

        // block quote
        if (t[0] == '>') {
            FLUSH_PARA();
            lv_obj_t *l = add_rich(skip_ws(t + 1), &lv_font_montserrat_22, UI_COLOR_TEXT_SECONDARY, 8, 16);
            if (l) {
                lv_obj_set_style_border_side(l, LV_BORDER_SIDE_LEFT, 0);
                lv_obj_set_style_border_width(l, 3, 0);
                lv_obj_set_style_border_color(l, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
            }
            block_count++;
            continue;
        }

        // list items: -, *, + or "N." / "N)"
        {
            int indent = leading_indent(line);
            bool bullet = (t[0] == '-' || t[0] == '*' || t[0] == '+') && (t[1] == ' ');
            bool numbered = isdigit((unsigned char)t[0]);
            const char *nptr = t;
            if (numbered) {
                while (isdigit((unsigned char)*nptr)) nptr++;
                numbered = (*nptr == '.' || *nptr == ')') && (nptr[1] == ' ');
            }
            if (bullet || numbered) {
                FLUSH_PARA();
                int lvl = indent / 2;   // ~2 leading spaces per nest level
                if (bullet) {
                    add_list_item(LV_SYMBOL_BULLET, skip_ws(t + 1), lvl);   // raw (add_rich_span handles **bold**)
                } else {
                    char num[8]; size_t k = 0;
                    for (const char *d = t; (isdigit((unsigned char)*d) || *d=='.'|| *d==')') && k < sizeof(num)-1; d++) num[k++] = *d;
                    num[k] = '\0';
                    add_list_item(num, skip_ws(nptr + 1), lvl);
                }
                block_count++;
                continue;
            }
        }

        // table rows: accumulate consecutive '|' lines; rendered as one block
        // by add_table_block() when the run ends (see the flush at loop top).
        if (t[0] == '|') {
            if (!in_table) { FLUSH_PARA(); in_table = true; code_len = 0; }
            size_t need = strlen(t) + 1;
            if (code_len + need < MD_CODE_SZ)
                code_len += (size_t)snprintf(code + code_len, MD_CODE_SZ - code_len, "%s\n", t);
            continue;
        }

        // ordinary text: accumulate into the current paragraph (soft-wrap join)
        {
            const char *seg = skip_ws(line);
            size_t need = strlen(seg) + 1;
            if (para_len + need < MD_PARA_SZ) {
                if (para_len) para[para_len++] = ' ';
                memcpy(para + para_len, seg, strlen(seg));
                para_len += strlen(seg);
                para[para_len] = '\0';
            }
        }
    }

    FLUSH_PARA();
    if (in_code && code_len)  { code[code_len] = '\0'; add_code_block(code); }
    if (in_table && code_len) { code[code_len] = '\0'; add_table_block(code); }

    #undef FLUSH_PARA

    set_page_title(title);
    if (s_anchor_obj) {
        // Lay the flex column out first - a child's y is meaningless until then.
        lv_obj_update_layout(s_body);
        lv_coord_t y = lv_obj_get_y(s_anchor_obj);
        if (y < 0) y = 0;
        lv_obj_scroll_to_y(s_body, y, LV_ANIM_OFF);
        ESP_LOGI(TAG, "context help: landed on '%s' (y=%d)", s_want_anchor, (int)y);
    } else {
        if (s_want_anchor[0] && !s_anchor_reported) {
            s_anchor_reported = true;   // once per request, not once per render
            ESP_LOGW(TAG, "context help: heading '%s' not found on this page - showing the top",
                     s_want_anchor);
        }
        lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
    }
    // The anchor stays armed until the PAGE changes (load_page/navigate_to clear
    // it), rather than being consumed by the first render. A single help request
    // can produce more than one render of the same page - the immediate one plus
    // the one reader_net's completion notify triggers - and a one-shot anchor meant
    // the second render found nothing to aim at and reset the scroll to the top,
    // undoing the jump a fraction of a second after making it.
    s_anchor_obj = NULL;
    ESP_LOGI(TAG, "rendered %d blocks", block_count);
    heap_caps_free(S);   // title already copied into the label above
}

// Read the cache file and render it. LVGL thread only.
// Render the current page STRAIGHT FROM THE EMBEDDED MANUAL.
//
// This used to read /spiffs/reader.md, which reader_net.c had just written from the
// very same blob - a pointless round-trip through flash that became a real bug on
// 2026-08-06: /spiffs is 1 MB shared with qso.adi, the LoTW cert+key and the diag
// log's 256 KB rolling file plus a rotation, so on a well-used device the write
// returned 0 of 12031 bytes and the manual showed an error for a page that was
// sitting in the firmware the whole time. Reading the blob cannot fail that way,
// needs no free space, and is faster.
//
// render_markdown() mutates its buffer, so the blob (in flash, const) is copied
// into PSRAM first rather than cast away.
static void render_from_cache(void)
{
    const char *data = NULL;
    size_t len = 0;
    if (!manual_embed_get(s_current_path, &data, &len) || !data) {
        lv_obj_clean(s_body);
        add_label("That page is not in this firmware's built-in manual.",
                  &lv_font_montserrat_24, UI_COLOR_TEXT_MUTED, 0, 0);
        set_page_title("Manual");
        ESP_LOGW(TAG, "render: '%s' not in the embedded manual", s_current_path);
        return;
    }
    if (len > MD_MAX_BYTES - 1) len = MD_MAX_BYTES - 1;
    char *buf = heap_caps_malloc(MD_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGW(TAG, "OOM rendering page"); return; }
    memcpy(buf, data, len);
    buf[len] = '\0';
    render_markdown(buf);
    heap_caps_free(buf);
}

// ============================ table of contents ============================

// Parse /spiffs/reader_toc.json ({"pages":[{title,path,level}]}) into s_toc[].
// LVGL thread only.
static void parse_toc_file(void)
{
    // Same reasoning as render_from_cache(): the contents list is "toc.json" inside
    // the embedded manual, so read it there rather than via a SPIFFS copy that can
    // fail to be written on a full partition.
    s_toc_n = 0;
    const char *data = NULL;
    size_t n = 0;
    if (!manual_embed_get("toc.json", &data, &n) || !data) return;
    if (n > 16 * 1024 - 1) n = 16 * 1024 - 1;
    char *buf = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    memcpy(buf, data, n);

    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    if (!root) return;

    cJSON *pages = cJSON_GetObjectItem(root, "pages");
    if (cJSON_IsArray(pages)) {
        int cnt = cJSON_GetArraySize(pages);
        for (int i = 0; i < cnt && s_toc_n < TOC_MAX; i++) {
            cJSON *p  = cJSON_GetArrayItem(pages, i);
            cJSON *t  = cJSON_GetObjectItem(p, "title");
            cJSON *pa = cJSON_GetObjectItem(p, "path");
            cJSON *lv = cJSON_GetObjectItem(p, "level");
            toc_entry_t *e = &s_toc[s_toc_n];
            snprintf(e->title, sizeof(e->title), "%s",
                     (cJSON_IsString(t) && t->valuestring) ? t->valuestring : "");
            if (cJSON_IsString(pa) && pa->valuestring)
                snprintf(e->path, sizeof(e->path), "%s", pa->valuestring);
            else
                e->path[0] = '\0';   // section header (no page)
            e->level = cJSON_IsNumber(lv) ? lv->valueint : 0;
            s_toc_n++;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "toc: %d entries", s_toc_n);
}

static void navigate_to(const char *rel, const char *title_hint);
static void index_btn_cb(lv_event_t *e);

// ---- A-Z index (context help, Layer 4) -------------------------------------
//
// Built into manual.bin as index.json by tools/pack_manual.py, one entry per
// heading in the manual, with every anchor verified at build time.
//
// Deliberately NOT a generated markdown page of links: this reader renders
// links as plain TEXT (only the Contents panel navigates), so an index written
// that way would look correct and do nothing.
//
// Two stages, letters then terms, for a reason that is not cosmetic: 256 terms
// means 256 LVGL objects, and at this display's ~13 fps building that many at
// once is a visible stall. A letter holds a couple of dozen.
#define IDX_MAX        320
#define IDX_CELL_MAX   96
typedef struct {
    char term[72];
    char path[56];
    char anchor[72];
} index_entry_t;

static index_entry_t *s_idx = NULL;      // PSRAM; parsed once, kept
static int   s_idx_n = 0;
static lv_obj_t *s_idx_panel = NULL;
static lv_obj_t *s_idx_btn   = NULL;   // header "A-Z" button
static lv_obj_t *s_idx_cells[IDX_CELL_MAX];
static int   s_idx_cell_term[IDX_CELL_MAX];  // -1 = a letter cell / the back cell
static char  s_idx_cell_letter[IDX_CELL_MAX];
static int   s_idx_cell_n = 0;
static int   s_idx_hi = -1;
static char  s_idx_letter = 0;           // 0 = showing the letter grid

// Which bucket a term belongs to: its first letter, or '#' for anything else.
static char index_bucket(const char *term)
{
    for (const char *p = term; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') return *p;
        if (*p >= 'a' && *p <= 'z') return (char)(*p - 'a' + 'A');
        if (*p > ' ' && (*p < '0' || *p > '9') && *p != '.') break;
        if (*p >= '0' && *p <= '9') return '#';
    }
    return '#';
}

// Parse index.json out of the embedded manual into a PSRAM array. Once per
// boot; ~60 KB, which has no business being on any task stack or in internal
// RAM (CLAUDE.md, "Audit every malloc under ~16 KB").
static void parse_index_file(void)
{
    if (s_idx || s_idx_n) return;
    const char *data = NULL;
    size_t n = 0;
    if (!manual_embed_get("index.json", &data, &n) || !data) {
        ESP_LOGW(TAG, "index: no index.json in the embedded manual");
        return;
    }
    char *buf = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    memcpy(buf, data, n);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    if (!root) { ESP_LOGW(TAG, "index: index.json did not parse"); return; }

    cJSON *terms = cJSON_GetObjectItem(root, "terms");
    if (cJSON_IsArray(terms)) {
        int cnt = cJSON_GetArraySize(terms);
        s_idx = heap_caps_malloc(sizeof(index_entry_t) * IDX_MAX,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_idx) { cJSON_Delete(root); return; }
        for (int i = 0; i < cnt && s_idx_n < IDX_MAX; i++) {
            cJSON *e = cJSON_GetArrayItem(terms, i);
            cJSON *t = cJSON_GetObjectItem(e, "t");
            cJSON *p = cJSON_GetObjectItem(e, "p");
            cJSON *a = cJSON_GetObjectItem(e, "a");
            if (!cJSON_IsString(t) || !cJSON_IsString(p)) continue;
            index_entry_t *d = &s_idx[s_idx_n++];
            snprintf(d->term,   sizeof(d->term),   "%s", t->valuestring);
            snprintf(d->path,   sizeof(d->path),   "%s", p->valuestring);
            snprintf(d->anchor, sizeof(d->anchor), "%s",
                     (cJSON_IsString(a) && a->valuestring) ? a->valuestring : "");
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "index: %d terms", s_idx_n);
}

static void index_cell_highlight(int k)
{
    if (k == s_idx_hi) return;
    if (s_idx_hi >= 0 && s_idx_hi < s_idx_cell_n)
        lv_obj_set_style_bg_opa(s_idx_cells[s_idx_hi], LV_OPA_TRANSP, 0);
    s_idx_hi = k;
    if (k >= 0 && k < s_idx_cell_n)
        lv_obj_set_style_bg_opa(s_idx_cells[k], LV_OPA_COVER, 0);
}

static void index_build(char letter);   // forward: the cells rebuild each other

static lv_obj_t *index_make_cell(lv_obj_t *parent, const char *text, int w, int h,
                                 bool big, int term_idx, char letter)
{
    if (s_idx_cell_n >= IDX_CELL_MAX) return NULL;
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_style_bg_color(cell, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);     // highlight paints this
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_radius(cell, 6, 0);
    lv_obj_set_style_pad_all(cell, 4, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);      // the panel does the picking
    lv_obj_t *lbl = lv_label_create(cell);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, w - 16);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, big ? &lv_font_montserrat_32 : &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(big ? 0xE8C060 : 0xFFFFFF), 0);
    lv_obj_center(lbl);
    s_idx_cell_term[s_idx_cell_n]   = term_idx;
    s_idx_cell_letter[s_idx_cell_n] = letter;
    s_idx_cells[s_idx_cell_n++]     = cell;
    return cell;
}

// letter == 0 -> the A-Z grid; otherwise that letter's terms.
static void index_build(char letter)
{
    if (!s_idx_panel) return;
    lv_obj_clean(s_idx_panel);
    s_idx_cell_n = 0;
    s_idx_hi = -1;
    s_idx_letter = letter;
    lv_obj_scroll_to_y(s_idx_panel, 0, LV_ANIM_OFF);

    if (letter == 0) {
        // Letter grid. Only buckets that actually hold something are offered -
        // a dead letter is a promise the index cannot keep.
        const int CW = 120, CH = 84, COLS = 8, GAP = 12;
        const char *alphabet = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        int shown = 0;
        for (const char *L = alphabet; *L; L++) {
            bool any = false;
            for (int i = 0; i < s_idx_n; i++)
                if (index_bucket(s_idx[i].term) == *L) { any = true; break; }
            if (!any) continue;
            char txt[2] = { *L, 0 };
            lv_obj_t *c = index_make_cell(s_idx_panel, txt, CW, CH, true, -1, *L);
            if (c) lv_obj_set_pos(c, (shown % COLS) * (CW + GAP) + 8,
                                     (shown / COLS) * (CH + GAP) + 8);
            shown++;
        }
        if (s_title_lbl) lv_label_set_text(s_title_lbl, "Index");
        return;
    }

    // Terms for one letter. First cell goes back to the grid, so the way out is
    // always the same gesture as the way in.
    const int W = SCR_W - 96, H = 56;
    int row = 0;
    lv_obj_t *back = index_make_cell(s_idx_panel, LV_SYMBOL_LEFT "  All letters",
                                     W, H, false, -1, 0);
    if (back) lv_obj_set_pos(back, 8, 8);
    row++;
    for (int i = 0; i < s_idx_n && s_idx_cell_n < IDX_CELL_MAX; i++) {
        if (index_bucket(s_idx[i].term) != letter) continue;
        lv_obj_t *c = index_make_cell(s_idx_panel, s_idx[i].term, W, H, false, i, 0);
        if (c) lv_obj_set_pos(c, 8, row * (H + 6) + 8);
        row++;
    }
    if (s_title_lbl) {
        char t[16];
        snprintf(t, sizeof(t), "Index - %c", letter);
        lv_label_set_text(s_title_lbl, t);
    }
}

// Open (or close) the index. Closing the Contents panel first, since only one
// of the two may be up.
static void index_panel_set_open(bool open)
{
    if (!s_idx_panel) return;
    if (open) {
        parse_index_file();
        if (s_toc_panel) lv_obj_add_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);
        index_build(0);
        lv_obj_clear_flag(s_idx_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_idx_panel);
    } else {
        lv_obj_add_flag(s_idx_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_title_lbl) lv_label_set_text(s_title_lbl, s_page_title);
    }
}

static void index_btn_cb(lv_event_t *e)
{
    (void)e;
    bool up = s_idx_panel && !lv_obj_has_flag(s_idx_panel, LV_OBJ_FLAG_HIDDEN);
    index_panel_set_open(!up);
}

// Same drag-to-pick as the Contents panel: highlight follows the finger, the
// release commits. Attached to the panel because the cells are inert.
static void index_drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_event_get_indev(e);
        if (!indev) return;
        lv_point_t p; lv_indev_get_point(indev, &p);
        int hit = -1;
        for (int k = 0; k < s_idx_cell_n; k++) {
            lv_area_t a; lv_obj_get_coords(s_idx_cells[k], &a);
            if (p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2) { hit = k; break; }
        }
        index_cell_highlight(hit);
    } else if (code == LV_EVENT_RELEASED) {
        int k = s_idx_hi;
        index_cell_highlight(-1);
        if (k < 0 || k >= s_idx_cell_n) return;
        int ti = s_idx_cell_term[k];
        char lt = s_idx_cell_letter[k];
        if (ti < 0) {                       // a letter cell, or "All letters"
            index_build(lt);                // lt == 0 rebuilds the grid
            return;
        }
        if (ti < s_idx_n) {
            lv_obj_add_flag(s_idx_panel, LV_OBJ_FLAG_HIDDEN);
            snprintf(s_want_anchor, sizeof(s_want_anchor), "%s", s_idx[ti].anchor);
            s_anchor_reported = false;
            s_anchor_obj = NULL;
            navigate_to(s_idx[ti].path, NULL);
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        index_cell_highlight(-1);
    }
}

// One TOC entry into column `col`. Style driven purely by nav depth (level):
//   level 0  -> gold 32 px  (section headers AND top pages read identically)
//   level >0 -> white 24 px (nested pages)
// Header labels get the same left pad as page cells so their text left-aligns.
static void make_toc_entry(lv_obj_t *col, int i)
{
    toc_entry_t *e = &s_toc[i];
    bool top = (e->level == 0);
    const lv_font_t *font = top ? &lv_font_montserrat_32 : &lv_font_montserrat_24;
    uint32_t color = top ? UI_COLOR_ACCENT_GOLD : UI_COLOR_TEXT;

    int indent = (e->level > 0) ? e->level * 24 : 0;   // nested pages indent under their section

    if (e->path[0] == '\0') {          // section header
        lv_obj_t *l = lv_label_create(col);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        lv_obj_set_style_pad_top(l, 10, 0);
        lv_obj_set_style_pad_left(l, 10, 0);   // align with the page cells' text
        lv_label_set_text(l, fold_static(e->title));
        return;
    }
    // Page cell. Inert (not individually clickable) — the panel drives a
    // drag-to-pick highlight and navigates on release. Registered in s_toc_cells.
    lv_obj_t *cell = lv_obj_create(col);
    lv_obj_remove_style_all(cell);
    lv_obj_set_width(cell, LV_PCT(100));
    lv_obj_set_height(cell, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(cell, 10 + indent, 0);
    lv_obj_set_style_pad_right(cell, 10, 0);
    lv_obj_set_style_pad_ver(cell, 7, 0);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x33404d), 0);   // colour used when highlighted
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);              // invisible until highlighted
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    if (s_toc_cell_n < TOC_MAX) { s_toc_cells[s_toc_cell_n] = cell; s_toc_cell_idx[s_toc_cell_n] = i; s_toc_cell_n++; }
    lv_obj_t *l = lv_label_create(cell);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, fold_static(e->title[0] ? e->title : e->path));
}

static lv_obj_t *make_toc_column(int w)
{
    lv_obj_t *c = lv_obj_create(s_toc_panel);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, w);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);   // let the panel receive the drag
    return c;
}

// Rebuild the contents panel from s_toc[]. Two side-by-side columns; entries are
// split between them at a SECTION boundary (a level-0 entry) near the halfway
// point, so a section is never torn across columns and reading order (col 1
// top->bottom, then col 2) is preserved. LVGL thread only.
static void rebuild_toc_panel(void)
{
    if (!s_toc_panel) return;
    lv_obj_clean(s_toc_panel);
    s_toc_cell_n = 0;
    s_toc_hi = -1;
    if (s_toc_n == 0) {
        lv_obj_t *l = lv_label_create(s_toc_panel);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_label_set_text(l, "Contents unavailable (connect to WiFi to download).");
        return;
    }
    const int COL_W    = (SCR_W - 2 * 40 - 40) / 2;              // two columns + centre gap
    const int USABLE_H = SCR_H - HEADER_H - 2 * 28 - 8;          // panel height without scrolling

    // Fill column 1 with WHOLE sections (a level-0 entry + its nested pages)
    // until the next section wouldn't fit; that section starts column 2. Keeps
    // sections intact and avoids vertical scrolling.
    int col1_h = 0, split = s_toc_n, i = 0;
    while (i < s_toc_n) {
        int bstart = i, bh = 0;
        do { bh += (s_toc[i].level == 0) ? 52 : 40; i++; } while (i < s_toc_n && s_toc[i].level != 0);
        if (col1_h > 0 && col1_h + bh > USABLE_H) { split = bstart; break; }
        col1_h += bh;
    }

    lv_obj_t *col1 = make_toc_column(COL_W);
    lv_obj_t *col2 = make_toc_column(COL_W);
    for (int j = 0; j < s_toc_n; j++) make_toc_entry(j < split ? col1 : col2, j);
}

// Highlight cell k (index into s_toc_cells), clearing the previous highlight.
// k = -1 clears.
static void toc_cell_highlight(int k)
{
    if (k == s_toc_hi) return;
    if (s_toc_hi >= 0 && s_toc_hi < s_toc_cell_n)
        lv_obj_set_style_bg_opa(s_toc_cells[s_toc_hi], LV_OPA_TRANSP, 0);
    s_toc_hi = k;
    if (k >= 0 && k < s_toc_cell_n)
        lv_obj_set_style_bg_opa(s_toc_cells[k], LV_OPA_COVER, 0);
}

// Drag-to-pick: while pressed, highlight whichever page cell is under the
// finger; on release, navigate to the highlighted one. Attached to the panel
// (cells/columns are inert) so a finger can slide between entries.
static void toc_drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_event_get_indev(e);
        if (!indev) return;
        lv_point_t p; lv_indev_get_point(indev, &p);
        int hit = -1;
        for (int k = 0; k < s_toc_cell_n; k++) {
            lv_area_t a; lv_obj_get_coords(s_toc_cells[k], &a);
            if (p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2) { hit = k; break; }
        }
        toc_cell_highlight(hit);
    } else if (code == LV_EVENT_RELEASED) {
        int k = s_toc_hi;
        toc_cell_highlight(-1);
        if (k >= 0 && k < s_toc_cell_n) {
            int ti = s_toc_cell_idx[k];
            navigate_to(s_toc[ti].path, s_toc[ti].title);
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        toc_cell_highlight(-1);
    }
}

static void toc_panel_set_open(bool open)
{
    if (!s_toc_panel) return;
    if (open) {
        lv_obj_scroll_to_y(s_toc_panel, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_toc_panel);
        if (s_title_lbl) lv_label_set_text(s_title_lbl, "Contents");
    } else {
        lv_obj_add_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_title_lbl) lv_label_set_text(s_title_lbl, s_page_title);   // restore page title
    }
}

// Simple page-visit history so "Back" returns to the previous page.
static char s_hist[16][96];
static int  s_hist_n = 0;

// Show "Back" only when it would actually do something. With no history and the
// contents panel closed it is a dead button sitting next to a live "Exit", and the
// operator is left working out which of the two leaves the manual - the exact
// confusion reported on 2026-08-06. When hidden, Exit slides into its slot so there
// is no gap where a button used to be.
static void nav_buttons_sync(void)
{
    const int BTN_Y = (HEADER_H - 46) / 2;
    bool toc_open  = s_toc_panel && !lv_obj_has_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);
    bool want_back = (s_hist_n > 0) || toc_open;   // Back also closes the contents panel
    if (s_back_btn) {
        if (want_back) lv_obj_clear_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_exit_btn) lv_obj_align(s_exit_btn, LV_ALIGN_TOP_LEFT, want_back ? 190 : 40, BTN_Y);
}

// Load and display a page. `push` records the page we're leaving onto the
// history stack (Back replays it with push=false). LVGL thread only.
static void load_page(const char *rel, const char *title_hint, bool push)
{
    if (!rel || !rel[0]) return;
    if (push && s_current_path[0] && strcmp(s_current_path, rel) != 0 && s_hist_n < 16)
        snprintf(s_hist[s_hist_n++], sizeof(s_hist[0]), "%s", s_current_path);
    snprintf(s_current_path, sizeof(s_current_path), "%s", rel);
    // A deliberate navigation (contents pick, in-page link, Back) means "top of
    // that page" - so retire any context-help anchor still armed from a help jump,
    // or it would keep yanking the scroll down on an unrelated page.
    s_want_anchor[0] = 0;
    s_anchor_reported = false;
    toc_panel_set_open(false);
    lv_obj_clean(s_body);
    add_label("Loading...", &lv_font_montserrat_24, UI_COLOR_TEXT_MUTED, 0, 0);
    if (title_hint) set_page_title(title_hint);
    // NOT "Downloading..." - nothing is downloaded. The page comes out of the
    // embedded manual (render_from_cache), and a status line claiming a network
    // fetch is exactly the kind of leftover that makes an operator go looking for
    // a WiFi problem they do not have.
    lock(); s_status[0] = '\0'; unlock();
    reader_net_fetch(rel, false);
}

static void navigate_to(const char *rel, const char *title_hint)
{
    load_page(rel, title_hint, true);
}

static void contents_btn_cb(lv_event_t *e)
{
    (void)e;
    bool hidden = s_toc_panel && lv_obj_has_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);
    toc_panel_set_open(hidden);
}

// Back: close the contents panel if it's open; else go to the previous page in
// history. (Leaving the manual is the separate Exit button.)
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_toc_panel && !lv_obj_has_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN)) {
        toc_panel_set_open(false);
        return;
    }
    if (s_hist_n > 0) {
        char prev[96];
        snprintf(prev, sizeof(prev), "%s", s_hist[--s_hist_n]);
        load_page(prev, NULL, false);   // don't re-push
    }
}

void reader_view_open_help(const char *page_rel, const char *anchor)
{
    if (!page_rel || !page_rel[0]) return;
    snprintf(s_want_anchor, sizeof(s_want_anchor), "%s", anchor ? anchor : "");
    s_anchor_reported = false;
    s_anchor_obj = NULL;
    ESP_LOGI(TAG, "context help: %s%s%s", page_rel,
             anchor && anchor[0] ? " -> " : "", anchor ? anchor : "");

    // Point s_current_path at the target BEFORE show(), and let show()'s own
    // fetch load it. Do NOT show-then-fetch: reader_view_show() already issues
    // reader_net_fetch(s_current_path, true) for whatever page was last open, and
    // reader_net_fetch() is a NO-OP while a load is in flight - so a second call
    // here was silently dropped and every context-help tap landed on the index
    // page instead (operator-reported, 2026-08-06: "all I have tried send me to
    // the homepage"). One fetch, of the right page, is the whole fix.
    snprintf(s_current_path, sizeof(s_current_path), "%s", page_rel);
    reader_view_show();
}

// Open the Reader straight onto the A-Z index. This is what the "Need
// guidance?" panel's bottom button does: the operator already knows the word
// they are looking for, and the contents list is the wrong shape for that.
void reader_view_open_index(void)
{
    s_want_anchor[0] = '\0';
    reader_view_show();
    index_panel_set_open(true);
}

// Exit: leave the Reader entirely (returns to whatever mode was underneath).
static void exit_btn_cb(lv_event_t *e)
{
    (void)e;
    reader_view_hide();
}

// ============================ LVGL timer ============================

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    bool do_reload = false; bool from_cache = false; bool do_toc = false;
    char status[64]; char ver[24];
    lock();
    if (s_reload_pending) { s_reload_pending = false; do_reload = true; from_cache = s_from_cache; }
    if (s_toc_reload_pending) { s_toc_reload_pending = false; do_toc = true; }
    strncpy(status, s_status, sizeof(status)); status[sizeof(status)-1] = '\0';
    strncpy(ver, s_update_ver, sizeof(ver)); ver[sizeof(ver)-1] = '\0';
    unlock();

    if (do_toc)    { parse_toc_file(); rebuild_toc_panel(); }
    if (do_reload) render_from_cache();
    nav_buttons_sync();   // history/contents state changes on navigation

    // The "Save offline" button is gone: the manual is built into the firmware, so
    // it is always available and there is nothing to save, download or restart
    // for. See net/manual_embed.h.

    if (s_status_lbl) {
        // No "Offline - showing cached copy" fallback any more: there is no online
        // case to be offline from, and no cached copy - every page is read out of
        // the firmware. Whatever status a caller set (or nothing) is the truth.
        lv_label_set_text(s_status_lbl, status);
    }

    if (s_banner) {
        if (ver[0]) {
            char msg[96];
            snprintf(msg, sizeof(msg), LV_SYMBOL_DOWNLOAD "  Firmware %s available - see Releases on GitHub", ver);
            if (s_banner_lbl) lv_label_set_text(s_banner_lbl, msg);
            lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================ build / show / hide ============================

void reader_view_init(lv_obj_t *parent)
{
    if (s_overlay) return;
    s_lock = xSemaphoreCreateMutex();

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_overlay, SCR_W, 0);   // parked off-screen right
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0a0d10), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);   // swallow touches (inert) so gestures behind it can't fire

    // Header strip
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

    // Title + status live IN the header bar (labels, non-interactive). Title is
    // gold and H1-sized to match the document's main heading.
    s_title_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_LEFT_MID, 520, 0);
    lv_label_set_text(s_title_lbl, "Documentation");

    // Status: a FIXED two-line wrapping box in the gap between the gold title
    // (starts x=520, can run to ~860 for long page titles) and the Save-offline
    // button (left edge ~1044). The old single-line right-aligned label grew
    // LEFTWARD from x=980 and ran under the title (operator report). Height
    // stays within the 64 px bar (2 lines at font 18 ≈ 44 px); anything longer
    // than two lines is dot-truncated rather than overflowing the bar.
    s_status_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_size(s_status_lbl, 170, 46);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_RIGHT_MID, -240, 0);
    lv_label_set_text(s_status_lbl, "");

    // The three buttons are children of the full-screen OVERLAY, not the header
    // bar. LVGL clips a child's hit area to its parent, so buttons inside the
    // 64 px bar couldn't be tapped from just below it; parenting them to the
    // overlay lets ext_click_area extend down past the bar. Foregrounded above
    // the body at the end of init so those extended areas win the touch.
    const int BTN_H = 46, BTN_Y = (HEADER_H - 46) / 2;

    // Back (previous page in history). Kept clear of the ~30 px left-edge
    // exit-swipe strip.
    lv_obj_t *back_btn = lv_button_create(s_overlay);
    s_back_btn = back_btn;
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 40, BTN_Y);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_pad_hor(back_btn, 20, 0);
    lv_obj_set_height(back_btn, BTN_H);
    lv_obj_set_ext_click_area(back_btn, 44);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  Back");

    // Exit (leaves the Reader). Same style/padding as Back.
    lv_obj_t *exit_btn = lv_button_create(s_overlay);
    s_exit_btn = exit_btn;
    lv_obj_align(exit_btn, LV_ALIGN_TOP_LEFT, 190, BTN_Y);
    lv_obj_set_style_bg_color(exit_btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_pad_hor(exit_btn, 20, 0);
    lv_obj_set_height(exit_btn, BTN_H);
    lv_obj_set_ext_click_area(exit_btn, 44);
    lv_obj_add_event_cb(exit_btn, exit_btn_cb, LV_EVENT_CLICKED, NULL);
    // Esc leaves the manual. No Enter: there is nothing here to confirm, and a
    // reader is exactly where a keyboard user expects Esc to work. Scrolling
    // with the arrows and PgUp/PgDn is handled in ui.c's kbd_text_cb, which can
    // see whether this overlay is the thing on screen.
    ui_kbd_set_buttons(NULL, exit_btn);
    lv_obj_t *exit_lbl = lv_label_create(exit_btn);
    lv_obj_set_style_text_font(exit_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(exit_lbl, LV_SYMBOL_CLOSE "  Exit");

    // Contents (opens the TOC panel). Same style/padding as Back/Exit, blue bg,
    // no icon.
    s_toc_btn = lv_button_create(s_overlay);
    lv_obj_align(s_toc_btn, LV_ALIGN_TOP_LEFT, 330, BTN_Y);
    lv_obj_set_style_bg_color(s_toc_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_pad_hor(s_toc_btn, 20, 0);
    lv_obj_set_height(s_toc_btn, BTN_H);
    lv_obj_set_ext_click_area(s_toc_btn, 44);
    lv_obj_add_event_cb(s_toc_btn, contents_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *toc_btn_lbl = lv_label_create(s_toc_btn);
    lv_obj_set_style_text_font(toc_btn_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(toc_btn_lbl, "Contents");

    // A-Z index, beside Contents. Contents answers "what is in here"; this one
    // answers "where is the thing I already know the name of".
    s_idx_btn = lv_button_create(s_overlay);
    // Right-hand end, not beside Contents: the gold page title is anchored at
    // x=520 and a button at 500 lands underneath it. The old "Save offline"
    // slot over here is free, and Back/Exit/Contents on the left with A-Z on
    // the right splits "move around the book" from "look something up".
    lv_obj_align(s_idx_btn, LV_ALIGN_TOP_RIGHT, -40, BTN_Y);
    lv_obj_set_style_bg_color(s_idx_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_pad_hor(s_idx_btn, 20, 0);
    lv_obj_set_height(s_idx_btn, BTN_H);
    lv_obj_set_ext_click_area(s_idx_btn, 44);
    lv_obj_add_event_cb(s_idx_btn, index_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *idx_btn_lbl = lv_label_create(s_idx_btn);
    lv_obj_set_style_text_font(idx_btn_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(idx_btn_lbl, "A-Z");

    // Save offline (right end, SD only). Label flips to "Saved offline - update?"
    // (The "Save offline" button used to live here, top-right. Removed: the manual
    // is built into the firmware, so there is nothing to download or save.)

    // Update-available banner (hidden until update_check reports one)
    s_banner = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, SCR_W, BANNER_H);
    lv_obj_set_pos(s_banner, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0x5a4300), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    s_banner_lbl = lv_label_create(s_banner);
    lv_obj_set_style_text_font(s_banner_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_banner_lbl, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_align(s_banner_lbl, LV_ALIGN_LEFT_MID, 20, 0);
    lv_label_set_text(s_banner_lbl, "");

    // Scrollable body
    s_body = lv_obj_create(s_overlay);
    // Reading a manual is the clearest case for arrow scrolling there is.
    ui_kbd_add_scrollable(s_body);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, SCR_W, SCR_H - HEADER_H);
    lv_obj_set_pos(s_body, 0, HEADER_H);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(s_body, BODY_PAD_X, 0);
    lv_obj_set_style_pad_right(s_body, BODY_PAD_X, 0);
    lv_obj_set_style_pad_top(s_body, BODY_PAD_Y, 0);
    lv_obj_set_style_pad_bottom(s_body, BODY_PAD_Y * 2, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_AUTO);

    add_label("Loading documentation...", &lv_font_montserrat_24, UI_COLOR_TEXT_MUTED, 0, 0);

    // Contents panel: opaque scrollable overlay covering the body when open.
    // Built after the body so it sits above it; hidden by default.
    s_toc_panel = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_toc_panel);
    lv_obj_set_size(s_toc_panel, SCR_W, SCR_H - HEADER_H);
    lv_obj_set_pos(s_toc_panel, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_toc_panel, lv_color_hex(0x0a0d10), 0);
    lv_obj_set_style_bg_opa(s_toc_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_toc_panel, 40, 0);
    lv_obj_set_style_pad_top(s_toc_panel, 28, 0);      // air below the top bar
    lv_obj_set_style_pad_bottom(s_toc_panel, 28, 0);   // air at the bottom
    // Two side-by-side columns (a flex ROW of two flex-COLUMN containers). The
    // entries are split between them at a SECTION boundary (see rebuild) so a
    // section is never torn across columns, and reading order is preserved.
    lv_obj_set_flex_flow(s_toc_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_toc_panel, 40, 0);   // gap between the two columns
    lv_obj_clear_flag(s_toc_panel, LV_OBJ_FLAG_SCROLLABLE);   // fits without scrolling; drag = pick, not scroll
    lv_obj_add_flag(s_toc_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_toc_panel, toc_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_toc_panel, toc_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_toc_panel, toc_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_toc_panel, toc_drag_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_set_scrollbar_mode(s_toc_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);

    // A-Z index panel (Layer 4). Same surface and same drag-to-pick as Contents,
    // but absolutely positioned and SCROLLABLE - a busy letter runs past the
    // bottom of the screen, which the two-column contents never does.
    s_idx_panel = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_idx_panel);
    lv_obj_set_size(s_idx_panel, SCR_W, SCR_H - HEADER_H);
    lv_obj_set_pos(s_idx_panel, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_idx_panel, lv_color_hex(0x0a0d10), 0);
    lv_obj_set_style_bg_opa(s_idx_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_idx_panel, 40, 0);
    lv_obj_set_style_pad_ver(s_idx_panel, 20, 0);
    lv_obj_add_flag(s_idx_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_idx_panel, index_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_idx_panel, index_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_idx_panel, index_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_idx_panel, index_drag_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_set_scrollbar_mode(s_idx_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_idx_panel, LV_OBJ_FLAG_HIDDEN);

    // Keep the header bar and its buttons above the body so the buttons' large
    // ext_click_area — which reaches BELOW the bar — wins the touch there
    // instead of the body swallowing it.
    lv_obj_move_foreground(hdr);
    lv_obj_move_foreground(back_btn);
    lv_obj_move_foreground(exit_btn);
    lv_obj_move_foreground(s_toc_btn);
    // A-Z too. hdr is raised above the whole overlay two lines up, so any
    // header control missing from this list is painted over by the bar it
    // sits in - which is precisely how this one went invisible while
    // reporting a perfectly good x/y/w/h and hidden=0.
    if (s_idx_btn) lv_obj_move_foreground(s_idx_btn);
    if (s_banner) lv_obj_move_foreground(s_banner);

    s_timer = lv_timer_create(tick_cb, 250, NULL);
    ESP_LOGI(TAG, "init");
}

void reader_view_show(void)
{
    if (!s_overlay) return;

    toc_panel_set_open(false);
    s_hist_n = 0;   // fresh navigation history each time the manual is opened

    // RENDER BEFORE SHOWING. The overlay used to be made visible and slid in while
    // s_body still held the PREVIOUS visit's widget tree, with the new page only
    // arriving on a later timer tick - so opening a help link visibly rendered one
    // or two wrong pages before settling on the right one. That was a leftover from
    // when pages came over WiFi and there was genuinely nothing to draw yet; now the
    // page is in the firmware, so it can be built synchronously here and the very
    // first visible frame is the correct one.
    parse_toc_file();
    rebuild_toc_panel();
    render_from_cache();
    nav_buttons_sync();
    lock();
    s_reload_pending = false;      // already rendered - do not repaint and lose the anchor
    s_toc_reload_pending = false;
    s_status[0] = 0;           // nothing to fetch: the manual is in the firmware
    unlock();

    // APPEAR IN ONE FRAME - no slide. Landscape runs at ~13 fps (every flush goes
    // through the software 90-degree rotation), so the old 220 ms slide was about
    // THREE frames: not motion, just two or three discrete snapshots of the page at
    // intermediate offsets. That is what read as a "rendering flicker offset to the
    // left", and why it varied - it depends where the frames fall in the animation.
    // A cross-fade would be the same three frames at 33/66/100% opacity, i.e. a
    // flash rather than a dissolve, so animating differently is not the answer on
    // this display. One correct frame is.
    lv_obj_set_x(s_overlay, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_active = true;

    // Stand the Panadapter's touch navigation down. Without this the top-bar
    // Band/Mode/BW hit zones - direct children of the screen, foregrounded above
    // this overlay - swallow taps aimed at our own Back/Exit/Contents buttons, so
    // the manual could be opened but not left.
    ui_help_overlay_changed();
    ESP_LOGI(TAG, "show (page=%s)", s_current_path);
}


void reader_view_hide(void)
{
    if (!s_overlay) return;
    s_active = false;
    ui_help_overlay_changed();   // hand the top bar and edge swipes back
    // Disappear in one frame, for the same reason show() appears in one (see there).
    // Sliding out was worse than sliding in: the page being dragged across the
    // screen at ~13 fps looked like an unrelated page surfacing before the FT8
    // screen came back.
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(s_overlay, 0);
    ESP_LOGI(TAG, "hide");
}

bool reader_view_is_active(void) { return s_active; }
lv_obj_t *reader_view_get_container(void) { return s_overlay; }

// ---- cross-task notifications ----

void reader_view_notify_loaded(bool from_cache)
{
    lock();
    s_reload_pending = true;
    s_from_cache = from_cache;
    if (!from_cache) s_status[0] = '\0';   // fresh: clear "Refreshing…"
    unlock();
}

void reader_view_notify_status(const char *status)
{
    lock();
    strncpy(s_status, status ? status : "", sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
    unlock();
}

void reader_view_notify_toc_loaded(void)
{
    lock();
    s_toc_reload_pending = true;
    unlock();
}

void reader_view_set_update_available(const char *latest_version)
{
    lock();
    strncpy(s_update_ver, latest_version ? latest_version : "", sizeof(s_update_ver) - 1);
    s_update_ver[sizeof(s_update_ver) - 1] = '\0';
    unlock();
}
