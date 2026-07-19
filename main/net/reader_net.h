#pragma once

// Network layer for the on-device docs Reader (reader_view.c).
//
// Fetches the tab5.lav.dk documentation markdown over HTTPS into a SPIFFS cache
// file (/spiffs/reader.md) on a background PSRAM-stack task, then notifies
// reader_view to re-render on the LVGL thread. On failure it leaves any existing
// cache in place so the Reader still works offline (POTA).
//
// See docs/reader-page-and-update-check-plan.md.

// Kick a background refresh of the docs index page. No-op if a fetch is already
// running, or if WiFi is disabled/down. Safe to call from the LVGL thread.
void reader_net_load_index(void);
