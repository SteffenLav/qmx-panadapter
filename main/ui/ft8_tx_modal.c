// FT8 TX confirmation modal - the sole path to ft8_tx_arm(). Shown after
// the operator taps a heard-station row (reply) or "Call CQ"; the request
// arrives already built+validated+encoded via ft8_tx_build_request(), so
// this modal only needs to present it and, on confirm, arm it.
//
// Structurally a copy of identity_config.c's full-screen-overlay scaffold
// (black backdrop + dark panel + red Cancel / green confirm), with a
// message preview, a live 1 Hz countdown, and an inline error line for
// arm-time failures (e.g. QMX won't confirm Digi mode).

#include "ft8_tx_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "ft8_tx.h"
#include "ft8_qso.h"
#include "ft8_screen_view.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "ft8_tx_modal";

// Modal state - lazily created on first show (mirrors identity_config.c).
static lv_obj_t   *s_modal         = NULL;
static lv_obj_t   *s_panel         = NULL;
static lv_obj_t   *s_lbl_title     = NULL;
static lv_obj_t   *s_lbl_message   = NULL;
static lv_obj_t   *s_lbl_detail    = NULL;
static lv_obj_t   *s_lbl_countdown = NULL;
static lv_obj_t   *s_lbl_error     = NULL;
static lv_obj_t   *s_btn_pounce    = NULL;  // "Auto Pounce" (fresh-grid REPLY only)
static lv_obj_t   *s_pounce_swatch = NULL;  // legend swatch, hidden with the button
static lv_obj_t   *s_pounce_legend = NULL;  // legend text, hidden with the button
static lv_obj_t   *s_btn_nudge_up  = NULL;  // re-target the row above (REPLY only)
static lv_obj_t   *s_btn_nudge_dn  = NULL;  // re-target the row below (REPLY only)
static lv_timer_t *s_timer         = NULL;
static bool        s_modal_open    = false;

// Private copy of the request being confirmed - taken at show() time so the
// caller's stack copy (e.g. a local in a row-tap handler) can go out of
// scope immediately. Only ever read from the LVGL task (button/timer
// callbacks), so no locking needed.
static ft8_tx_request_t s_pending_req;

// Seconds from now until the next slot this request could fire in, *if*
// armed right this instant - a UI preview, since the request isn't armed yet
// (ft8_tx_get_status() only knows about already-armed requests) and the
// *actual* fire time will be fixed at the moment the operator taps Transmit.
// Calls ft8_tx_seconds_until_slot() directly instead of keeping a local copy
// of its math - a prior local duplicate here drifted out of sync with the
// real (FT4-aware, millisecond-precision) implementation in ft8_tx.c and
// kept showing an FT8-cadence countdown for FT4 requests.
static void update_countdown(void)
{
    int secs = ft8_tx_seconds_until_slot(s_pending_req.use_parity,
                                         s_pending_req.want_even_slot,
                                         s_pending_req.protocol);
    char b[32];
    snprintf(b, sizeof(b), "Transmits in ~%d s", secs);
    lv_label_set_text(s_lbl_countdown, b);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_modal_open) return;
    update_countdown();
}

static void modal_close(void)
{
    if (!s_modal || !s_modal_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_modal_open = false;
    ESP_LOGI(TAG, "modal closed");
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "cancelled - nothing armed, radio untouched ('%s')", s_pending_req.display_text);
    modal_close();
}

static void transmit_btn_cb(lv_event_t *e)
{
    (void)e;
    char err[64] = {0};
    char busy_target[24];
    // A one-off Transmit on a row would otherwise silently overwrite an
    // already-armed exchange message (ft8_tx_arm only refuses a burst already
    // ACTIVE on air, not one merely ARMED-and-waiting) - refuse the same way
    // ft8_qso_start() does for Auto Pounce.
    if (ft8_qso_is_busy(busy_target, sizeof(busy_target))) {
        if (busy_target[0]) snprintf(err, sizeof(err), "Busy: working %s", busy_target);
        else                 snprintf(err, sizeof(err), "Busy: calling CQ");
        ESP_LOGW(TAG, "transmit refused: %s", err);
        lv_label_set_text(s_lbl_error, err);
        lv_obj_clear_flag(s_lbl_error, LV_OBJ_FLAG_HIDDEN);
        ui_toast(err);
        return;
    }
    if (ft8_tx_arm(&s_pending_req, err, sizeof(err))) {
        ESP_LOGI(TAG, "armed via modal: '%s'", s_pending_req.display_text);
        // Track who we're manually working: keeps the partner out of the
        // pileup capture mid-exchange and drives the amber "working" row
        // highlight for hand-run QSOs (same look as an auto QSO).
        if (s_pending_req.target_call[0])
            ft8_qso_note_manual_target(s_pending_req.target_call);
        // A manually-armed closing message (RR73/73) wraps up a hand-run QSO:
        // seed the QSO machine's WAIT_DONE so the burst's completion logs the
        // QSO to ADIF and drops the partner from the pileup - same wrap-up an
        // auto QSO gets. No-op if a machine QSO is already active.
        if (s_pending_req.kind == FT8_TX_KIND_73)
            ft8_qso_notify_manual_final(s_pending_req.target_call);
        modal_close();
    } else {
        ESP_LOGW(TAG, "arm refused: %s", err);
        lv_label_set_text(s_lbl_error, err[0] ? err : "Could not arm transmission");
        lv_obj_clear_flag(s_lbl_error, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pounce_btn_cb(lv_event_t *e)
{
    (void)e;
    char err[64] = {0};
    // Pass the pre-built, pre-encoded TX1 request directly — no re-encoding,
    // parity already correctly set by ft8_tx_build_request at row_activate time.
    if (ft8_qso_start(&s_pending_req, err, sizeof(err))) {
        ESP_LOGI(TAG, "auto pounce started: '%s'", s_pending_req.display_text);
        modal_close();
    } else {
        ESP_LOGW(TAG, "pounce refused: %s", err);
        lv_label_set_text(s_lbl_error, err[0] ? err : "Could not start auto pounce");
        lv_obj_clear_flag(s_lbl_error, LV_OBJ_FLAG_HIDDEN);
        if (strstr(err, "Busy")) ui_toast(err);
    }
}

// Re-target the confirm dialog to the row above/below the one currently
// shown, without closing/reopening the modal. ft8_screen_view re-resolves
// the row and calls back into ft8_tx_modal_show() with the fresh request.
static void nudge_up_btn_cb(lv_event_t *e)
{
    (void)e;
    ft8_screen_view_nudge_confirm(-1);
}

static void nudge_dn_btn_cb(lv_event_t *e)
{
    (void)e;
    ft8_screen_view_nudge_confirm(+1);
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
    lv_obj_set_size(s_panel, 880, 460);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 24, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_title = lv_label_create(s_panel);
    lv_label_set_text(s_lbl_title, "Confirm FT8 Transmission");
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_32, 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 0);

    // The encoded message preview - the most important line in the dialog
    // (this is *exactly* what will go on-air; ft8_tx_build_request already
    // ran it through ftx_message_encode_std + ft8_encode).
    s_lbl_message = lv_label_create(s_panel);
    lv_label_set_text(s_lbl_message, "");
    lv_obj_set_style_text_color(s_lbl_message, lv_color_hex(0xFFA040), 0);
    lv_obj_set_style_text_font(s_lbl_message, &lv_font_montserrat_32, 0);
    lv_obj_align(s_lbl_message, LV_ALIGN_TOP_MID, 0, 64);

    s_lbl_detail = lv_label_create(s_panel);
    lv_label_set_text(s_lbl_detail, "");
    lv_obj_set_style_text_color(s_lbl_detail, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_lbl_detail, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_detail, LV_ALIGN_TOP_MID, 0, 130);

    s_lbl_countdown = lv_label_create(s_panel);
    lv_label_set_text(s_lbl_countdown, "");
    lv_obj_set_style_text_color(s_lbl_countdown, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_countdown, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_countdown, LV_ALIGN_TOP_MID, 0, 174);

    // Row nudge — moves the confirm target up/down one row in the decode
    // list, for when the hold-and-drag gesture caught the wrong station.
    // Pinned to the top and bottom of the panel's right column (not stacked
    // tight together) so they're easy to tell apart and hit at speed - the
    // panel has plenty of unused height in that column since the message/
    // detail/countdown text is centered, not full-width.
    s_btn_nudge_up = lv_btn_create(s_panel);
    lv_obj_set_size(s_btn_nudge_up, 100, 100);
    lv_obj_align(s_btn_nudge_up, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_btn_nudge_up, lv_color_hex(0x2a2f37), 0);
    lv_obj_set_style_border_color(s_btn_nudge_up, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_btn_nudge_up, 2, 0);
    lv_obj_set_style_radius(s_btn_nudge_up, 8, 0);
    lv_obj_add_event_cb(s_btn_nudge_up, nudge_up_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nudge_up_lbl = lv_label_create(s_btn_nudge_up);
    lv_label_set_text(nudge_up_lbl, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(nudge_up_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(nudge_up_lbl, &lv_font_montserrat_32, 0);
    lv_obj_center(nudge_up_lbl);

    // Bottom-anchored (not top-anchored + fixed offset) so it stays clear of
    // the Cancel/Auto Pounce/Transmit row (72 px tall) regardless of panel
    // height - 66 px clearance above that row.
    s_btn_nudge_dn = lv_btn_create(s_panel);
    lv_obj_set_size(s_btn_nudge_dn, 100, 100);
    lv_obj_align(s_btn_nudge_dn, LV_ALIGN_BOTTOM_RIGHT, 0, -138);
    lv_obj_set_style_bg_color(s_btn_nudge_dn, lv_color_hex(0x2a2f37), 0);
    lv_obj_set_style_border_color(s_btn_nudge_dn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(s_btn_nudge_dn, 2, 0);
    lv_obj_set_style_radius(s_btn_nudge_dn, 8, 0);
    lv_obj_add_event_cb(s_btn_nudge_dn, nudge_dn_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nudge_dn_lbl = lv_label_create(s_btn_nudge_dn);
    lv_label_set_text(nudge_dn_lbl, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(nudge_dn_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(nudge_dn_lbl, &lv_font_montserrat_32, 0);
    lv_obj_center(nudge_dn_lbl);

    s_lbl_error = lv_label_create(s_panel);
    lv_label_set_text(s_lbl_error, "");
    lv_obj_set_style_text_color(s_lbl_error, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_font(s_lbl_error, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(s_lbl_error, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_error, 800);
    lv_obj_align(s_lbl_error, LV_ALIGN_TOP_MID, 0, 224);
    lv_obj_add_flag(s_lbl_error, LV_OBJ_FLAG_HIDDEN);

    // Legend explaining what the two send buttons actually do - sits in the
    // free space between the error line and the bottom button row. Color
    // swatches match each button's own color so the association is visual,
    // not just textual.
    lv_obj_t *swatch_tx = lv_obj_create(s_panel);
    lv_obj_set_size(swatch_tx, 24, 24);
    lv_obj_set_style_radius(swatch_tx, 12, 0);
    lv_obj_set_style_bg_color(swatch_tx, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_border_width(swatch_tx, 0, 0);
    lv_obj_clear_flag(swatch_tx, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(swatch_tx, LV_ALIGN_TOP_LEFT, 0, 234);

    lv_obj_t *lbl_tx_legend = lv_label_create(s_panel);
    lv_label_set_text(lbl_tx_legend, "Sends this message once, right now.");
    lv_obj_set_style_text_color(lbl_tx_legend, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_tx_legend, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_tx_legend, LV_ALIGN_TOP_LEFT, 34, 230);

    s_pounce_swatch = lv_obj_create(s_panel);
    lv_obj_t *swatch_pounce = s_pounce_swatch;
    lv_obj_set_size(swatch_pounce, 24, 24);
    lv_obj_set_style_radius(swatch_pounce, 12, 0);
    lv_obj_set_style_bg_color(swatch_pounce, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_width(swatch_pounce, 0, 0);
    lv_obj_clear_flag(swatch_pounce, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(swatch_pounce, LV_ALIGN_TOP_LEFT, 0, 278);

    // Kept short enough to stay on one line at this font/width - the prior
    // longer phrasing could wrap to 2 lines and run into the button row
    // below, leaving less than the required 30px clearance.
    s_pounce_legend = lv_label_create(s_panel);
    lv_obj_t *lbl_pounce_legend = s_pounce_legend;
    lv_label_set_text(lbl_pounce_legend, "Sends it, then auto-replies until the QSO is done.");
    lv_obj_set_style_text_color(lbl_pounce_legend, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_pounce_legend, &lv_font_montserrat_28, 0);
    lv_label_set_long_mode(lbl_pounce_legend, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_pounce_legend, 760);
    lv_obj_align(lbl_pounce_legend, LV_ALIGN_TOP_LEFT, 34, 274);

    // Bottom row: [Cancel]  [Auto Pounce]  [Transmit]
    // Cancel — always visible
    lv_obj_t *cancel_btn = lv_btn_create(s_panel);
    lv_obj_set_size(cancel_btn, 180, 72);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 20, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0xc04040), 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cancel_lbl);

    // Auto Pounce — visible only for REPLY kind (hidden initially, shown in show())
    s_btn_pounce = lv_btn_create(s_panel);
    lv_obj_set_size(s_btn_pounce, 220, 72);
    lv_obj_align(s_btn_pounce, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_btn_pounce, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_color(s_btn_pounce, lv_color_hex(UI_COLOR_PRIMARY_BORDER), 0);
    lv_obj_set_style_border_width(s_btn_pounce, 2, 0);
    lv_obj_set_style_radius(s_btn_pounce, 8, 0);
    lv_obj_add_event_cb(s_btn_pounce, pounce_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pounce_lbl = lv_label_create(s_btn_pounce);
    lv_label_set_text(pounce_lbl, "Auto Pounce");
    lv_obj_set_style_text_color(pounce_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(pounce_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(pounce_lbl);

    // Transmit (manual single TX)
    lv_obj_t *tx_btn = lv_btn_create(s_panel);
    lv_obj_set_size(tx_btn, 180, 72);
    lv_obj_align(tx_btn, LV_ALIGN_BOTTOM_RIGHT, -20, 0);
    lv_obj_set_style_bg_color(tx_btn, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_border_color(tx_btn, lv_color_hex(0x4caf50), 0);
    lv_obj_set_style_border_width(tx_btn, 2, 0);
    lv_obj_set_style_radius(tx_btn, 8, 0);
    lv_obj_add_event_cb(tx_btn, transmit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tx_lbl = lv_label_create(tx_btn);
    lv_label_set_text(tx_lbl, "Transmit");
    lv_obj_set_style_text_color(tx_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(tx_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(tx_lbl);

    s_timer = lv_timer_create(timer_cb, 1000, NULL);

    ESP_LOGI(TAG, "modal built");
}

void ft8_tx_modal_init(void)
{
    modal_build();
}

void ft8_tx_modal_show(const ft8_tx_request_t *req)
{
    if (!req) return;
    modal_build();   // no-op if already built (idempotent via s_modal guard)

    s_pending_req = *req;   // private copy - caller's may go out of scope

    lv_label_set_text(s_lbl_title, s_pending_req.protocol == FTX_PROTOCOL_FT4
                       ? "Confirm FT4 Transmission" : "Confirm FT8 Transmission");
    lv_label_set_text(s_lbl_message, s_pending_req.display_text);

    char detail[80];
    if (s_pending_req.use_parity) {
        // Both REPLY and parity-locked CQ fire on a specific slot type.
        const char *slot_name = s_pending_req.want_even_slot ? "EVEN" : "ODD";
        const char *verb      = (s_pending_req.kind == FT8_TX_KIND_REPLY)
                                ? "reply in" : "fire on";
        snprintf(detail, sizeof(detail), "Audio %d Hz  -  needs an %s slot to %s",
                 s_pending_req.audio_freq_hz, slot_name, verb);
    } else {
        snprintf(detail, sizeof(detail), "Audio %d Hz  -  fires on the next slot boundary",
                 s_pending_req.audio_freq_hz);
    }
    lv_label_set_text(s_lbl_detail, detail);

    lv_label_set_text(s_lbl_error, "");
    lv_obj_add_flag(s_lbl_error, LV_OBJ_FLAG_HIDDEN);

    // Row-nudge makes sense for any reply built from a decode-list row (REPLY
    // kind); hide it for a CQ (we *are* the caller — no row to nudge between).
    bool is_reply = (s_pending_req.kind == FT8_TX_KIND_REPLY);
    // Auto Pounce (the full auto-sequencer) is offered for any REPLY-kind
    // request: a grid TX1 (extra empty) starts in WAIT_RPT, and a report-first
    // reply (extra = "+NN"/"-NN" - Skip-TX1 manual build, pileup modal, or a
    // tapped their-grid row) is honoured as-is by ft8_qso_start() and starts
    // in WAIT_ROGER. Only the later ladder steps (ROGER_RPT / 73 kinds) would
    // mis-seed the state machine - those get single Transmit only.
    bool is_fresh_pounce = is_reply;
    lv_obj_t *pounce_objs[] = { s_btn_pounce, s_pounce_swatch, s_pounce_legend };
    for (unsigned i = 0; i < sizeof(pounce_objs) / sizeof(pounce_objs[0]); i++) {
        if (!pounce_objs[i]) continue;
        if (is_fresh_pounce) lv_obj_clear_flag(pounce_objs[i], LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_add_flag(pounce_objs[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_btn_nudge_up) {
        if (is_reply) lv_obj_clear_flag(s_btn_nudge_up, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(s_btn_nudge_up, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_btn_nudge_dn) {
        if (is_reply) lv_obj_clear_flag(s_btn_nudge_dn, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(s_btn_nudge_dn, LV_OBJ_FLAG_HIDDEN);
    }

    update_countdown();   // populate immediately - don't wait up to 1s for the first tick

    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_modal_open = true;
    ESP_LOGI(TAG, "showing confirmation: '%s' (%s)", s_pending_req.display_text, detail);
}

void ft8_tx_modal_hide(void)
{
    modal_close();
}
