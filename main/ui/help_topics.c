// Context help topic table. Contract and rationale in help_topics.h.
//
// Keep the page paths and anchors in step with docs/mkdocs/** - the build checks
// them (tools/pack_manual.py) and fails if a page or heading has gone.

#include "help_topics.h"
#include "reader_view.h"
#include "ui.h"
#include "ui_mode.h"
#include "ft8_tx.h"
#include "ft8_screen.h"
#include "cat.h"
#include "wifi/wifi.h"

#include "esp_log.h"

static const char *TAG = "help";

// Anchors are heading SUBSTRINGS, matched case-insensitively. Prefer the shortest
// distinctive fragment: it survives renumbering and small wording edits.
static const help_entry_t s_topics[] = {
    // Where you are (Layer 1). Anchors chosen so the operator lands on the part
    // that answers "what am I looking at", not on a chapter title they then have
    // to scroll past.
    { HELP_PANADAPTER,          "guide/panadapter.md",          "Layout",                    "Panadapter"           },
    { HELP_FT8_RX,              "guide/ft8-rx.md",              "Decode List",               "FT8 receive"          },
    { HELP_FT8_TX,              "guide/ft8-tx.md",              "Modes of Transmission",     "FT8 transmit"         },
    { HELP_SETTINGS,            "guide/settings.md",            "",                          "Settings"             },
    { HELP_SPOTS,               "guide/spots.md",               "What you see",              "Live spots"           },
    { HELP_WEB_UI,              "guide/web-ui.md",              "Quick Start",               "Web interface"        },
    { HELP_TIME_SYNC,           "guide/time-sync.md",           "Time Sources",              "Time sync"            },

    // Specific controls (Layer 2).
    { HELP_TAP_TO_TUNE,         "guide/panadapter.md",          "Tap to Tune",               "Tap to tune"          },
    { HELP_GESTURES,            "reference/gestures.md",        "Spectrum",                  "Gestures"             },
    { HELP_TX_TONE,             "guide/ft8-tx.md",              "Call CQ",                   "TX frequency"         },
    { HELP_CQ_PRESETS,          "guide/ft8-tx.md",              "Call CQ",                   "CQ messages"          },
    { HELP_LOGGING,             "guide/web-ui.md",              "Bottom Bar Menus",          "QSO logging"          },
    { HELP_UPLOADS,             "guide/web-ui.md",              "LoTW Upload",               "Log uploads"          },
    { HELP_SPOTS_TAP,           "guide/spots.md",               "Tapping a spot",            "Tapping a spot"       },
    { HELP_ROBOT,              "guide/ft8-tx.md",               "Auto-Reply",                "Auto-reply robot"     },

    // What just went wrong (Layer 3). These point at headings that ALREADY exist
    // in troubleshooting.md - none were invented for this.
    { HELP_TROUBLE_USB,         "reference/troubleshooting.md", "won't reconnect",           "Radio not connecting" },
    { HELP_TROUBLE_WIFI,        "reference/troubleshooting.md", "WiFi won't connect",        "WiFi problems"        },
    { HELP_TROUBLE_NO_DECODES,  "reference/troubleshooting.md", "decoding is slow or stops", "No FT8 decodes"       },
    { HELP_TROUBLE_TIME,        "reference/troubleshooting.md", "Time is wrong",             "Clock is wrong"       },
    { HELP_TROUBLE_NO_TX,       "reference/troubleshooting.md", "doesn't key the QMX",       "TX not keying"        },
    { HELP_TROUBLE_FLAT,        "reference/troubleshooting.md", "Spectrum is flat",          "No signal on screen"  },
    { HELP_TROUBLE_IQ,          "reference/troubleshooting.md", "shifted/mirrored",          "Spectrum looks wrong" },
};

const help_entry_t *help_topic_get(help_topic_t t)
{
    for (size_t i = 0; i < sizeof(s_topics) / sizeof(s_topics[0]); i++)
        if (s_topics[i].topic == t) return &s_topics[i];
    return NULL;
}

void help_open(help_topic_t t)
{
    const help_entry_t *e = help_topic_get(t);
    if (!e) return;                       // HELP_NONE, or an id with no entry yet
    ESP_LOGI(TAG, "opening help: %s", e->label);
    reader_view_open_help(e->page, e->anchor);
}

// --- triage ----------------------------------------------------------------

// One candidate row: the symptom text, the topic it resolves to, and the live
// condition that says it is happening NOW. A NULL condition means "offer this as
// a normal question for this screen, but never claim it is the problem".
typedef struct {
    help_topic_t topic;
    const char  *symptom;
    bool       (*happening_now)(void);
    bool         panadapter;   // offer on the panadapter screen
    bool         ft8;          // offer in FT8/FT4
} triage_cand_t;

static bool cond_no_radio(void)   { return !cat_is_ready(); }
static bool cond_iq_bad(void)     { return ui_iq_mode_warning_active(); }
// Only a fault if WiFi is supposed to be up. Someone operating POTA with WiFi
// deliberately off must not be told their network is broken - the row stays in the
// list as a normal question, it just is not flagged as happening now.
static bool cond_no_wifi(void)    { return panadapter_wifi_is_enabled() && !wifi_is_connected(); }
static bool cond_no_decodes(void)
{
    // Only meaningful in FT8/FT4, and only once the radio is actually there -
    // otherwise "nothing is decoding" is just a restatement of "no radio", and
    // two rows would be competing to describe one fault.
    if (ui_mode_get() != UI_MODE_FT8 || !cat_is_ready()) return false;
    return ft8_screen_active_count() == 0;
}

// Order here is the tie-break among rows that are equally (un)flagged, so it runs
// most-serious first: no radio at all, then a radio that is misbehaving, then the
// things that are merely puzzling.
//
// EVERY ROW MUST MAKE SENSE ON THE SCREEN THAT OFFERS IT. "The spectrum looks
// mirrored" was originally offered in FT8, where there is no spectrum on screen at
// all - the operator called it nonsense, and he was right: one irrelevant row is
// enough to make someone stop reading the list. The panadapter/ft8 flags are the
// mechanism, so use them rather than adding a row that has to be mentally skipped.
static const triage_cand_t s_cands[] = {
    // Shared: a missing radio and an unreachable web page mean the same thing in
    // either mode.
    { HELP_TROUBLE_USB,        "My radio is not showing up",              cond_no_radio,   true,  true  },

    // FT8/FT4. No spectrum is drawn here, so nothing about the spectrum belongs.
    { HELP_TROUBLE_NO_DECODES, "Nothing appears in the decode list",      cond_no_decodes, false, true  },
    { HELP_TROUBLE_NO_TX,      "It never transmits",                      NULL,            false, true  },
    { HELP_TROUBLE_TIME,       "Decodes look late, or the timer is off",  NULL,            false, true  },
    { HELP_FT8_TX,             "Nobody answers my CQ",                    NULL,            false, true  },
    { HELP_FT8_RX,             "How do I answer a station I can see?",    NULL,            false, true  },
    { HELP_TX_TONE,            "Which frequency am I transmitting on?",   NULL,            false, true  },

    // Panadapter. The IQ warning is here and NOT in FT8: its symptom is something
    // you can only see on a spectrum. In FT8 the same topic is still one tap away
    // from the warning banner itself, which is tappable (Layer 3).
    { HELP_TROUBLE_IQ,         "The spectrum looks mirrored or shifted",  cond_iq_bad,     true,  false },
    { HELP_TROUBLE_FLAT,       "The spectrum is flat - no signals",       NULL,            true,  false },
    { HELP_TAP_TO_TUNE,        "Tapping the screen tunes the wrong way",  NULL,            true,  false },

    { HELP_TROUBLE_WIFI,       "I cannot reach the web page",             cond_no_wifi,    true,  true  },
};

int help_triage_collect(help_triage_row_t *out, int max)
{
    if (!out || max <= 0) return 0;
    const bool ft8 = (ui_mode_get() == UI_MODE_FT8);
    int n = 0;

    // Two passes rather than a sort: flagged rows first, each pass already in
    // seriousness order. Keeps it allocation-free and obviously stable.
    for (int pass = 0; pass < 2 && n < max; pass++) {
        const bool want_flagged = (pass == 0);
        for (size_t i = 0; i < sizeof(s_cands) / sizeof(s_cands[0]) && n < max; i++) {
            const triage_cand_t *c = &s_cands[i];
            if (!(ft8 ? c->ft8 : c->panadapter)) continue;
            bool now = c->happening_now ? c->happening_now() : false;
            if (now != want_flagged) continue;
            out[n].topic   = c->topic;
            out[n].symptom = c->symptom;
            out[n].flagged = now;
            n++;
        }
    }
    return n;
}

help_topic_t help_topic_for_current_context(void)
{
    // FT8/FT4: split on whether the operator is transmitting or about to. Someone
    // with a burst armed is asking a different question from someone watching the
    // decode list, and the device already knows which.
    if (ui_mode_get() == UI_MODE_FT8) {
        ft8_tx_state_t st = ft8_tx_get_status(NULL, 0, NULL);
        if (st == FT8_TX_ARMED || st == FT8_TX_ACTIVE) return HELP_FT8_TX;
        return HELP_FT8_RX;
    }
    return HELP_PANADAPTER;
}
