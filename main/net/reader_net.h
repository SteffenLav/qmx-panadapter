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

// Download the ENTIRE manual (toc + every page) for offline use, on a background
// task. Progress is reported via reader_view's status line. Needs WiFi but NOT a
// card: this is STAGE 1 of a two-stage save.
//
// WHY TWO STAGES. Writing to the SD card requires WiFi to be down. The card is
// only reachable before esp_hosted brings up its SDIO link - hardware-verified
// 2026-07-26: with WiFi never started the card mirrored flawlessly for 230 s,
// while with WiFi up it becomes unreachable within 10-140 s and cannot be
// remounted (the MALLOC_CAP_DMA pool is ~400 B by then). The old one-shot save
// needed WiFi and the card at the SAME time, which is exactly the combination
// that fails - it only ever worked inside the short window before the card died.
//
// So stage 1 downloads to a SPIFFS staging area (survives a reboot), and stage 2
// flushes it to the card early on the next boot, before WiFi starts. The Reader's
// button text guides the operator through both steps.
void reader_net_save_offline(void);

// True if a COMPLETE staged manual is waiting to be written to the card. The
// staging manifest is written last, so a partial or interrupted download never
// looks ready.
bool reader_net_has_staged_manual(void);

// STAGE 2: copy the staged manual onto the card, then delete the staging copy.
// MUST be called early in boot, before panadapter_wifi_start(), with the card
// mounted - that is the only window in which SD writes are reliable. Returns the
// number of files written (0 if nothing staged or the card is unavailable).
int reader_net_flush_staged_to_sd(void);

// Bench/test only: fabricate a COMPLETE staged manual (a minimal toc.json plus
// one page) without any network access, so stage 2's pre-WiFi flush can be
// verified on its own. Returns true if staging was created.
bool reader_net_bench_stage_fake(void);

// Cache whether the card already holds a saved manual. Call once at boot while
// the card is still reachable; reader_net_manual_on_sd() then answers without
// touching the card (which may be dead later in the session).
void reader_net_probe_sd_manual(void);
bool reader_net_manual_on_sd(void);

// Erase every stored copy of the manual: SPIFFS page/TOC caches + the whole
// microSD offline mirror. Recovery for a cache poisoned by a captive portal
// (hotel WiFi). Triggered by a >=3 s hold on the drawer's User Manual button.
void reader_net_erase_all(void);
