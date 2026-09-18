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
#include "spot_map_view.h"
#include "storage/sd_archive.h"

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
    { HELP_WSPR,                "guide/wspr.md",                "Reading the page",          "WSPR"                 },
    // 2026-09-13: SelfSpotter (Uwe DL8UG's spot map) had a full guide page
    // (guide/spot-map.md) and no entry anywhere in this table or the triage
    // list below it feeds - reachable only by already knowing the manual's
    // index existed and going to look. This is that entry.
    { HELP_SPOTMAP,             "guide/spot-map.md",            "Opening it",                "Spot map"             },

    // Specific controls (Layer 2).
    { HELP_TAP_TO_TUNE,         "guide/panadapter.md",          "Tap to Tune",               "Tap to tune"          },
    { HELP_GESTURES,            "reference/gestures.md",        "Spectrum",                  "Gestures"             },
    { HELP_TX_TONE,             "guide/ft8-tx.md",              "Call CQ",                   "TX frequency"         },
    { HELP_CQ_PRESETS,          "guide/ft8-tx.md",              "Call CQ",                   "CQ messages"          },
    { HELP_LOGGING,             "guide/web-ui.md",              "Bottom Bar Menus",          "QSO logging"          },
    // Points at ft8-tx.md's "Upload to QRZ, eQSL & LoTW", not web-ui.md's
    // "LoTW Upload": all THREE services are asked about together, and only that
    // section walks through all three. Anchor omits the section number so it
    // survives renumbering.
    { HELP_UPLOADS,             "guide/ft8-tx.md",              "Upload to QRZ",             "Log uploads"          },
    { HELP_SPOTS_TAP,           "guide/spots.md",               "Tapping a spot",            "Tapping a spot"       },
    { HELP_ROBOT,              "guide/ft8-tx.md",               "Auto-Reply",                "Auto-reply robot"     },
    // 2026-09-13 feature-coverage audit (the SelfSpotter pass turned up how
    // much else had the same problem: real, documented features with zero
    // path in from "Need guidance?"). All seven below have full guide
    // content already - this is wiring, not new writing, except
    // HELP_RELEASE_RADIO (see its own comment).
    { HELP_RIT,                 "guide/panadapter.md",          "receiving off your transmit","RIT"                 },
    { HELP_STILL_SPECTRUM,      "guide/panadapter.md",          "Still Spectrum",            "Still spectrum"       },
    { HELP_SIM_MODE,            "guide/ft8-tx.md",              "FT8 Simulation Mode",       "Simulation mode"      },
    { HELP_SWR_PROTECTION,      "guide/settings.md",            "SWR protection",            "SWR protection"       },
    { HELP_ANTENNA_TUNE,        "guide/web-ui.md",              "Antenna Tune from the browser", "Antenna Tune"     },
    // No dedicated heading exists for this one - "Let me use the QMX menus"
    // is a bolded paragraph inside settings.md's one `## Radio` section, not
    // a heading of its own, and pack_manual.py's anchor check only matches
    // against real headings. Anchoring on the section heading lands a couple
    // of paragraphs above the right one rather than exactly on it - close
    // enough to find, and true today rather than a link that quietly rots
    // the next time that section is reordered.
    { HELP_RELEASE_RADIO,       "guide/settings.md",            "Radio",                     "Release radio"        },
    { HELP_RADIO_MENUS,         "guide/radio-menus.md",         "Opening it",                "Radio menus"          },

    // What just went wrong (Layer 3). These point at headings that ALREADY exist
    // in troubleshooting.md - none were invented for this.
    { HELP_TROUBLE_USB,         "reference/troubleshooting.md", "won't reconnect",           "Radio not connecting" },
    { HELP_TROUBLE_WIFI,        "reference/troubleshooting.md", "WiFi won't connect",        "WiFi problems"        },
    { HELP_TROUBLE_NO_DECODES,  "reference/troubleshooting.md", "decoding is slow or stops", "No FT8 decodes"       },
    { HELP_TROUBLE_TIME,        "reference/troubleshooting.md", "Time is wrong",             "Clock is wrong"       },
    { HELP_TROUBLE_NO_TX,       "reference/troubleshooting.md", "doesn't key the QMX",       "TX not keying"        },
    { HELP_TROUBLE_FLAT,        "reference/troubleshooting.md", "Spectrum is flat",          "No signal on screen"  },
    { HELP_TROUBLE_IQ,          "reference/troubleshooting.md", "shifted/mirrored",          "Spectrum looks wrong" },
    { HELP_SD_BENEFITS,         "guide/settings.md",            "Benefits of a microSD card","microSD benefits"     },
    { HELP_SPOTMAP_EMPTY,       "guide/spot-map.md",            "stays empty",               "Empty spot map"       },
    { HELP_WSPR_EMPTY,          "guide/wspr.md",                "If nothing is decoded",     "No WSPR decodes"      },
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
    bool         wspr;         // offer on the WSPR screen
    bool         spotmap;      // offer while the SelfSpotter overlay is open
} triage_cand_t;

// ⛔ SelfSpotter is an OVERLAY, not a ui_mode_t - opening it does not change
// ui_mode_get(), so without this a row set picked purely from the base mode
// silently carries whatever screen you opened the map FROM into the map
// itself. Caught live, 2026-09-13/14: opening SelfSpotter from the WSPR page
// showed "Nothing is decoding" (a WSPR row) over the map, and the WSPR page
// itself showed "The spot map is empty" (a map row) - the exact same
// category of bug patch #wspr-triage-fix already found and fixed for
// spectrum/tap-to-tune, just one layer further down. Checked FIRST in
// help_triage_collect(), ahead of the panadapter/ft8/wspr split - being IN
// the map overrides whatever screen you came from.
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
// Only meaningful while the map is actually open (see spot_map_view_spot_
// count()'s own header comment on staleness when it is not) - operator,
// live on the device: "The spot map is empty should be highlighted also".
static bool cond_spotmap_empty(void)
{
    return spot_map_view_is_active() && spot_map_view_spot_count() == 0;
}

// No card in the slot. Deliberately NOT phrased as a fault anywhere it shows:
// a Tab5 with no microSD is a perfectly normal Tab5, so this is flagged only to
// float an OFFER to the top, and the symptom text is a question rather than a
// complaint. The same state crosses out the bottom-bar SD icon, and tapping that
// icon lands on this same page - one answer, two ways in.
static bool cond_no_sd(void)      { return !sd_archive_is_mounted(); }

// Order here is the tie-break among rows that are equally (un)flagged, so it runs
// most-serious first: no radio at all, then a radio that is misbehaving, then the
// things that are merely puzzling.
//
// EVERY ROW MUST MAKE SENSE ON THE SCREEN THAT OFFERS IT. "The spectrum looks
// mirrored" was originally offered in FT8, where there is no spectrum on screen at
// all - the operator called it nonsense, and he was right: one irrelevant row is
// enough to make someone stop reading the list. The panadapter/ft8 flags are the
// mechanism, so use them rather than adding a row that has to be mentally skipped.
// Within each pass the order below is what the operator sees, so it runs: things
// that are broken first, then "how do I" questions. The list scrolls, so being
// generous costs nothing - and a question answered here saves scrolling the manual,
// which is the whole point.
// Columns: topic, symptom, condition, panadapter, ft8, wspr, spotmap.
static const triage_cand_t s_cands[] = {
    // --- Universal: mean the same thing on every screen, INCLUDING inside
    //     the SelfSpotter overlay (the map still needs the radio and WiFi). ---
    { HELP_TROUBLE_USB,        "My radio is not showing up",              cond_no_radio,   true,  true,  true,  true  },
    { HELP_TROUBLE_WIFI,       "I cannot reach the web page",             cond_no_wifi,    true,  true,  true,  true  },

    // --- microSD discovery row. Universal on purpose: the card is a
    //     whole-device facility, not a feature of one screen. Flagged when
    //     there is no card in, so it floats up exactly for the operator who
    //     stands to gain from reading it and drops to an ordinary row for the
    //     one who already has one. ---
    { HELP_SD_BENEFITS,        "What do I get from an SD card?",          cond_no_sd,      true,  true,  true,  true  },

    // --- SelfSpotter fault row. spotmap:true ONLY - "the spot map is empty"
    //     is meaningless language unless you are actually looking at it, and
    //     until 2026-09-14 this had panadapter/ft8/wspr all true instead,
    //     which is a DIFFERENT mistake from the one below but the same
    //     lesson: "reachable from every screen" (true for the discovery row
    //     right after this one) is not the same claim as "relevant on every
    //     screen" (false for a fault specific to being inside the overlay).
    //     Now has a REAL condition - operator, live on the device: "The spot
    //     map is empty should be highlighted also". ---
    { HELP_SPOTMAP_EMPTY,      "The spot map is empty",                   cond_spotmap_empty, false, false, false, true },

    // --- SelfSpotter discovery row. This one genuinely IS universal - it is
    //     how you find the feature from any screen you might be on - so it
    //     keeps panadapter/ft8/wspr true and ALSO offers on spotmap itself
    //     ("what does this page do"), unlike the fault row above it. ---
    { HELP_SPOTMAP,            "How do I see who is hearing me?",         NULL,            true,  true,  true,  true  },

    // --- FT8/FT4 problems. No spectrum is drawn here, so nothing about the
    //     spectrum belongs, however tempting the shared wording is. ---
    { HELP_TROUBLE_NO_DECODES, "Nothing appears in the decode list",      cond_no_decodes, false, true,  false, false },
    { HELP_TROUBLE_NO_TX,      "It never transmits",                      NULL,            false, true,  false, false },
    { HELP_TROUBLE_TIME,       "Decodes look late, or the timer is off",  NULL,            false, true,  false, false },
    { HELP_FT8_TX,             "Nobody answers my CQ",                    NULL,            false, true,  false, false },

    // --- Panadapter problems. The IQ and flat-spectrum symptoms are things you can
    //     only SEE on a spectrum, so they are offered here and not in FT8 (where the
    //     IQ topic is still one tap from the warning banner, which is tappable). ---
    { HELP_TROUBLE_IQ,         "The spectrum looks mirrored or shifted",  cond_iq_bad,     true,  false, false, false },
    { HELP_TROUBLE_FLAT,       "The spectrum is flat - no signals",       NULL,            true,  false, false, false },
    { HELP_TAP_TO_TUNE,        "Tapping the screen tunes the wrong way",  NULL,            true,  false, false, false },

    // --- WSPR problems. Its own page, its own questions - it was silently
    //     inheriting the panadapter rows above (spectrum/tap-to-tune) until
    //     2026-09-13, because this file only ever distinguished ft8 from
    //     "everything else". Operator, live on the device: "the base page
    //     WSPR has wrong Need Guidance sentences about spectrum and tap to
    //     tune". spotmap:false for the same reason HELP_SPOTMAP_EMPTY is
    //     false everywhere else - "nothing is decoding" is a WSPR-page
    //     statement, not a SelfSpotter one, and the overlay not changing
    //     ui_mode_get() is exactly what let this leak into the map when it
    //     was opened FROM the WSPR page (found the same evening as the fix
    //     above, same root cause one layer down). ---
    { HELP_WSPR_EMPTY,         "Nothing is decoding",                     NULL,            false, false, true,  false },

    // --- FT8/FT4 how-to ---
    { HELP_FT8_RX,             "How do I answer a station I can see?",    NULL,            false, true,  false, false },
    { HELP_TX_TONE,            "Which frequency am I transmitting on?",   NULL,            false, true,  false, false },
    { HELP_CQ_PRESETS,         "How do I change what my CQ says?",        NULL,            false, true,  false, false },
    { HELP_ROBOT,              "Can it work stations by itself?",         NULL,            false, true,  false, false },
    { HELP_SIM_MODE,           "Can I practice without a real station?",  NULL,            false, true,  false, false },
    { HELP_LOGGING,            "Where are my contacts logged?",           NULL,            false, true,  false, false },
    { HELP_UPLOADS,            "How do I send my log to QRZ, eQSL or LoTW?", NULL,         false, true,  false, false },

    // --- Panadapter how-to ---
    { HELP_GESTURES,           "How do I zoom or pan the spectrum?",      NULL,            true,  false, false, false },
    { HELP_SPOTS,              "What are the coloured call signs?",       NULL,            true,  false, false, false },
    { HELP_SPOTS_TAP,          "How do I tune to a spotted station?",     NULL,            true,  false, false, false },
    { HELP_PANADAPTER,         "How do I change band or filter width?",   NULL,            true,  false, false, false },
    { HELP_RIT,                "How do I receive off my transmit frequency?", NULL,        true,  false, false, false },
    { HELP_STILL_SPECTRUM,     "Why does the display hold still while I tune?", NULL,       true,  false, false, false },

    // --- WSPR how-to ---
    { HELP_WSPR,               "What does this page show?",               NULL,            false, false, true,  false },

    // --- Shared how-to: cross-cutting radio-level features, same question
    //     regardless of which screen you asked it from - INCLUDING inside
    //     the map, since these describe the radio, not any one screen. ---
    { HELP_TIME_SYNC,          "How does it know the time?",              NULL,            true,  true,  true,  true  },
    { HELP_SETTINGS,           "Where do I find the settings?",           NULL,            true,  true,  true,  true  },
    { HELP_WEB_UI,             "What can the web interface do?",          NULL,            true,  true,  true,  true  },
    { HELP_SWR_PROTECTION,     "Will it protect the radio if my SWR is bad?", NULL,         true,  true,  true,  true  },
    { HELP_ANTENNA_TUNE,       "How do I tune my antenna?",               NULL,            true,  true,  true,  true  },
    // 2026-09-13 audit: both below had complete guide content and were
    // reachable from nowhere in "Need guidance?" at all - not low down, not
    // present. The QMX's own front-panel menus fight with CAT for the same
    // serial port (see settings.md's "Radio" section), which is exactly the
    // kind of thing a symptom-first list exists to surface.
    { HELP_RELEASE_RADIO,      "I need to use the QMX's own menus",       NULL,            true,  true,  true,  true  },
    { HELP_RADIO_MENUS,        "Can I see the radio's menus on this screen?", NULL,         true,  true,  true,  true  },
};

int help_triage_collect(help_triage_row_t *out, int max)
{
    if (!out || max <= 0) return 0;
    // ⛔ Used to be a bare bool (ft8 or "everything else"), so the WSPR page
    // silently got the PANADAPTER rows - spectrum/tap-to-tune nonsense on a
    // page with neither. Found live, 2026-09-13: "the base page WSPR has
    // wrong Need Guidance sentences about spectrum and tap to tune". Three
    // screens now get three genuinely separate selections.
    //
    // ⛔ AND SelfSpotter IS A FOURTH SCREEN THAT THIS SAME BUG HID IN, ONE
    // LAYER DOWN. The overlay does not change ui_mode_get(), so checking mode
    // alone means the map silently shows whatever the underlying page's rows
    // are - found live 2026-09-14, opening the map FROM the WSPR page showed
    // "Nothing is decoding" over the map, and the WSPR page showed "The spot
    // map is empty" back. spot_map_view_is_active() is checked FIRST and, if
    // true, wins outright: being IN the map overrides whatever screen you
    // opened it from, exactly the same fix as the ft8/wspr split above, one
    // level further in.
    const bool in_spotmap = spot_map_view_is_active();
    const ui_mode_t mode = ui_mode_get();
    const bool ft8  = !in_spotmap && (mode == UI_MODE_FT8);
    const bool wspr = !in_spotmap && (mode == UI_MODE_WSPR);
    int n = 0;

    // Two passes rather than a sort: flagged rows first, each pass already in
    // seriousness order. Keeps it allocation-free and obviously stable.
    for (int pass = 0; pass < 2 && n < max; pass++) {
        const bool want_flagged = (pass == 0);
        for (size_t i = 0; i < sizeof(s_cands) / sizeof(s_cands[0]) && n < max; i++) {
            const triage_cand_t *c = &s_cands[i];
            bool on_this_screen = in_spotmap ? c->spotmap
                                 : wspr       ? c->wspr
                                 : ft8        ? c->ft8
                                              : c->panadapter;
            if (!on_this_screen) continue;
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
    /* The WSPR page asks its own questions - the two-minute rhythm, what the
     * columns mean, why nothing decoded - and none of them are answered by the
     * panadapter chapter. */
    if (ui_mode_get() == UI_MODE_WSPR) return HELP_WSPR;
    return HELP_PANADAPTER;
}
