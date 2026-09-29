#pragma once
// One flag: "esp_hosted's SDIO card init has finished".
//
// WHY THIS EXISTS (2026-09-06, measured on the dev bench)
// ------------------------------------------------------
// The SD mount and esp_hosted's SDIO card init both want MALLOC_CAP_DMA, and
// they were overlapping for no reason other than that nobody had sequenced
// them. Measured at the exact moment the card init allocates:
//
//     SD archive ON   ->  DMA free  2039 B   largest block  1152 B
//     SD archive OFF  ->  DMA free 19927 B   largest block 10240 B
//
// i.e. the SD mount leaves WiFi about 2 KB to work with, and a card init that
// fails used to REBOOT the device (a bare `return;` from a FreeRTOS task
// function, jumping to address 0 - see the sdio_drv.c patch).
//
// The obvious fixes are both bad. Mounting the SD first (what app_main did)
// starves WiFi. Starting WiFi first and leaving it there kills the SD archive
// outright, because CLAUDE.md records that before-WiFi is "the only window in
// which SD writes are reliable on this board".
//
// ⭐ But "before WiFi" was always a PROXY for "before the contention". The
// contention is a specific, short, identifiable event - the card init - so we
// can aim at the real thing instead: let WiFi take its memory first, and mount
// the SD once the card init has finished. Both get their allocation at a
// moment when the other is not asking.
//
// ⚠ HYPOTHESIS, NOT A CONCLUSION. If the card then refuses to mount because a
// LIVE SDIO link is what actually breaks it, the proxy was right and the
// honest answer is to go back to SD-first - which is survivable now that a
// failed card init loses WiFi for the session instead of rebooting.
//
// Defined in FIRMWARE and only extern-declared inside the patched
// managed_components file, deliberately that way round: same reasoning as
// util/usb_patch_counters.c, so a missing patch leaves the flag unset rather
// than failing the link.

#include <stdbool.h>
#include <stdint.h>

// Called from the patched esp_hosted sdio_drv.c once the card init returns OK.
// Safe from any task; idempotent.
void qmx_sdio_card_ready_set(void);

// Block until the card init has finished, up to timeout_ms. Returns true if it
// actually completed, false on timeout.
//
// ⚠ A TIMEOUT MUST NOT MEAN "never mount". WiFi can be switched off, the patch
// can be missing after a fullclean, or the init can fail - in all three the
// flag never arrives, and an unbounded wait would silently disable the SD
// archive. The caller proceeds on false.
bool qmx_sdio_card_ready_wait(uint32_t timeout_ms);

// Whether the flag is set, without blocking.
bool qmx_sdio_card_ready(void);
