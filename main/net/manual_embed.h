#pragma once
#include <stdbool.h>
#include <stddef.h>

// The user manual, built into the firmware binary.
//
// Packed by tools/pack_manual.py (run from mkdocs_reader_export.py during
// `mkdocs build`) and linked in via EMBED_FILES, so the manual is ALWAYS
// available: WiFi on or off, SD card or none, first boot out of the box. It also
// can never document a different firmware version than the one running, because
// it ships inside it.
//
// This replaced downloading the manual over WiFi and mirroring it to the SD card.
// Both were unreliable here - SD writes only work before WiFi starts (see
// storage/sd_archive.c) and the docs host rate-limits bulk downloads - and both
// are now unnecessary. Cost: ~136 KB in an 8 MB app partition with ~5.5 MB free,
// and zero SPIFFS, which stays entirely for the QSO log and diagnostics.
//
// The data is memory-mapped rodata, so a "read" is just a pointer: the returned
// buffer is NOT NUL-terminated and must not be freed or written to.

// Look up one entry by its relative path ("toc.json", "guide/ft8-tx.md").
// Returns false if the manual blob is missing/corrupt or the path is not in it.
bool manual_embed_get(const char *rel, const char **out_data, size_t *out_len);

// Number of packed entries (pages + toc), or 0 if the blob is unusable.
int manual_embed_count(void);

// One-line boot check: entry count, total size, and that every entry's offsets
// lie inside the blob. Cheap (one pass over ~18 records) and worth keeping - it
// is how a truncated or mis-packed manual shows up in a field log instead of
// silently rendering "page not found" to the operator.
void manual_embed_log_summary(void);
