#pragma once

// Context help topics: the map from "what the operator is doing / what just went
// wrong" to a place in the built-in user manual.
//
// WHY A TABLE AND NOT CALL SITES SCATTERED EVERYWHERE
// Every help affordance - the drawer's User Manual button, a tappable warning
// banner, the term index - resolves to one of these IDs, so there is exactly one
// place that knows which manual page covers what. That matters because the manual
// is regenerated from docs/mkdocs/** on every build: `tools/pack_manual.py`
// verifies each page path exists in the blob and each anchor matches a real
// heading, and FAILS THE BUILD otherwise. Deep links that rot silently are worse
// than no deep links, because the operator stops trusting them.
//
// The anchor is a case-insensitive SUBSTRING of the heading, so "Tap to Tune"
// matches "### 2. Tap to Tune" and survives renumbering.

typedef enum {
    HELP_NONE = 0,

    // --- Where you are (Layer 1: the drawer's User Manual button) ---
    HELP_PANADAPTER,
    HELP_FT8_RX,
    HELP_FT8_TX,
    HELP_SETTINGS,
    HELP_SPOTS,
    HELP_WEB_UI,
    HELP_TIME_SYNC,

    // --- What you are looking at (Layer 2: specific controls) ---
    HELP_TAP_TO_TUNE,
    HELP_GESTURES,
    HELP_TX_TONE,
    HELP_CQ_PRESETS,
    HELP_LOGGING,
    HELP_UPLOADS,
    HELP_SPOTS_TAP,
    HELP_ROBOT,

    // --- What just went wrong (Layer 3: tappable warnings) ---
    HELP_TROUBLE_USB,
    HELP_TROUBLE_WIFI,
    HELP_TROUBLE_NO_DECODES,
    HELP_TROUBLE_TIME,
    HELP_TROUBLE_NO_TX,
    HELP_TROUBLE_FLAT,
    HELP_TROUBLE_IQ,

    HELP_TOPIC_COUNT
} help_topic_t;

typedef struct {
    help_topic_t topic;
    const char  *page;      // path inside the embedded manual, e.g. "guide/ft8-tx.md"
    const char  *anchor;    // heading substring, or "" for the page top
    const char  *label;     // short human name, for the index and log lines
} help_entry_t;

// The table itself (help_topics.c). NULL-safe lookup; returns NULL for HELP_NONE
// or an unknown id.
const help_entry_t *help_topic_get(help_topic_t t);

// Open the manual at a topic. No-op for HELP_NONE, so callers can resolve a
// context that may have no sensible page and just pass the result through.
void help_open(help_topic_t t);

// What is the operator doing right now? Used by the drawer's User Manual button
// so a short tap lands somewhere useful instead of always on the contents page.
help_topic_t help_topic_for_current_context(void);
