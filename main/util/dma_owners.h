#pragma once

// === TEMP INSTRUMENT (#283) - delete with the diagnosis, see TODO #283.
//
// Reports which TASK owns the MALLOC_CAP_DMA bytes, via ESP-IDF's per-task heap
// tracking. Built to answer "who consumed the pool" directly, after .bss creep,
// the NimBLE tuning and Bluetooth itself had each been eliminated by
// measurement and the only remaining method was one reboot per guess.
//
// ⛔ ON DEMAND ONLY. It walks the heap with interrupts off - the documented
// cause of this panel's cyan flash (see #281). Never put it on a periodic path.
//
// ⚠ Needs CONFIG_HEAP_TASK_TRACKING, which adds an owning-task handle to every
// block header. Absolute free figures on such a build are therefore lower than
// on a shipping build: read the ATTRIBUTION, not the totals.
void dma_owners_report(void);
