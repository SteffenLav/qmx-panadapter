#pragma once

/* ---- Memory ledger ------------------------------------------------------
 *
 * Internal RAM is THE constraint on this board and nothing measured where it
 * goes. On 2026-09-25 the bench sat at 19 KB free / 10-12 KB largest block
 * with audio on, which is enough to cost three separate features:
 *   - the SD diag mirror (esp_dma_capable_malloc refused, backing off to 240 s),
 *   - four network feeds (gated off whenever RX audio is on),
 *   - the BLE mouse (its guard needs 24576 B and measured 21391 B).
 *
 * dma_owners can attribute it, but only with CONFIG_HEAP_TASK_TRACKING, which
 * stores an owner handle in EVERY block header and so changes the very numbers
 * being measured (documented in sdkconfig.defaults, measured 2026-08-28).
 *
 * This is the non-perturbing alternative: print free DMA-capable and internal
 * bytes either side of each major init, so the consumer is named by
 * SUBTRACTION rather than by attribution. Two heap_caps_get_free_size() calls
 * and one log line per milestone - no tracking, no allocation, nothing that
 * changes what it observes.
 *
 * Deliberately INFO and deliberately permanent: every field diagnostic then
 * carries the same ledger, so a user reporting "no Bluetooth" or a yellow SD
 * dot arrives with the reason already in the file.
 *
 * ⚠ IT LIVES IN A HEADER NOW (2026-10-04), not in main.c where it was born.
 * app_main is not the only place that spends this pool, and the 2026-10-04
 * measurement proved the interesting spend happens AFTER app_main has
 * returned - in the network tasks. A mark there has to be callable from
 * wifi.c, mdns_svc.c, webserver.c, rigctld_server.c and spots.c. The
 * definition in main.c was deleted in the same commit; there is no second
 * copy. */

#include "esp_log.h"
#include "esp_heap_caps.h"

#define MEM_LEDGER(stage)                                                     \
    ESP_LOGI("memledger", "%-26s dma=%6u B (lblk %5u)  int=%6u B (lblk %5u)", \
             (stage),                                                         \
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),               \
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),      \
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),          \
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))
