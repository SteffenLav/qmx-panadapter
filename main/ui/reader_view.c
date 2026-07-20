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
#define HEADER_H       56
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
static char s_current_path[96] = "index.md";   // page currently shown

// ---- LVGL objects (LVGL thread only) ----
static lv_obj_t *s_overlay      = NULL;   // full-screen opaque page
static lv_obj_t *s_title_lbl    = NULL;   // header: page title
static lv_obj_t *s_status_lbl   = NULL;   // header: right-aligned status
static lv_obj_t *s_banner       = NULL;   // update-available bar (hidden unless set)
static lv_obj_t *s_banner_lbl   = NULL;
static lv_obj_t *s_body         = NULL;   // scrollable flex column of content
static lv_obj_t *s_toc_btn      = NULL;   // header "Contents" button
static lv_obj_t *s_save_btn     = NULL;   // header "Save offline" button (SD only)
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

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ============================ markdown rendering ============================

// If `s` points at a UTF-8 sequence we fold to ASCII, return the number of input
// bytes consumed and set *rep to the replacement; else return 0. Covers the
// General Punctuation block (dashes/quotes/ellipsis/nbsp) and the Box Drawing
// block (─│┌┐└┘├… -> -|+) — the montserrat fonts have glyphs for none of these,
// so unfolded they render as tofu boxes (the ASCII-art layout diagrams in the
// docs were the worst offender).
static int fold_seq(const char *s, const char **rep)
{
    unsigned char c0 = (unsigned char)s[0];
    unsigned char c1 = c0 ? (unsigned char)s[1] : 0;
    unsigned char c2 = c1 ? (unsigned char)s[2] : 0;
    if (c0 == 0xC2 && c1 == 0xA0) { *rep = " "; return 2; }        // nbsp
    if (c0 == 0xE2 && c1 == 0x80) {                                // General Punctuation
        switch (c2) {
            case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: *rep = "-";   return 3; // hyphen/dashes
            case 0xa6:                                             *rep = "...";  return 3; // ellipsis
            case 0x98: case 0x99: case 0x9b:                       *rep = "'";   return 3; // single quotes
            case 0x9c: case 0x9d: case 0x9e:                       *rep = "\"";  return 3; // double quotes
            default:                                               *rep = "";     return 3; // drop other punct
        }
    }
    if (c0 == 0xE2 && c1 == 0x86) {                              // Arrows (subset)
        switch (c2) {
            case 0x90: *rep = "<-";  return 3;   // left
            case 0x91: *rep = "^";   return 3;   // up
            case 0x92: *rep = "->";  return 3;   // right
            case 0x93: *rep = "v";   return 3;   // down
            case 0x94: *rep = "<->"; return 3;   // left-right
            default:   *rep = "->";  return 3;
        }
    }
    if (c0 == 0xE2 && (c1 == 0x94 || c1 == 0x95)) {               // Box Drawing
        if      (c1 == 0x94 && (c2 == 0x80 || c2 == 0x81)) *rep = "-";
        else if (c1 == 0x94 && (c2 == 0x82 || c2 == 0x83)) *rep = "|";
        else                                               *rep = "+";
        return 3;
    }
    // Any other 3-byte E2/E3/EF symbol (checkmarks, bullets, misc symbols,
    // fullwidth/CJK forms) or 4-byte emoji: no font glyph -> drop rather than
    // render a tofu box.
    if (c0 == 0xE2 || c0 == 0xE3 || c0 == 0xEF) { *rep = ""; return 3; }
    if (c0 >= 0xF0)                              { *rep = ""; return 4; }
    return 0;
}

// Fold UTF-8 punctuation/box-drawing to ASCII in place (output is always <= input
// length for every mapping, so read/write cursors never cross). Used for code
// and table blocks, which bypass md_inline_clean.
static void fold_utf8_inplace(char *s)
{
    size_t r = 0, w = 0;
    while (s[r]) {
        const char *rep;
        int adv = fold_seq(&s[r], &rep);
        if (adv) { while (*rep) s[w++] = *rep++; r += (size_t)adv; }
        else     { s[w++] = s[r++]; }
    }
    s[w] = '\0';
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

// A code / preformatted block: distinct background, smaller font, no emphasis
// stripping (rendered verbatim).
static void add_code_block(const char *text)
{
    if (!s_body) return;
    lv_obj_t *box = lv_obj_create(s_body);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
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
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_label_set_text(l, text && text[0] ? text : " ");
}

// Render a markdown table (accumulated rows, newline-separated, mutated in
// place) as one bordered block: header row + data rows, cells separated, the
// header and each row's first cell in gold (via LVGL label recolor). Far nicer
// than the old one-bordered-box-per-row degrade. LVGL thread only.
static void add_table_block(char *rows)
{
    if (!s_body) return;
    fold_utf8_inplace(rows);

    lv_obj_t *box = lv_obj_create(s_body);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_style_margin_top(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

    char *out = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return;

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

        // Strip markdown emphasis (**bold** etc.) from each cell in place
        // (md_inline_clean only ever shrinks, so same-buffer is safe).
        for (int c = 0; c < nc; c++) md_inline_clean(cells[c], cells[c], strlen(cells[c]) + 1);

        size_t o = 0; out[0] = '\0';
        for (int c = 0; c < nc && o < 1000; c++) {
            const char *sep = (c < nc - 1) ? "     " : "";
            o += (size_t)snprintf(out + o, 1024 - o, "%s%s", cells[c], sep);
        }

        // Header row in gold, data rows in white (LVGL 9 dropped per-run label
        // recolor, so colour is whole-label).
        lv_obj_t *l = lv_label_create(box);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(rownum == 0 ? UI_COLOR_ACCENT_GOLD : UI_COLOR_TEXT), 0);
        lv_label_set_text(l, out);
        rownum++;
    }
    heap_caps_free(out);
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
            md_inline_clean(para, cleaned, MD_CLEANED_SZ);                 \
            add_label(cleaned, &lv_font_montserrat_24, UI_COLOR_TEXT,      \
                      block_count ? 14 : 0, 0);                            \
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
            if (in_code) { code[code_len] = '\0'; fold_utf8_inplace(code); add_code_block(code); code_len = 0; block_count++; in_code = false; }
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

        // horizontal rule
        if (strcmp(t, "---") == 0 || strcmp(t, "***") == 0 || strcmp(t, "___") == 0) {
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
            const char *q = skip_ws(t + 1);
            md_inline_clean(q, cleaned, MD_CLEANED_SZ);
            lv_obj_t *l = add_label(cleaned, &lv_font_montserrat_22, UI_COLOR_TEXT_SECONDARY, 8, 16);
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
                if (bullet) {
                    md_inline_clean(skip_ws(t + 1), cleaned, MD_CLEANED_SZ);
                    char withb[MD_LINE_SZ]; snprintf(withb, sizeof(withb), "%s  %s", LV_SYMBOL_BULLET, cleaned);
                    add_label(withb, &lv_font_montserrat_24, UI_COLOR_TEXT, 6, 20 + indent * 8);
                } else {
                    char num[8]; size_t k = 0;
                    for (const char *d = t; (isdigit((unsigned char)*d) || *d=='.'|| *d==')') && k < sizeof(num)-1; d++) num[k++] = *d;
                    num[k] = '\0';
                    md_inline_clean(skip_ws(nptr + 1), cleaned, MD_CLEANED_SZ);
                    char withn[MD_LINE_SZ]; snprintf(withn, sizeof(withn), "%s %s", num, cleaned);
                    add_label(withn, &lv_font_montserrat_24, UI_COLOR_TEXT, 6, 20 + indent * 8);
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
    if (in_code && code_len)  { code[code_len] = '\0'; fold_utf8_inplace(code); add_code_block(code); }
    if (in_table && code_len) { code[code_len] = '\0'; add_table_block(code); }

    #undef FLUSH_PARA

    if (s_title_lbl) lv_label_set_text(s_title_lbl, title[0] ? title : "Documentation");
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
        if (s_title_lbl) lv_label_set_text(s_title_lbl, "Documentation");
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
    for (int i = 0; i < s_toc_n; i++) {
        toc_entry_t *e = &s_toc[i];
        if (e->path[0] == '\0') {
            // section header — not tappable
            lv_obj_t *l = lv_label_create(s_toc_panel);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
            lv_obj_set_style_pad_top(l, 14, 0);
            lv_obj_set_style_pad_left(l, 8 + e->level * 24, 0);
            lv_label_set_text(l, e->title);
            continue;
        }
        lv_obj_t *row = lv_obj_create(s_toc_panel);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(row, 12, 0);
        lv_obj_set_style_pad_left(row, 24 + e->level * 24, 0);
        lv_obj_set_style_pad_right(row, 12, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, toc_row_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(row);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
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
    } else {
        lv_obj_add_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);
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
    if (s_title_lbl && title_hint) lv_label_set_text(s_title_lbl, title_hint);
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

    // "Save offline" is only meaningful with a card in the slot.
    if (s_save_btn) {
        if (sd_archive_is_mounted()) lv_obj_clear_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);
        else                         lv_obj_add_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);
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

    // Back button (primary exit — the Reader is launched from the Settings
    // drawer, not the swipe stack). Kept clear of the ~30 px left-edge
    // exit-swipe strip so that strip doesn't steal its taps.
    lv_obj_t *back_btn = lv_button_create(hdr);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 44, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_pad_hor(back_btn, 14, 0);
    lv_obj_set_style_pad_ver(back_btn, 8, 0);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  Back");

    // "Contents" button (opens the TOC panel).
    s_toc_btn = lv_button_create(hdr);
    lv_obj_align(s_toc_btn, LV_ALIGN_LEFT_MID, 190, 0);
    lv_obj_set_style_bg_color(s_toc_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_pad_hor(s_toc_btn, 14, 0);
    lv_obj_set_style_pad_ver(s_toc_btn, 8, 0);
    lv_obj_add_event_cb(s_toc_btn, contents_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *toc_btn_lbl = lv_label_create(s_toc_btn);
    lv_obj_set_style_text_font(toc_btn_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(toc_btn_lbl, LV_SYMBOL_LIST "  Contents");

    // "Save offline" button — mirrors the whole manual to SD. Shown only while a
    // card is mounted (toggled in tick_cb).
    s_save_btn = lv_button_create(hdr);
    lv_obj_align(s_save_btn, LV_ALIGN_LEFT_MID, 340, 0);
    lv_obj_set_style_bg_color(s_save_btn, lv_color_hex(UI_COLOR_SUCCESS), 0);
    lv_obj_set_style_pad_hor(s_save_btn, 14, 0);
    lv_obj_set_style_pad_ver(s_save_btn, 8, 0);
    lv_obj_add_event_cb(s_save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(s_save_btn);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(save_lbl, LV_SYMBOL_SD_CARD "  Save offline");
    lv_obj_add_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);

    s_title_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_LEFT_MID, 560, 0);
    lv_label_set_text(s_title_lbl, "Documentation");

    s_status_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_label_set_text(s_status_lbl, "");

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
    lv_obj_set_flex_flow(s_toc_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_toc_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_toc_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_toc_panel, LV_OBJ_FLAG_HIDDEN);

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

void reader_view_set_update_available(const char *latest_version)
{
    lock();
    strncpy(s_update_ver, latest_version ? latest_version : "", sizeof(s_update_ver) - 1);
    s_update_ver[sizeof(s_update_ver) - 1] = '\0';
    unlock();
}
