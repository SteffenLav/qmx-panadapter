#pragma once
#include <stdbool.h>

// Network layer for the on-device docs Reader (reader_view.c).
//
// Fetches the tab5.lav.dk documentation markdown over HTTPS into a SPIFFS cache
// file (/spiffs/reader.md) on a background PSRAM-stack task, then notifies
// reader_view to re-render on the LVGL thread. On failure it leaves any existing
// cache in place so the Reader still works offline (POTA).
//
// See docs/reader-page-and-update-check-plan.md.

// Kick a background refresh of the docs index page plus the TOC. No-op if a
// fetch is already running. Safe to call from the LVGL thread.
void reader_net_load_index(void);

// Fetch a specific docs page (relative path from toc.json, e.g.
// "guide/panadapter.md"). `with_toc` also (re)fetches toc.json. Result written
// to the SPIFFS cache, then reader_view is notified. When offline, falls back to
// the SD manual mirror (if a card holds a saved copy). No-op if busy.
void reader_net_fetch(const char *page_rel, bool with_toc);

// Download the ENTIRE manual (toc + every page) to the microSD card for offline
// use, on a background task. Progress is reported via reader_view's status line.
// No-op if busy, WiFi is down, or no card is mounted. Safe from the LVGL thread.
void reader_net_save_offline(void);

// Erase every stored copy of the manual: SPIFFS page/TOC caches + the whole
// microSD offline mirror. Recovery for a cache poisoned by a captive portal
// (hotel WiFi). Triggered by a >=3 s hold on the drawer's User Manual button.
void reader_net_erase_all(void);
