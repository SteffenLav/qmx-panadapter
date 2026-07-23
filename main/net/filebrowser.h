// Simple web-based microSD file browser.
//
// Serves a self-contained page at /files that lists the card, downloads files,
// uploads files, and deletes them — from any browser on any OS (Win/Mac/Linux/
// phone), with zero client setup: no drive mapping, no WebClient service, no
// third-party software. Chosen over a mounted network drive (WebDAV/SMB)
// precisely because those need per-OS setup that non-technical operators trip
// over; a URL in a browser does not.
//
// All SD access goes through sd_archive_lock() and pauses the spectrum WS +
// DSP transfers for the duration of a download/upload, matching the discipline
// the reader "Save offline" and log-download paths already use for the fragile
// SD-during-WiFi combo (see CLAUDE.md).
//
// Endpoints (all rooted at /sdcard, with a ".." traversal guard):
//   GET  /files                 -> the browser page (HTML)
//   GET  /api/files?path=<rel>  -> JSON listing of a directory
//   GET  /api/file?path=<rel>   -> download a file
//   POST /api/file?path=<rel>   -> upload/overwrite a file (raw body)
//   DELETE /api/file?path=<rel> -> delete a file or directory (recursive)

#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the file-browser handlers on the given (already-started) server.
// Adds 5 URI handlers.
void filebrowser_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
