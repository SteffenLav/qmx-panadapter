// On-device pileup list - tappable version of adif_view_modal.c's read-only
// list pattern. See ft8_pileup_modal.h for the concept.

#include "ft8_pileup_modal.h"
#include "ft8_pileup.h"
#include "ft8_tx.h"
#include "ft8_tx_modal.h"
#include "ft8_qso.h"            // ft8_qso_fmt_report(), ft8_qso_build_manual_reply()
#include "ft8_screen.h"         // decode-table snapshot for the intelligent reply
#include "storage/settings.h"   // Field Day mode check
#include "esp_heap_caps.h"
#include "ui_theme.h"
#include "ui.h"
#include "util/dxcc.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"

static const char *TAG = "ft8_pileup_modal";

static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_list  = NULL;
static bool      s_open  = false;

static void list_render(void);

static void modal_close(void)
{
    if (!s_modal || !s_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

// Work this station: re-resolve from a fresh pileup snapshot (rows aren't
// permanently bound to an entry, same reasoning as ft8_screen_view.c's
// row_activate()) and hand off to the normal TX confirmation modal - its
// "Auto Pounce" button is what actually calls ft8_qso_start(), which removes
// the entry from the pileup list itself once the QSO is committed to.
static void row_work_cb(lv_event_t *e)
{
    lv_obj_t *l_call = (lv_obj_t *)lv_event_get_user_data(e);
    const char *call = l_call ? lv_label_get_text(l_call) : NULL;
    if (!call || !call[0]) return;

    ft8_pileup_entry_t snap[FT8_PILEUP_MAX];
    int n = ft8_pileup_get_all(snap, FT8_PILEUP_MAX);
    const ft8_pileup_entry_t *match = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].call, call) == 0) { match = &snap[i]; break; }
    }
    if (!match) {
        ESP_LOGW(TAG, "row work: '%s' no longer in the pileup - refreshing", call);
        list_render();
        return;
    }

    // Reply on our own clear tone, same as a live decode-list row tap -
    // their stored frequency/last-seen-slot only feed the parity calc.
    int reply_freq_hz = ft8_find_clear_tone_hz();

    // If the caller is still in the live decode table, build the correct
    // NEXT message from their actual last transmission (the intelligent-
    // Transmit ladder), exactly like a decode-row tap. This is what makes a
    // hunting-mode comeback work from the pileup: a station we called
    // earlier answers with a signal report minutes later - the right reply
    // is "R"+our report, which the report-first fallback below can never
    // produce (Roy KI0ER field report: only the grid could be sent, the
    // exchange was lost). Fall back to report-first when they've aged out.
    {
        ft8_call_t *snap = heap_caps_malloc(
            sizeof(ft8_call_t) * FT8_CALL_TABLE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (snap) {   // PSRAM - LVGL task stack is far too small for this
            int n = 0;
            ft8_screen_get_all(snap, FT8_CALL_TABLE_SIZE, &n);
            for (int i = 0; i < n; i++) {
                if (strcmp(snap[i].call, match->call) != 0) continue;
                ft8_tx_request_t ireq;
                char ierr[64];
                if (ft8_qso_build_manual_reply(&snap[i], reply_freq_hz,
                                               &ireq, NULL, ierr, sizeof(ierr))) {
                    ESP_LOGI(TAG, "pileup row work: %s via last message '%s'",
                             match->call, snap[i].last_text);
                    heap_caps_free(snap);
                    modal_close();
                    ft8_tx_modal_show(&ireq);
                    return;
                }
                break;
            }
            heap_caps_free(snap);
        }
    }

    // A pileup entry called US, so reply the way cqrun_answer() does:
    // report-first, never grid TX1 (that's the answering station's message -
    // the wrong role here, and it's what actually went on-air before this
    // fix: the station has usually aged out of the live decode table, so
    // ft8_qso_start()'s skip-TX1 scan missed and fell back to grid; Ken
    // KF0AYY field report, 2026-07-15). Building the report into the request
    // here means the TX modal previews exactly what will transmit, and
    // ft8_qso_start() sees the report-shaped extra_field and starts in
    // WAIT_ROGER regardless of the Skip-TX1 toggle. The report is their SNR
    // as heard when they called us - the message we're answering. Field Day
    // mode keeps the grid TX1 path (the FD class+section exchange is sent at
    // the report step by the QSO machine, same as a decode-list pounce).
    qmx_settings_t qs;
    settings_load_all(&qs);
    bool fd_mode = qs.field_day_en && qs.fd_class[0] && qs.fd_section[0];
    char rpt[8];
    const char *extra = NULL;
    if (!fd_mode) {
        ft8_qso_fmt_report(match->snr_db, rpt, sizeof(rpt));
        extra = rpt;
    }

    ft8_tx_request_t req;
    char err[64];
    if (ft8_tx_build_request(FT8_TX_KIND_REPLY, match->call, reply_freq_hz,
                             match->last_seen_utc, extra, &req, err, sizeof(err))) {
        ESP_LOGI(TAG, "pileup row work: %s report=%s (their_freq=%d Hz -> our_freq=%d Hz)",
                 match->call, extra ? extra : "(grid TX1, FD mode)",
                 (int)match->freq_hz, reply_freq_hz);
        modal_close();
        ft8_tx_modal_show(&req);
    } else {
        ESP_LOGW(TAG, "build_request(pileup %s) failed: %s", match->call, err);
    }
}

static void row_dismiss_cb(lv_event_t *e)
{
    lv_obj_t *l_call = (lv_obj_t *)lv_event_get_user_data(e);
    const char *call = l_call ? lv_label_get_text(l_call) : NULL;
    if (!call || !call[0]) return;
    ft8_pileup_remove(call);
    list_render();
}

static void add_pileup_row(lv_obj_t *parent, const ft8_pileup_entry_t *entry, bool even_row)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(even_row ? UI_COLOR_SURFACE_RAISED : UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l_call = lv_label_create(row);
    lv_label_set_text(l_call, entry->call);
    lv_obj_set_style_text_font(l_call, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l_call, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(l_call, LV_ALIGN_LEFT_MID, 12, 0);

    const char *country = dxcc_lookup(entry->call);
    lv_obj_t *l_country = lv_label_create(row);
    lv_label_set_text(l_country, country ? country : "-");
    lv_obj_set_style_text_font(l_country, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l_country, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_align(l_country, LV_ALIGN_LEFT_MID, 190, 0);

    char snrbuf[12];
    snprintf(snrbuf, sizeof(snrbuf), "%+d dB", entry->snr_db);
    lv_obj_t *l_snr = lv_label_create(row);
    lv_label_set_text(l_snr, snrbuf);
    lv_obj_set_style_text_font(l_snr, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l_snr, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_align(l_snr, LV_ALIGN_LEFT_MID, 420, 0);

    // Age since last heard - entry->last_seen_utc is a slot-start UTC second.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t age_s = (int64_t)tv.tv_sec - entry->last_seen_utc;
    if (age_s < 0) age_s = 0;
    char agebuf[24];
    if (age_s < 60) snprintf(agebuf, sizeof(agebuf), "%llds ago", (long long)age_s);
    else            snprintf(agebuf, sizeof(agebuf), "%lldm ago", (long long)(age_s / 60));
    lv_obj_t *l_age = lv_label_create(row);
    lv_label_set_text(l_age, agebuf);
    lv_obj_set_style_text_font(l_age, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l_age, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_align(l_age, LV_ALIGN_LEFT_MID, 550, 0);

    // Dismiss - own click zone (a child button, no LV_OBJ_FLAG_EVENT_BUBBLE),
    // so tapping it does not also fire the row's own "work" handler below.
    lv_obj_t *dismiss = lv_btn_create(row);
    lv_obj_set_size(dismiss, 48, 48);
    lv_obj_align(dismiss, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(dismiss, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(dismiss, 6, 0);
    lv_obj_set_style_border_width(dismiss, 0, 0);
    lv_obj_t *dismiss_lbl = lv_label_create(dismiss);
    lv_label_set_text(dismiss_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(dismiss_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(dismiss_lbl);
    lv_obj_add_event_cb(dismiss, row_dismiss_cb, LV_EVENT_CLICKED, l_call);

    // Work: tap anywhere else on the row.
    lv_obj_add_event_cb(row, row_work_cb, LV_EVENT_CLICKED, l_call);
}

static void list_render(void)
{
    ft8_pileup_entry_t entries[FT8_PILEUP_MAX];
    int n = ft8_pileup_get_all(entries, FT8_PILEUP_MAX);

    if (s_title) {
        char t[40];
        snprintf(t, sizeof(t), "Pileup - %d waiting", n);
        lv_label_set_text(s_title, t);
    }
    if (!s_list) return;
    lv_obj_clean(s_list);

    if (n == 0) {
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "No one waiting");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        add_pileup_row(s_list, &entries[i], (i % 2) == 1);
    }
    ESP_LOGI(TAG, "pileup viewer: showing %d waiting", n);
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
    lv_obj_set_width(s_panel, 820);
    lv_obj_set_height(s_panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_panel, 640, 0);
    lv_obj_set_align(s_panel, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_pad_all(s_panel, 16, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 12, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_panel);
    lv_label_set_text(s_title, "Pileup");
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);

    lv_obj_t *hint = lv_label_create(s_panel);
    lv_label_set_text(hint, "Tap a row to work them - X to dismiss without working");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);

    s_list = lv_obj_create(s_panel);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_list, 440, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    lv_obj_t *close_btn = lv_btn_create(s_panel);
    lv_obj_set_size(close_btn, 240, 72);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(close_lbl);
    ui_kbd_set_buttons(NULL, close_btn);   // physical keyboard Esc -> Close
}

void ft8_pileup_modal_init(void)
{
    modal_build();
}

void ft8_pileup_modal_show(void)
{
    modal_build();
    list_render();
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_open = true;
}
