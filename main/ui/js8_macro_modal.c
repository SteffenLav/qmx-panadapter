// The JS8 macro sheet - twelve directed commands on the selected station.
//
// Structurally the plain half of js8_text_modal.c: same scrim, same panel
// geometry, same button styling. No textarea and no keyboard, because nothing
// here is typed.
//
// ⛔ EVERY COLOUR AND EVERY FONT COMES FROM ui_theme.h, and the body font is
// montserrat_24 for the same reason it is there: 14 px body text on a 5"
// panel at arm's length is not readable to most of the people using this.
//
// ⭐ THE TWO ROWS ARE SPLIT BY BEHAVIOUR, not by taste. The top row is the
// queries a JS8Call station answers BY ITSELF - the operator gets a reply
// without the other end touching the radio. The bottom row is QSO courtesy,
// which a human reads and answers. Mixing them would hide the one property
// that decides whether a press is worth a slot of airtime.
//
// ⛔ WHAT IS NOT HERE, and why:
//   - HB and CQ are @ALLCALL. They are not addressed to the selected station,
//     so they belong on the global bar, not on a sheet titled with somebody's
//     callsign.
//   - MSG, MSG TO:, QUERY and QUERY CALL need an argument a Directed frame
//     has no field for. The free-text box already carries 160 characters and
//     is already addressed to the selection - that is where the mailbox
//     family goes. js8_cmd_from_text() refuses them outright.
//   - SNR and GRID keep their own buttons on the TX bar. SNR carries a NUMBER
//     and GRID goes out as free text; neither is a bare directive, and both
//     were proven on air before this sheet existed.
#include "js8_macro_modal.h"
#include "ft8_screen_view.h"
#include "js8_target.h"
#include "js8_submode.h"
#include "storage/settings.h"
#include "ui_theme.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "js8_macro";

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_title = NULL;
static bool      s_open  = false;

/* The sheet, as one table. The word on the key is the word that goes on the
 * air, and it is the SAME string that reaches js8_cmd_from_text() - so there
 * is no mapping here to get wrong and no second copy of the command list.
 *
 * Order within a row is by how often it is pressed, not alphabetical. */
typedef struct { const char *cmd; int row; } macro_t;
static const macro_t MACROS[] = {
    /* Row 0 - the far end answers these without an operator. */
    { "SNR?",     0 },
    { "GRID?",    0 },
    { "INFO?",    0 },
    { "STATUS?",  0 },
    { "HEARING?", 0 },
    { "HW CPY?",  0 },
    /* Row 1 - QSO courtesy; a human reads these. */
    { "RR73",     1 },
    { "QSL",      1 },
    { "73",       1 },
    { "AGN?",     1 },
    { "YES",      1 },
    { "NO",       1 },
};
#define N_MACROS ((int)(sizeof(MACROS) / sizeof(MACROS[0])))
#define MACRO_COLS 6

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

/* ⛔ THE SHEET CLOSES ON A SEND, and that is not politeness. A press arms one
 * frame for the next slot; a second press would REPLACE it, because
 * ft8_tx_arm() is "most recent confirm wins". A sheet left open invites the
 * operator to press three commands and transmit only the last one. Closing
 * makes one press mean one transmission. */
static void macro_cb(lv_event_t *e)
{
    const char *cmd = (const char *)lv_event_get_user_data(e);
    if (!cmd) return;

    char msg[96];
    bool ok = ft8_screen_view_js8_directive(cmd, msg, sizeof(msg));
    ui_toast(msg);
    ESP_LOGI(TAG, "macro '%s': %s", cmd, ok ? "armed" : msg);
    modal_close();
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

    /* y=70 clears the top bar (band/mode/freq/S-meter), the same offset the
     * free-text composer uses. 900x400 ends at 470, clear of the bottom of a
     * 720 px screen - there is no keyboard to leave room for here. */
    s_panel = lv_obj_create(s_modal);
    lv_obj_set_size(s_panel, 900, 400);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 20, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* The callsign and the airtime. show() rewrites it; built empty so the
     * geometry below is laid out once. */
    s_title = lv_label_create(s_panel);
    lv_label_set_text(s_title, "");
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 0, 0);

    static const struct { const char *text; int y; } HEADS[2] = {
        { "They answer these automatically", 48  },
        { "QSO courtesy",                    186 },
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *h = lv_label_create(s_panel);
        lv_label_set_text(h, HEADS[i].text);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(h, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, HEADS[i].y);
    }

    /* Six across the 860 px inner width: 136 wide, 8 apart = 856. */
    const int BW = 136, BH = 64, GAP = 8;
    const int ROW_Y[2] = { 86, 224 };
    for (int i = 0; i < N_MACROS; i++) {
        int col = i % MACRO_COLS;
        lv_obj_t *b = lv_button_create(s_panel);
        lv_obj_set_size(b, BW, BH);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, col * (BW + GAP), ROW_Y[MACROS[i].row]);
        /* The queries come back by themselves, so they get the primary
         * colour; courtesy is the raised surface. Neither is red: nothing on
         * this sheet is destructive, and the only red on a dialog that keys
         * the radio should be the way out of it. */
        lv_obj_set_style_bg_color(b,
            lv_color_hex(MACROS[i].row == 0 ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE_RAISED), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        /* The string in the table IS the user data - a literal with static
         * storage, so there is nothing to keep alive alongside it. */
        lv_obj_add_event_cb(b, macro_cb, LV_EVENT_CLICKED, (void *)MACROS[i].cmd);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, MACROS[i].cmd);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_center(l);
    }

    lv_obj_t *close = lv_button_create(s_panel);
    lv_obj_set_size(close, 180, 64);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(UI_COLOR_DANGER), 0);
    lv_obj_set_style_radius(close, 8, 0);
    lv_obj_set_style_border_width(close, 0, 0);
    lv_obj_set_style_pad_all(close, 0, 0);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(cl);

    /* Esc closes. ⛔ NO ENTER BUTTON: there is no default command here, and a
     * sheet where the Enter key can key the radio by itself is not one. */
    ui_kbd_set_buttons(NULL, close);
}

void js8_macro_modal_init(void)
{
    modal_build();
}

void js8_macro_modal_show(void)
{
    modal_build();
    if (s_open) return;

    char call[32];
    js8_target_get(call, sizeof(call));
    if (!call[0]) {
        /* Unreachable from the row re-tap, which is the only caller today.
         * A sheet with no addressee would be twelve buttons aimed at nobody. */
        ESP_LOGW(TAG, "macro sheet asked for with no station selected");
        return;
    }

    /* ⭐ THE AIRTIME GOES IN THE HEADER, because it is the cost of the press
     * and the operator should read it BEFORE the radio is keyed - the same
     * reason the free-text composer prints its frame count. Every command on
     * this sheet is exactly one frame; what varies is the submode's slot
     * length, 6 s on Turbo and 30 s on Slow. The number comes from
     * js8_submode_secs(), the same table row the picker's label comes from. */
    char head[64];
    snprintf(head, sizeof(head), "%s  -  1 frame, %d s",
             call, js8_submode_secs(settings_get_js8_submode()));
    lv_label_set_text(s_title, head);

    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
    ESP_LOGI(TAG, "macro sheet open for %s", call);
}
