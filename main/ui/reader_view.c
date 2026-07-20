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
#include "reader_net.h"
#include "ui_theme.h"
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
#define SLIDE_TIME_MS  220

#define READER_CACHE_PATH  "/spiffs/reader.md"
#define TOC_CACHE_PATH     "/spiffs/reader_toc.json"
#define MD_MAX_BYTES       (96 * 1024)   // cache-file read cap

// Table of contents (parsed from toc.json published by mkdocs_reader_export.py).
#define TOC_MAX  64
typedef struct { char title[48]; char path[96]; int level; } toc_entry_t;
static toc_entry_t s_toc[TOC_MAX];
static int  s_toc_n = 0;
static char s_current_path[96] = "index.md";     // page currently shown
static char s_page_title[64]   = "Documentation"; // title shown when TOC is closed

// ---- LVGL objects (LVGL thread only) ----
static lv_obj_t *s_overlay      = NULL;   // full-screen opaque page
static lv_obj_t *s_title_lbl    = NULL;   // header: page title
static lv_obj_t *s_status_lbl   = NULL;   // header: right-aligned status
static lv_obj_t *s_banner       = NULL;   // update-available bar (hidden unless set)
static lv_obj_t *s_banner_lbl   = NULL;
static lv_obj_t *s_body         = NULL;   // scrollable flex column of content
static lv_obj_t *s_toc_btn      = NULL;   // header "Contents" button
static lv_obj_t *s_save_btn     = NULL;   // header "Save offline" button (SD only)
static lv_obj_t *s_save_lbl     = NULL;   // its label (text toggles Save/Saved)
static lv_obj_t *s_toc_panel    = NULL;   // scrollable contents overlay (hidden unless open)
static lv_timer_t *s_timer      = NULL;

static bool s_active = false;

// ---- Cross-task state (guarded by s_lock) ----
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_reload_pending = false;
static volatile bool s_toc_reload_pending = false;
static bool s_from_cache = false;
static char s_status[64]  = {0};
static char s_update_ver[24] = {0};
static volatile int s_save_state = 0;   // 0 idle, 1 saved-ok, 2 saved-failed

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// Set the current page's title. Reflected on the top bar only while the TOC
// panel is closed (when it's open the bar shows "Contents").
static void set_page_title(const char *t)
{
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
    // Box Drawing U+2500-257F -> -|+
    if (cp >= 0x2500 && cp <= 0x257F) {
        if      (cp == 0x2500 || cp == 0x2501) *rep = "-";
        else if (cp == 0x2502 || cp == 0x2503) *rep = "|";
        else                                   *rep = "+";
        return len;
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
                    for (const char *p = src + i + 2; p < close && o + 1 < dstsz; p++) dst[o++] = *p;
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
                    for (const char *p = src + i + 1; p < close && o + 1 < dstsz; p++) dst[o++] = *p;
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
    lv_label_set_text(l, text && text[0] ? text : " ");
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
                    for (const char *q = src + i + 2; q < close && o + 1 < sizeof(seg); q++) seg[o++] = *q;
                    i = (size_t)(paren - src) + 1; continue;
                }
            }
        }
        if (c == '[') {                                          // link [text](url) -> text
            const char *close = strchr(src + i + 1, ']');
            if (close && close[1] == '(') {
                const char *paren = strchr(close, ')');
                if (paren) {
                    for (const char *q = src + i + 1; q < close && o + 1 < sizeof(seg); q++) seg[o++] = *q;
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
            if (in_code) { code[code_len] = '\0'; add_code_block(code); code_len = 0; block_count++; in_code = false; }
            else         { FLUSH_PARA(); in_code = true; code_len = 0; }
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
                add_label(cleaned, f, col, block_count ? 22 : 4, 0);
                block_count++;
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
    lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
    ESP_LOGI(TAG, "rendered %d blocks", block_count);
    heap_caps_free(S);   // title already copied into the label above
}

// Read the cache file and render it. LVGL thread only.
static void render_from_cache(void)
{
    FILE *f = fopen(READER_CACHE_PATH, "rb");
    if (!f) {
        lv_obj_clean(s_body);
        add_label("No documentation cached yet.\n\nConnect to WiFi and swipe back "
                  "into this page to download it from tab5.lav.dk.",
                  &lv_font_montserrat_24, UI_COLOR_TEXT_MUTED, 0, 0);
        set_page_title("Documentation");
        return;
    }
    char *buf = heap_caps_malloc(MD_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); ESP_LOGW(TAG, "OOM rendering cache"); return; }
    size_t n = fread(buf, 1, MD_MAX_BYTES - 1, f);
    fclose(f);
    buf[n] = '\0';
    render_markdown(buf);
    heap_caps_free(buf);
}

// ============================ table of contents ============================

// Parse /spiffs/reader_toc.json ({"pages":[{title,path,level}]}) into s_toc[].
// LVGL thread only.
static void parse_toc_file(void)
{
    s_toc_n = 0;
    FILE *f = fopen(TOC_CACHE_PATH, "rb");
    if (!f) return;
    char *buf = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, 16 * 1024 - 1, f);
    fclose(f);
    buf[n] = '\0';

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

// A contents row was tapped: user_data holds the s_toc index.
static void toc_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    if (i < 0 || i >= s_toc_n) return;
    navigate_to(s_toc[i].path, s_toc[i].title);
}

// Rebuild the contents panel from s_toc[]. LVGL thread only.
static void rebuild_toc_panel(void)
{
    if (!s_toc_panel) return;
    lv_obj_clean(s_toc_panel);
    if (s_toc_n == 0) {
        lv_obj_t *l = lv_label_create(s_toc_panel);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        lv_label_set_text(l, "Contents unavailable (connect to WiFi to download).");
        return;
    }
    // Layout: TOP-LEVEL entries (level 0) span the full width and are gold —
    // both the section headers (User Guide/Reference/...) AND the standalone
    // top pages (Home/Quick Start/Releases), so they read at the same level.
    // NESTED pages (level > 0) pack two-across in white. Tight spacing so the
    // whole tree fits without scrolling.
    for (int i = 0; i < s_toc_n; i++) {
        toc_entry_t *e = &s_toc[i];
        bool top = (e->level == 0);
        if (e->path[0] == '\0') {
            // section header — full-width, not tappable
            lv_obj_t *l = lv_label_create(s_toc_panel);
            lv_obj_set_width(l, LV_PCT(100));
            lv_obj_set_style_text_font(l, &lv_font_montserrat_32, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
            lv_obj_set_style_pad_top(l, 8, 0);
            lv_label_set_text(l, e->title);
            continue;
        }
        lv_obj_t *cell = lv_obj_create(s_toc_panel);
        lv_obj_remove_style_all(cell);
        lv_obj_set_width(cell, top ? LV_PCT(100) : LV_PCT(48));   // top pages full-width like headers
        lv_obj_set_height(cell, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(cell, 9, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(UI_COLOR_SURFACE_RAISED), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_30, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(cell, 8, 0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(cell, (void *)(intptr_t)i);
        lv_obj_add_event_cb(cell, toc_row_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(cell);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_font(l, top ? &lv_font_montserrat_28 : &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(top ? UI_COLOR_ACCENT_GOLD : UI_COLOR_TEXT), 0);
        lv_label_set_text(l, e->title[0] ? e->title : e->path);
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

// Load and display a specific page. LVGL thread only.
static void navigate_to(const char *rel, const char *title_hint)
{
    if (!rel || !rel[0]) return;
    snprintf(s_current_path, sizeof(s_current_path), "%s", rel);
    toc_panel_set_open(false);
    lv_obj_clean(s_body);
    add_label("Loading...", &lv_font_montserrat_24, UI_COLOR_TEXT_MUTED, 0, 0);
    if (title_hint) set_page_title(title_hint);
    lock(); strncpy(s_status, "Downloading...", sizeof(s_status) - 1); unlock();
    reader_net_fetch(rel, false);
}

static void contents_btn_cb(lv_event_t *e)
{
    (void)e;
    bool hidden = s_toc_panel && lv_obj_has_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);
    toc_panel_set_open(hidden);
}

// "Save offline" — download the whole manual to the SD card.
static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    s_save_state = 0;   // back to "Save offline" while the run is in progress
    reader_net_save_offline();
}

// Back button: if the contents panel is open, close it first; otherwise close
// the whole Reader (returns to whatever mode was showing underneath).
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_toc_panel && !lv_obj_has_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN)) {
        toc_panel_set_open(false);
        return;
    }
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

    // "Save offline" is only meaningful with a card in the slot; its label
    // reflects the last save's outcome.
    if (s_save_btn) {
        if (sd_archive_is_mounted()) lv_obj_clear_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);
        else                         lv_obj_add_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_save_lbl) {
            const char *txt = (s_save_state == 1) ? LV_SYMBOL_SD_CARD "  Saved offline - update?"
                            : (s_save_state == 2) ? LV_SYMBOL_SD_CARD "  Save failed"
                                                  : LV_SYMBOL_SD_CARD "  Save offline";
            if (strcmp(lv_label_get_text(s_save_lbl), txt) != 0) lv_label_set_text(s_save_lbl, txt);
        }
    }

    if (s_status_lbl) {
        if (do_reload && from_cache && !status[0]) strncpy(status, "Offline - showing cached copy", sizeof(status)-1);
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
    lv_obj_align(s_title_lbl, LV_ALIGN_LEFT_MID, 460, 0);
    lv_label_set_text(s_title_lbl, "Documentation");

    s_status_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_RIGHT_MID, -300, 0);
    lv_label_set_text(s_status_lbl, "");

    // The three buttons are children of the full-screen OVERLAY, not the header
    // bar. LVGL clips a child's hit area to its parent, so buttons inside the
    // 64 px bar couldn't be tapped from just below it; parenting them to the
    // overlay lets ext_click_area extend down past the bar. Foregrounded above
    // the body at the end of init so those extended areas win the touch.
    const int BTN_H = 46, BTN_Y = (HEADER_H - 46) / 2;

    // Back (primary exit). Kept clear of the ~30 px left-edge exit-swipe strip.
    lv_obj_t *back_btn = lv_button_create(s_overlay);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 40, BTN_Y);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_pad_hor(back_btn, 20, 0);
    lv_obj_set_height(back_btn, BTN_H);
    lv_obj_set_ext_click_area(back_btn, 44);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  Back");

    // Contents (opens the TOC panel).
    s_toc_btn = lv_button_create(s_overlay);
    lv_obj_align(s_toc_btn, LV_ALIGN_TOP_LEFT, 205, BTN_Y);
    lv_obj_set_style_bg_color(s_toc_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_pad_hor(s_toc_btn, 20, 0);
    lv_obj_set_height(s_toc_btn, BTN_H);
    lv_obj_set_ext_click_area(s_toc_btn, 44);
    lv_obj_add_event_cb(s_toc_btn, contents_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *toc_btn_lbl = lv_label_create(s_toc_btn);
    lv_obj_set_style_text_font(toc_btn_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(toc_btn_lbl, LV_SYMBOL_LIST "  Contents");

    // Save offline (right end, SD only). Label flips to "Saved offline - update?"
    s_save_btn = lv_button_create(s_overlay);
    lv_obj_align(s_save_btn, LV_ALIGN_TOP_RIGHT, -16, BTN_Y);
    lv_obj_set_style_bg_color(s_save_btn, lv_color_hex(UI_COLOR_SUCCESS), 0);
    lv_obj_set_style_pad_hor(s_save_btn, 20, 0);
    lv_obj_set_height(s_save_btn, BTN_H);
    lv_obj_set_ext_click_area(s_save_btn, 44);
    lv_obj_add_event_cb(s_save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    s_save_lbl = lv_label_create(s_save_btn);
    lv_obj_set_style_text_font(s_save_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_save_lbl, LV_SYMBOL_SD_CARD "  Save offline");
    lv_obj_add_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_set_style_pad_ver(s_toc_panel, 16, 0);
    lv_obj_set_flex_flow(s_toc_panel, LV_FLEX_FLOW_ROW_WRAP);   // 2-column grid
    lv_obj_set_style_pad_column(s_toc_panel, 20, 0);
    lv_obj_set_style_pad_row(s_toc_panel, 6, 0);               // tight so it fits without scrolling
    lv_obj_set_scroll_dir(s_toc_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_toc_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);

    // Keep the header bar and its buttons above the body so the buttons' large
    // ext_click_area — which reaches BELOW the bar — wins the touch there
    // instead of the body swallowing it.
    lv_obj_move_foreground(hdr);
    lv_obj_move_foreground(back_btn);
    lv_obj_move_foreground(s_toc_btn);
    lv_obj_move_foreground(s_save_btn);
    if (s_banner) lv_obj_move_foreground(s_banner);

    s_timer = lv_timer_create(tick_cb, 250, NULL);
    ESP_LOGI(TAG, "init");
}

// Simple x-position animator for the slide.
static void anim_x_cb(void *var, int32_t v) { lv_obj_set_x((lv_obj_t *)var, v); }

static void slide(int32_t from, int32_t to)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_overlay);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, SLIDE_TIME_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void reader_view_show(void)
{
    if (!s_overlay) return;
    // Left-edge swipe drags RIGHT, so the page enters from the LEFT and follows
    // the finger (matches the Panadapter<->FT8 slide direction).
    lv_obj_set_x(s_overlay, -SCR_W);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_active = true;
    slide(-SCR_W, 0);

    toc_panel_set_open(false);
    // Render whatever is cached immediately (page + TOC), then kick a refresh of
    // the current page and the contents list.
    lock();
    s_reload_pending = true; s_from_cache = true;
    s_toc_reload_pending = true;
    strncpy(s_status, "Refreshing...", sizeof(s_status)-1);
    unlock();
    reader_net_fetch(s_current_path, true);
    ESP_LOGI(TAG, "show (page=%s)", s_current_path);
}

static void hide_done_cb(lv_anim_t *a)
{
    (void)a;
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void reader_view_hide(void)
{
    if (!s_overlay) return;
    s_active = false;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_overlay);
    lv_anim_set_exec_cb(&anim, anim_x_cb);
    // Exit slides off to the RIGHT (revealing the Panadapter underneath),
    // matching the FT8->Panadapter direction on the same rightward swipe.
    lv_anim_set_values(&anim, lv_obj_get_x(s_overlay), SCR_W);
    lv_anim_set_time(&anim, SLIDE_TIME_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&anim, hide_done_cb);
    lv_anim_start(&anim);
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

void reader_view_notify_saved(bool ok)
{
    s_save_state = ok ? 1 : 2;   // picked up by tick_cb -> button label
}

void reader_view_set_update_available(const char *latest_version)
{
    lock();
    strncpy(s_update_ver, latest_version ? latest_version : "", sizeof(s_update_ver) - 1);
    s_update_ver[sizeof(s_update_ver) - 1] = '\0';
    unlock();
}
