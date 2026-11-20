#pragma once
/* Host shim for esp_attr.h. On the Tab5 EXT_RAM_BSS_ATTR places a static in
 * PSRAM; on a host there is one kind of RAM, so it is nothing.
 *
 * Added 2026-11-21: main/adif/adif_log.c has included esp_attr.h since
 * f6d65527 (T197), and from then until this file existed
 * test/build_adif_worked_cache.sh failed to compile - the regression test for
 * the QSO log could not be built, and nothing said so until
 * tools/check_harness_buildlines.py learned to run harness scripts. */
#define EXT_RAM_BSS_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
