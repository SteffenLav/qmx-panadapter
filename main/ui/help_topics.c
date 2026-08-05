// Context help topic table. Contract and rationale in help_topics.h.
//
// Keep the page paths and anchors in step with docs/mkdocs/** - the build checks
// them (tools/pack_manual.py) and fails if a page or heading has gone.

#include "help_topics.h"
#include "reader_view.h"
#include "ui.h"
#include "ui_mode.h"
#include "ft8_tx.h"

#include "esp_log.h"

static const char *TAG = "help";

// Anchors are heading SUBSTRINGS, matched case-insensitively. Prefer the shortest
// distinctive fragment: it survives renumbering and small wording edits.
static const help_entry_t s_topics[] = {
    { HELP_PANADAPTER,          "guide/panadapter.md",          "",                  "Panadapter"          },
    { HELP_FT8_RX,              "guide/ft8-rx.md",              "",                  "FT8 receive"         },
    { HELP_FT8_TX,              "guide/ft8-tx.md",              "",                  "FT8 transmit"        },
    { HELP_SETTINGS,            "guide/settings.md",            "",                  "Settings"            },
    { HELP_SPOTS,               "guide/spots.md",               "",                  "Live spots"          },
    { HELP_WEB_UI,              "guide/web-ui.md",              "",                  "Web interface"       },
    { HELP_TIME_SYNC,           "guide/time-sync.md",           "",                  "Time sync"           },

    { HELP_TAP_TO_TUNE,         "guide/panadapter.md",          "Tap to Tune",       "Tap to tune"         },
    { HELP_GESTURES,            "reference/gestures.md",        "",                  "Gestures"            },
    { HELP_TX_TONE,             "guide/ft8-tx.md",              "",                  "TX frequency"        },
    { HELP_CQ_PRESETS,          "guide/ft8-tx.md",              "",                  "CQ messages"         },
    { HELP_LOGGING,             "guide/ft8-tx.md",              "",                  "QSO logging"         },
    { HELP_UPLOADS,             "guide/web-ui.md",              "",                  "Log uploads"         },

    { HELP_TROUBLE_USB,         "reference/troubleshooting.md", "",                  "Radio not connecting" },
    { HELP_TROUBLE_WIFI,        "reference/troubleshooting.md", "",                  "WiFi problems"        },
    { HELP_TROUBLE_NO_DECODES,  "reference/troubleshooting.md", "",                  "No FT8 decodes"       },
    { HELP_TROUBLE_TIME,        "guide/time-sync.md",           "",                  "Clock is wrong"       },
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
