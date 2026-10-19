#include "js8_submode.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ft8_qso.h"
#include "ft8_test.h"          /* ft8_op_mode_protocol() - for the log line   */
#include "ft8_tx.h"
#include "storage/settings.h"
#include "ui/ft8_screen_view.h"  /* the MODE label names the submode          */
#include "ft8/constants.h"       /* ftx_protocol_label()                      */

static const char *TAG = "js8sub";

/* ⛔ ONE TABLE. Adding a submode means adding a row here and nothing else:
 * the drawer, the JS8 screen picker and /api/cmd all read it. */
static const struct {
    uint8_t     value;          /* JS8Call NSUBMODE */
    const char *label;
} S[] = {
    { 0, "Normal - 15 s" },
    { 1, "Fast - 10 s"   },
    { 2, "Turbo - 6 s"   },
    { 4, "Slow - 30 s"   },
};
#define N_SUBMODES ((int)(sizeof(S) / sizeof(S[0])))

int js8_submode_count(void) { return N_SUBMODES; }

uint8_t js8_submode_value(int index)
{
    if (index < 0 || index >= N_SUBMODES) return 0;
    return S[index].value;
}

int js8_submode_index_of(uint8_t value)
{
    for (int i = 0; i < N_SUBMODES; i++)
        if (S[i].value == value) return i;
    return 0;                   /* unknown reads as Normal, same as the clamp */
}

const char *js8_submode_label(int index)
{
    if (index < 0 || index >= N_SUBMODES) return S[0].label;
    return S[index].label;
}

const char *js8_submode_options_lvgl(void)
{
    /* Built once, on first use. A static buffer rather than a literal so the
     * table above stays the only place the list is written. */
    static char buf[96];
    if (buf[0] == 0) {
        size_t n = 0;
        for (int i = 0; i < N_SUBMODES && n < sizeof(buf) - 1; i++)
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s",
                                  i ? "\n" : "", S[i].label);
    }
    return buf;
}

bool js8_submode_set(uint8_t value, const char **why)
{
    static const char *BUSY =
        "busy - finish or cancel the QSO first";
    static const char *BAD =
        "not a JS8 submode";

    if (js8_submode_value(js8_submode_index_of(value)) != value) {
        /* index_of() falls back to Normal, so this catches a value that is not
         * in the table rather than silently setting Normal. */
        if (why) *why = BAD;
        return false;
    }

    /* ⛔ ARMED COUNTS AS BUSY, not just keyed. An armed request already
     * carries tones built for the OLD submode, and changing the physical
     * layer mid-exchange abandons whoever is being worked. The same rule the
     * band and mode controls follow. */
    if (ft8_qso_is_busy(NULL, 0) ||
        ft8_tx_get_status(NULL, 0, NULL) != FT8_TX_IDLE) {
        if (why) *why = BUSY;
        return false;
    }

    settings_set_js8_submode(value);

    /* The slot loop picks this up through ft8_op_mode_protocol() and rebuilds
     * the monitor pool at the top of the next slot - nothing to do here.
     *
     * ⛔ EXCEPT THE LABEL, which names the submode and is the only thing on
     * the screen that can explain why JS8 has stopped decoding. The call is
     * flag-based, so it is safe from the HTTP task as well as from LVGL. */
    ft8_screen_view_refresh_mode_label();

    ESP_LOGI(TAG, "JS8 submode -> %s",
             ftx_protocol_label(ft8_op_mode_protocol()));
    if (why) *why = NULL;
    return true;
}
