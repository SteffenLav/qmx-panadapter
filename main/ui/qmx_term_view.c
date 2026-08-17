#include "qmx_term_view.h"
#include "qmx_term.h"
#include "ui.h"                 // ui_help_overlay_changed(), ui_toast()
#include "ui_theme.h"

#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "util/psram_task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "term_view";

LV_FONT_DECLARE(qmx_mono_25);

/* The grid. CELL_W must match the font's advance width EXACTLY or every
 * reverse-video highlight drifts along its row - see the note at the top of
 * font_qmx_mono_25.c. adv_w there is 240 sixteenths = 15 px. */
#define CELL_W      15
#define ROW_H       27
#define GRID_W      (ANSI_COLS * CELL_W)   /* 1200 */
#define GRID_H      (ANSI_ROWS * ROW_H)    /* 648  */
#define HEADER_H    62

/* Header key row: NINE identical buttons (4 arrows, Enter, Back, BS, keyboard
 * toggle, Close), because the operator could not reliably hit the small ones when
 * they were five different widths. One size, derived positions.
 *
 * KEY_X0 clears the title ("Radio menus" at font 28) and the status label beside
 * it. The static assert below is the point of expressing it this way: it fails the
 * BUILD if a tenth key, a wider key or a bigger gap would push the row off the
 * right-hand edge, instead of that being discovered on the screen - which is how
 * DEL once ended up underneath Close. */
#define KEY_COUNT   9
#define KEY_W       92
#define KEY_H       48
#define KEY_GAP     6
#define KEY_X0      380
#define KEY_ROW_END (KEY_X0 + KEY_COUNT * KEY_W + (KEY_COUNT - 1) * KEY_GAP)
_Static_assert(KEY_ROW_END <= 1280 - 8,
               "qmx_term_view header key row would run off the right edge - "
               "reduce KEY_W/KEY_GAP or drop a key");

#define POLL_MS     350

static lv_obj_t  *s_overlay;
static lv_obj_t  *s_grid;
static lv_obj_t  *s_rows[ANSI_ROWS];
static lv_obj_t  *s_state;
static lv_obj_t  *s_help;           // shown when the radio offers no second port
static lv_obj_t  *s_cursor;         // the radio's cursor - see repaint()
static lv_timer_t *s_timer;
static bool       s_open;
static uint32_t   s_seen_seq;
static int        s_blank_ticks;
static bool       s_nudged;
static uint32_t   s_open_tick;      // when this session opened, for the nudge window

/* How long after opening a blank screen may still be a lost opening CR. Past
 * this, a blank screen is the radio doing as it was told - see poll_cb(). */
#define NUDGE_WINDOW_MS 8000

/* ⛔ THE UI THREAD MUST NEVER TOUCH THE PORT.
 *
 * qmx_term_open() blocks for up to ~2.4 s (it retries the opening CR three
 * times, waiting for the radio each time) and every keystroke is a blocking USB
 * write. Calling those from a button callback runs them on taskLVGL, which at
 * this display's ~13 fps is a freeze - serial-captured as
 * "ui_update_band: display_lock timeout", and the operator reported it as a
 * crash. The original code got away with a single 400 ms delay; the CR retry
 * made a latent mistake fatal, which is the honest history of this queue.
 *
 * So: the UI posts a command and returns immediately. This worker owns all
 * port I/O, and the poll timer picks the result up from the screen model. */
typedef enum { CMD_OPEN, CMD_CLOSE, CMD_KEY } term_cmd_kind_t;
typedef struct { term_cmd_kind_t kind; char key[12]; } term_cmd_t;

static QueueHandle_t s_cmdq;
static TaskHandle_t  s_worker;

/* Written by the worker, read by the LVGL poll. A plain enum is enough - it is
 * a single aligned word and the poll only ever displays it. */
typedef enum { ST_IDLE, ST_CONNECTING, ST_CONNECTED, ST_NO_PORT, ST_CLOSED } term_status_t;
static volatile term_status_t s_status = ST_IDLE;

static void worker_task(void *arg)
{
    (void)arg;
    term_cmd_t c;
    for (;;) {
        if (xQueueReceive(s_cmdq, &c, portMAX_DELAY) != pdTRUE) continue;
        switch (c.kind) {
        case CMD_OPEN:
            s_status = ST_CONNECTING;
            s_status = qmx_term_open() ? ST_CONNECTED : ST_NO_PORT;
            break;
        case CMD_CLOSE:
            qmx_term_close();
            s_status = ST_CLOSED;
            break;
        case CMD_KEY:
            qmx_term_key(c.key);
            break;
        }
    }
}

static void post(term_cmd_kind_t kind, const char *key)
{
    if (!s_cmdq) {
        s_cmdq = xQueueCreate(8, sizeof(term_cmd_t));
        if (!s_cmdq) return;
    }
    if (!s_worker) {
        s_worker = psram_task_create(worker_task, "qmx_term_ui", 4096, NULL, 3,
                                     tskNO_AFFINITY);
    }
    term_cmd_t c = { .kind = kind };
    if (key) { strncpy(c.key, key, sizeof(c.key) - 1); c.key[sizeof(c.key) - 1] = '\0'; }
    xQueueSend(s_cmdq, &c, 0);      /* never block the UI, even for a tick */
}

/* Reverse-video blocks, rebuilt on every change. There are one or two on a
 * normal menu, so a small fixed pool is plenty and avoids churning LVGL's
 * allocator at 3 Hz. Anything beyond the pool is simply not highlighted, which
 * degrades to "no highlight" rather than to a wrong one. */
#define MAX_REV 24
static lv_obj_t *s_rev[MAX_REV];
static int       s_rev_used;

bool qmx_term_view_is_open(void) { return s_open; }

static void set_state(const char *txt, uint32_t colour)
{
    if (!s_state) return;
    lv_label_set_text(s_state, txt);
    lv_obj_set_style_text_color(s_state, lv_color_hex(colour), 0);
}

/* Paint the whole grid from the model. Caller must NOT hold the screen lock -
 * this takes it itself and gives it straight back. */
static void repaint(void)
{
    const ansi_term_t *t = qmx_term_lock_screen();
    if (!t) return;

    if (t->dirty_seq == s_seen_seq) { qmx_term_unlock_screen(); return; }
    s_seen_seq = t->dirty_seq;

    char line[ANSI_COLS + 1];
    int rev_n = 0;
    struct { int r, c, len; } runs[MAX_REV];

    for (int r = 0; r < ANSI_ROWS; r++) {
        ansi_term_row_text(t, r, line);
        int e = ANSI_COLS;
        while (e > 0 && line[e - 1] == ' ') e--;
        line[e] = '\0';
        if (s_rows[r]) lv_label_set_text(s_rows[r], line);

        for (int c = 0; c < ANSI_COLS; ) {
            if (t->cell[r][c].reverse) {
                int s = c;
                while (c < ANSI_COLS && t->cell[r][c].reverse) c++;
                if (rev_n < MAX_REV) { runs[rev_n].r = r; runs[rev_n].c = s;
                                       runs[rev_n].len = c - s; rev_n++; }
            } else c++;
        }
    }
    /* Where the radio's cursor is, and whether it wants it shown. Read under the
     * same lock as the cells so it cannot disagree with what we just drew. */
    int cur_r = t->cur_r, cur_c = t->cur_c;
    bool cur_vis = t->cursor_visible;
    qmx_term_unlock_screen();

    /* ⭐ DRAW THE CURSOR. The model has tracked cur_r/cur_c/cursor_visible since
     * the first version and the renderer ignored all three, so in any field you
     * type into - Messages especially - there was no way to see where you were.
     * Randy N4OPI: "Cursor location is not displayed." Exact placement, because
     * the font is fixed-pitch and CELL_W is integral. */
    if (cur_vis && cur_r >= 0 && cur_r < ANSI_ROWS && cur_c >= 0 && cur_c < ANSI_COLS) {
        if (!s_cursor) {
            s_cursor = lv_obj_create(s_grid);
            lv_obj_remove_style_all(s_cursor);
            lv_obj_set_style_bg_opa(s_cursor, LV_OPA_60, 0);
            lv_obj_set_style_bg_color(s_cursor, lv_color_hex(UI_COLOR_PRIMARY), 0);
            lv_obj_set_size(s_cursor, CELL_W, ROW_H);
            lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_obj_set_pos(s_cursor, cur_c * CELL_W, cur_r * ROW_H);
        lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_cursor);   // above the reverse-video blocks
    } else if (s_cursor) {
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    }

    /* Highlights are drawn as filled blocks BEHIND the text, with the text
     * recoloured to dark for those rows... which cannot be done per-run on a
     * plain label. So instead each run gets its own small label on TOP of its
     * block, carrying just that run's characters. Exact because the font is
     * fixed-pitch and CELL_W is integral. */
    for (int i = 0; i < rev_n; i++) {
        if (!s_rev[i]) {
            s_rev[i] = lv_obj_create(s_grid);
            lv_obj_remove_style_all(s_rev[i]);
            lv_obj_set_style_bg_opa(s_rev[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_rev[i], lv_color_hex(0xD8D8D8), 0);
            lv_obj_clear_flag(s_rev[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(s_rev[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *lbl = lv_label_create(s_rev[i]);
            lv_obj_set_style_text_font(lbl, &qmx_mono_25, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
            lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
        }
        lv_obj_set_pos(s_rev[i], runs[i].c * CELL_W, runs[i].r * ROW_H);
        lv_obj_set_size(s_rev[i], runs[i].len * CELL_W, ROW_H);
        lv_obj_clear_flag(s_rev[i], LV_OBJ_FLAG_HIDDEN);

        /* Re-read the run's text from the row label we just set. */
        const char *row_txt = s_rows[runs[i].r] ? lv_label_get_text(s_rows[runs[i].r]) : "";
        char frag[ANSI_COLS + 1];
        int rl = (int)strlen(row_txt);
        int n = 0;
        for (int k = runs[i].c; k < runs[i].c + runs[i].len && k < ANSI_COLS; k++)
            frag[n++] = (k < rl) ? row_txt[k] : ' ';
        frag[n] = '\0';
        lv_label_set_text(lv_obj_get_child(s_rev[i], 0), frag);
    }
    for (int i = rev_n; i < s_rev_used; i++)
        if (s_rev[i]) lv_obj_add_flag(s_rev[i], LV_OBJ_FLAG_HIDDEN);
    s_rev_used = rev_n;
}

/* Is there anything at all on the screen? */
static bool grid_is_blank(void)
{
    const ansi_term_t *t = qmx_term_lock_screen();
    if (!t) return false;
    bool blank = true;
    for (int r = 0; r < ANSI_ROWS && blank; r++)
        for (int c = 0; c < ANSI_COLS; c++)
            if (t->cell[r][c].ch != ' ') { blank = false; break; }
    qmx_term_unlock_screen();
    return blank;
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_open) return;

    /* Reflect whatever the worker has got to. This is the only place the state
     * label is driven while a session is up, so it cannot disagree with what
     * the port is actually doing. */
    switch (s_status) {
    case ST_CONNECTING:
        set_state("connecting...", 0x888888);
        return;                          /* nothing to draw yet */
    case ST_NO_PORT:
        // Short: the buttons start at x=380 and the panel below carries the detail.
        set_state("no second port", 0xFF6050);
        if (s_help) lv_obj_clear_flag(s_help, LV_OBJ_FLAG_HIDDEN);
        return;
    default:
        break;
    }

    if (!qmx_term_is_open()) {
        /* The device's own idle watchdog closed it. Say which, or the screen
         * simply freezing looks like a fault. */
        set_state("session timed out - closed", 0xFFA040);
        return;
    }
    set_state("connected", 0x60C060);
    repaint();

    /* Last-ditch recovery for a blank screen at OPEN time. qmx_term_open()
     * already retries the opening CR three times, so reaching here means all
     * three went unanswered - but a blank page is what the OPERATOR sees, and
     * they should not have to know that pressing Enter is the cure.
     *
     * ⛔ IT IS BOUNDED TO THE OPENING WINDOW, and that bound is the whole fix
     * for a bug I shipped. The first version fired whenever the screen was
     * blank, at any point in the session. Michael KZ4LY then chose "Exit
     * terminal" from inside the radio's own menu - which clears the screen - and
     * the nudge helpfully sent a CR a second and a half later and put him
     * straight back into terminal mode: "I guessed that explicitly selecting
     * 'Exit terminal' would do this, but it fired back up."
     *
     * A lost opening CR can only be lost at open. Anything blank later is the
     * radio doing what the operator asked, so leave it alone. */
    if (!s_nudged && s_open_tick && grid_is_blank()) {
        if (lv_tick_elaps(s_open_tick) > NUDGE_WINDOW_MS) {
            s_nudged = true;                 /* window closed - never nudge again */
        } else if (++s_blank_ticks >= 4) {    /* ~1.4 s of nothing */
            s_nudged = true;
            ESP_LOGW(TAG, "screen still blank %lu ms after open - sending one CR",
                     (unsigned long)lv_tick_elaps(s_open_tick));
            post(CMD_KEY, "enter");
            s_seen_seq = 0;
        }
    } else {
        s_blank_ticks = 0;
    }
}

/* ---- On-screen QWERTY (#164, Randy N4OPI + Michael KZ4LY) -----------------
 *
 * The Tab5 has no keyboard unless the snap-on one is fitted, so a menu field that
 * wants a value typed into it was unreachable from the device - the browser had a
 * real keyboard and the Tab5 had arrows. Randy asked for this and it was
 * deliberately held back: "it waits behind the cursor and delete work - a keyboard
 * is not much use typing into a field you cannot see." Both of those now work, so
 * this is unblocked rather than merely wanted.
 *
 * Note the deliberate difference from every other keyboard in this firmware: it is
 * attached to NO textarea. Each press is forwarded straight to the radio as a
 * keystroke, because the radio's own menu owns the field and its cursor - there is
 * no local copy of the text to edit, and inventing one would immediately disagree
 * with what the radio shows.
 */
static lv_obj_t *s_kb;          /* NULL until first shown */
static lv_obj_t *s_kb_btn;      /* the header toggle, so its look can follow */

static void kb_set_shown(bool shown);

static void kb_event_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_buttonmatrix_get_button_text(kb, id);
    if (!txt) return;

    /* Layout and shift keys belong to the shared caps-cycle handler, which runs
     * BEFORE this one and already invoked LVGL's built-in for them. Calling
     * lv_keyboard_def_event_cb() here as well would double-handle the press, so
     * these are simply skipped. "Abc" is the cycle's pending-shift label - the
     * v0.15.12 bug was LVGL typing that label as text, so it must be skipped by
     * name here too. */
    if (!strcmp(txt, "1#")  || !strcmp(txt, "ABC") ||
        !strcmp(txt, "abc") || !strcmp(txt, "Abc")) {
        return;
    }
    /* The keyboard glyph is conventionally "put it away", and that is genuinely
     * useful here because the keyboard covers the lower rows of the screen. */
    if (!strcmp(txt, LV_SYMBOL_KEYBOARD)) { kb_set_shown(false); return; }

    /* Control keys map onto the same names the header buttons use, so there is
     * one definition of what each key sends (qmx_term.c) and no second table to
     * drift. */
    const char *name = NULL;
    if      (!strcmp(txt, LV_SYMBOL_BACKSPACE)) name = "bksp";
    else if (!strcmp(txt, LV_SYMBOL_NEW_LINE))  name = "enter";
    else if (!strcmp(txt, LV_SYMBOL_OK))        name = "enter";
    else if (!strcmp(txt, LV_SYMBOL_LEFT))      name = "left";
    else if (!strcmp(txt, LV_SYMBOL_RIGHT))     name = "right";

    if (name) {
        post(CMD_KEY, name);
    } else if (txt[0] && txt[1] == '\0') {
        /* A single BYTE - covers the letters, digits, punctuation and space.
         * Anything longer is a multi-byte glyph from a layout we do not enable;
         * dropping it is right, since the radio's menu is ASCII. */
        post(CMD_KEY, txt);
    } else {
        return;
    }
    s_seen_seq = 0;                 /* repaint on the next tick */
}

static void kb_set_shown(bool shown)
{
    if (!s_kb) return;
    if (shown) lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    if (s_kb_btn) {
        /* Show the state on the button: this thing covers half the screen, so
         * whether it is up must be readable at a glance. */
        lv_obj_set_style_bg_color(s_kb_btn,
            lv_color_hex(shown ? UI_COLOR_PRIMARY : 0x252525), 0);
    }
}

static void kb_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (!s_kb) return;
    kb_set_shown(lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN));
}
/* DEV ONLY: show/hide the on-screen keyboard without a finger, so its appearance
 * can be screenshotted and checked against the other keyboards in the app. The
 * toggle itself is a touch target with no API, and "does it look like the
 * others" is exactly the kind of claim that should be verified rather than
 * asserted from having copied the style block. */
void qmx_term_view_set_keyboard(bool shown) { kb_set_shown(shown); }

static void key_cb(lv_event_t *e)
{
    const char *k = (const char *)lv_event_get_user_data(e);
    if (!k) return;
    post(CMD_KEY, k);               /* never blocking USB from taskLVGL */
    s_seen_seq = 0;                 /* force a repaint on the next tick */
}

static void close_cb(lv_event_t *e) { (void)e; qmx_term_view_close(); }

static lv_obj_t *make_key(lv_obj_t *parent, const char *label, const char *key,
                          int x, int w)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, KEY_H);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2a3138), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, (void *)key);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    return b;
}

static void build(void)
{
    if (s_overlay) return;

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);   // swallow taps to the panadapter

    lv_obj_t *hdr = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_HOR_RES, HEADER_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Radio menus");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);

    s_state = lv_label_create(hdr);
    lv_label_set_text(s_state, "");
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_20, 0);
    lv_obj_align(s_state, LV_ALIGN_LEFT_MID, 236, 0);   // clear of the title at _28

    /* ⭐ ONE SIZE FOR EVERY BUTTON ON THIS BAR, including Close.
     *
     * Operator, after using it: "ALL buttons on the top needs to have same size
     * so you can actually hit them." They had drifted to five different widths -
     * arrows 100, Enter 96, Back 88, BS 54, the keyboard toggle 54, Close 130 -
     * each one locally justified and the row collectively a lottery for a finger.
     *
     * So the geometry is now DERIVED, not hand-placed: nine slots of KEY_W with
     * KEY_GAP between them, laid out from KEY_X0. Adding or removing a key means
     * changing the count, not re-deriving every x - which is what produced the
     * drift, and what put DEL underneath Close on the first attempt.
     *
     * Close keeps its red colour: same size to hit, still unmistakable. */
    const int slot = KEY_W + KEY_GAP;
    make_key(hdr, LV_SYMBOL_UP,    "up",     KEY_X0 + 0 * slot, KEY_W);
    make_key(hdr, LV_SYMBOL_DOWN,  "down",   KEY_X0 + 1 * slot, KEY_W);
    make_key(hdr, LV_SYMBOL_LEFT,  "left",   KEY_X0 + 2 * slot, KEY_W);
    make_key(hdr, LV_SYMBOL_RIGHT, "right",  KEY_X0 + 3 * slot, KEY_W);
    make_key(hdr, "Enter",         "enter",  KEY_X0 + 4 * slot, KEY_W);
    make_key(hdr, "Back",          "ctrl-q", KEY_X0 + 5 * slot, KEY_W);
    /* ONE delete key. v1.8.4 shipped BS and DEL side by side rather than guessing
     * which byte the radio acts on; Randy N4OPI answered it from PuTTY (BS deletes
     * leftward in a numeric field, Del does nothing), so DEL is gone. */
    make_key(hdr, "BS",            "bksp",   KEY_X0 + 6 * slot, KEY_W);

    s_kb_btn = lv_btn_create(hdr);
    lv_obj_set_size(s_kb_btn, KEY_W, KEY_H);
    lv_obj_align(s_kb_btn, LV_ALIGN_LEFT_MID, KEY_X0 + 7 * slot, 0);
    lv_obj_set_style_bg_color(s_kb_btn, lv_color_hex(0x2a3138), 0);
    lv_obj_set_style_border_color(s_kb_btn, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_width(s_kb_btn, 2, 0);
    lv_obj_add_event_cb(s_kb_btn, kb_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *kbl = lv_label_create(s_kb_btn);
    lv_label_set_text(kbl, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_font(kbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(kbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(kbl);

    lv_obj_t *cb = lv_btn_create(hdr);
    lv_obj_set_size(cb, KEY_W, KEY_H);
    lv_obj_align(cb, LV_ALIGN_LEFT_MID, KEY_X0 + 8 * slot, 0);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x3a2222), 0);
    lv_obj_set_style_border_color(cb, lv_color_hex(0xB05050), 0);
    lv_obj_set_style_border_width(cb, 2, 0);
    lv_obj_add_event_cb(cb, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cl);

    s_grid = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, GRID_W, GRID_H);
    lv_obj_set_pos(s_grid, (LV_HOR_RES - GRID_W) / 2, HEADER_H + 4);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int r = 0; r < ANSI_ROWS; r++) {
        s_rows[r] = lv_label_create(s_grid);
        lv_obj_set_style_text_font(s_rows[r], &qmx_mono_25, 0);
        lv_obj_set_style_text_color(s_rows[r], lv_color_hex(0xD8D8D8), 0);
        lv_label_set_long_mode(s_rows[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_rows[r], GRID_W);
        lv_obj_set_pos(s_rows[r], 0, r * ROW_H);
        lv_label_set_text(s_rows[r], "");
    }

    /* Shown INSTEAD of the grid when the radio has no second serial port, and it
     * carries the menu path in full. Michael KZ4LY: "If it can't open the second
     * serial port, could it display this literal menu path in text on screen? I
     * knew I needed to do this but couldn't remember where in the menu structure
     * to do it, so I had to come back here to read the announcement."
     *
     * A toast was wrong for this: it disappears, and the one person who needs the
     * instruction is the one who never read the announcement. */
    /* The QWERTY itself, created LAST so it draws over the grid, and hidden until
     * asked for. It covers the lower rows - unavoidable at 1280x720 with a 648 px
     * grid already on screen - which is why it is a toggle and why its own
     * keyboard glyph puts it away. */
    /* Styled EXACTLY like every other keyboard in the app - operator: "Keyboard has
     * a completely wrong style and colour - needs to be equal to all other
     * keybords we use in this app." My first version took LVGL's default theme,
     * which is a light keyboard in a dark application.
     *
     * This is the same block ft8_cq_modal/identity_config/wifi use: the shared
     * per-key style on LV_PART_ITEMS, then ui_theme_style_keyboard() for the
     * surface and the checked-state override, then montserrat_28. Copied
     * deliberately rather than re-invented, since "equal to the others" is the
     * requirement and the shared helper is what defines equal. */
    s_kb = lv_keyboard_create(s_overlay);
    static lv_style_t style_kb_btn;
    static bool kb_btn_style_inited = false;
    if (!kb_btn_style_inited) {
        lv_style_init(&style_kb_btn);
        lv_style_set_bg_color(&style_kb_btn, lv_color_hex(UI_COLOR_KEY_BG));
        lv_style_set_bg_opa(&style_kb_btn, LV_OPA_COVER);
        lv_style_set_text_color(&style_kb_btn, lv_color_white());
        lv_style_set_border_width(&style_kb_btn, 1);
        lv_style_set_border_color(&style_kb_btn, lv_color_hex(0x505050));
        kb_btn_style_inited = true;
    }
    lv_obj_add_style(s_kb, &style_kb_btn, LV_PART_ITEMS);
    ui_theme_style_keyboard(s_kb);
    lv_obj_set_size(s_kb, LV_PCT(100), 280);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_28, 0);

    /* The shared iPad-style abc/Abc/ABC shift cycle, so shift behaves the way it
     * does in every other field in this firmware. It REMOVES LVGL's built-in
     * VALUE_CHANGED handler and becomes the primary one, invoking the built-in
     * itself for non-shift keys - so our forwarding handler is added AFTER it and
     * must not call lv_keyboard_def_event_cb() again. */
    ui_theme_keyboard_attach_caps_cycle(s_kb);

    /* NO textarea on purpose - see the comment on kb_event_cb. With none
     * attached, nothing can type into a local copy of the text; the radio's own
     * menu owns the field. */
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    kb_set_shown(false);            /* also paints the toggle's idle colour */

    s_help = lv_label_create(s_overlay);
    lv_label_set_text(s_help,
        "This needs the radio's SECOND USB serial port, which is off by default.\n\n"
        "On the QMX, set:\n\n"
        "    System config\n"
        "      -> GPS & Ser. ports\n"
        "        -> USB serial ports\n"
        "          -> 2\n\n"
        "You only have to do this once - it survives a power cycle. Then reopen\n"
        "this screen.\n\n"
        "The panadapter keeps working while you are in the radio's menus, because\n"
        "they run on different ports.");
    lv_obj_set_style_text_font(s_help, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_help, lv_color_hex(0xFFC060), 0);
    lv_obj_set_pos(s_help, (LV_HOR_RES - GRID_W) / 2, HEADER_H + 40);
    lv_obj_add_flag(s_help, LV_OBJ_FLAG_HIDDEN);
}

void qmx_term_view_open(void)
{
    build();
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    s_open = true;
    s_seen_seq = 0;
    s_blank_ticks = 0;
    s_nudged = false;
    s_open_tick = lv_tick_get();
    if (s_help) lv_obj_add_flag(s_help, LV_OBJ_FLAG_HIDDEN);
    kb_set_shown(false);            /* every session starts with it out of the way */
    for (int r = 0; r < ANSI_ROWS; r++) if (s_rows[r]) lv_label_set_text(s_rows[r], "");
    for (int i = 0; i < s_rev_used; i++) if (s_rev[i]) lv_obj_add_flag(s_rev[i], LV_OBJ_FLAG_HIDDEN);
    s_rev_used = 0;

    /* Stand the panadapter's touch navigation down. Without this the top-bar
     * Band/Mode/BW hit zones - direct children of the screen, foregrounded above
     * this overlay - swallow taps aimed at our own keys, exactly as they did to
     * the Reader's Back/Exit buttons in v1.5.0. */
    ui_help_overlay_changed();

    s_status = ST_CONNECTING;
    set_state("connecting...", 0x888888);
    post(CMD_OPEN, NULL);           /* the worker does the waiting, not us */

    if (!s_timer) s_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
    lv_timer_resume(s_timer);
    ESP_LOGI(TAG, "open");
}

void qmx_term_view_close(void)
{
    if (!s_open) return;
    s_open = false;
    if (s_timer) lv_timer_pause(s_timer);
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    ui_help_overlay_changed();     // hand the top bar and edge swipes back
    /* The exit walk is several seconds of USB round-trips - the screen is
     * already gone, so the operator sees an instant close while the worker
     * hands the radio back properly in the background. */
    post(CMD_CLOSE, NULL);
    ESP_LOGI(TAG, "close");
}
