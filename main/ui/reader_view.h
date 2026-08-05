#pragma once
#include "lvgl.h"
#include <stdbool.h>

// On-device docs Reader page (v1.1+).
//
// A full-screen opaque overlay that renders the tab5.lav.dk documentation as a
// scrollable, text-forward view. It is NOT a browser: there is no HTML engine
// on this device. It renders a *markdown subset* read from a SPIFFS cache file
// (/spiffs/reader.md) that reader_net.c fetches from the site. See
// docs/reader-page-and-update-check-plan.md for the full design.
//
// The overlay is created (hidden) during ui_init, BEFORE the edge-swipe strips,
// so those always-on-top strips stay above it and the operator can always swipe
// back out. Show/hide slide the overlay in/out from the right edge; the page is
// reached as the third page of the left-swipe cycle
// (Panadapter -> FT8 -> Reader -> Panadapter).
//
// Threading: the render itself runs on the LVGL thread via a 4 Hz timer that
// picks up volatile flags. The notify/set functions below are safe to call from
// any task (they only touch small mutex-protected state, never LVGL).

void reader_view_init(lv_obj_t *parent);

// Context help: open the manual AT a specific place and, when `anchor` is given,
// scroll to the first heading containing it (case-insensitive substring, so an
// anchor survives small heading edits). Pass NULL/"" for the page top.
//
// This is the single entry point every "help me with THIS" affordance uses. Exit
// needs no special handling: the Reader is an overlay, so leaving it reveals the
// screen the operator came from, already in the state they left it.
//
// Anchors are validated at build time by tools/pack_manual.py against the actual
// headings in docs/mkdocs/**, so a renamed heading breaks the build instead of
// quietly landing users on the top of a long page three releases later.
void reader_view_open_help(const char *page_rel, const char *anchor);
void reader_view_show(void);   // slide overlay in; kick a (re)load if stale
void reader_view_hide(void);   // slide overlay out
bool reader_view_is_active(void);
lv_obj_t *reader_view_get_container(void);

// --- Notifications from the network layer (safe from any task) ---

// The cache file (/spiffs/reader.md) now holds fresh content; ask the LVGL
// thread to re-render it. `from_cache` = true when the content is the previously
// cached copy shown because a live fetch failed (drives the "offline" note).
void reader_view_notify_loaded(bool from_cache);

// A fetch is in progress / failed with no cache — sets the status line only.
void reader_view_notify_status(const char *status);

// The TOC cache file (/spiffs/reader_toc.json) now holds fresh content; ask the
// LVGL thread to re-parse it and rebuild the contents list. Safe from any task.
void reader_view_notify_toc_loaded(void);

// Result of an offline "Save to SD" run — flips the button label to "Saved
// offline" (ok) or "Save failed". Safe from any task.

// "Firmware vX available" banner, from update_check.c. Pass NULL/empty version
// to clear it. Safe from any task.
void reader_view_set_update_available(const char *latest_version);
