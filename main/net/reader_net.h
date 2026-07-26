#pragma once
#include <stdbool.h>

// Content layer for the on-device docs Reader (ui/reader_view.c).
//
// The user manual is built into the firmware binary (see net/manual_embed.h), so
// a page view needs no WiFi, no SD card and no download, and can never show
// documentation for a different firmware version than the one running. This
// module just resolves a page from that blob into the SPIFFS render cache.
//
// Everything network- and SD-related that used to live here was removed once the
// manual moved into the binary - see the note at the top of reader_net.c for what
// went and why, so it does not come back.

// Load the manual's index page plus the contents list. Safe from the LVGL thread.
void reader_net_load_index(void);

// Load a specific page by its relative path from the contents list (e.g.
// "guide/ft8-tx.md"). `with_toc` also refreshes the contents list. No-op while a
// load is already in flight. Safe from the LVGL thread.
void reader_net_fetch(const char *page_rel, bool with_toc);

// Clear the Reader's SPIFFS render caches. They are rebuilt from the embedded
// manual on the next page open, so this is just a "reset the reader" action
// (wired to a >=3 s hold on the drawer's User Manual button).
void reader_net_erase_all(void);
