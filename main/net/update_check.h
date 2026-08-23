#pragma once
#include <stdbool.h>
#include <stddef.h>   // size_t, for update_check_get_asset_url()

// Automatic firmware-update check for the on-device Reader (v1.1+).
//
// Polls the GitHub Releases API for the newest release (pre-releases count) and
// compares its tag against the running firmware version
// (esp_app_get_description()->version). Falls back to a static
// https://tab5.lav.dk/latest.json when the API is unreachable. When a newer
// version is found it pushes an "update available" banner to reader_view.
//
// See docs/reader-page-and-update-check-plan.md §2.

// Start the background poller (spawns one PSRAM-stack task). Call once after
// WiFi/webserver bring-up. No-op if already started. The task self-throttles
// (first check shortly after WiFi is up, then every UPDATE_CHECK_INTERVAL).
void update_check_start(void);

// Latest version string discovered ("v1.2.3"), or "" if none/unknown. Copies
// into out (safe from any task).
void update_check_get_latest(char *out, int out_sz);

// True if a newer-than-running version has been found.
bool update_check_available(void);

// The download URL for the latest release's firmware image, or "" when no
// newer release is known. ONE place builds this: status.c's manual path and
// the #239 auto-download must not each carry their own copy of the release
// asset layout, or a rename breaks one of them silently.
void update_check_get_asset_url(char *out, size_t out_sz);

// Ask for a check RIGHT NOW instead of waiting for the next interval. Used by
// /api/cmd {"action":"update_check"} so a release can be watched arriving
// rather than waited for - and so "is the update system working?" is a question
// with a five-second answer.
void update_check_now(void);
