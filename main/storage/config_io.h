#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// QMX Panadapter config import/export as an editable INI-style text file.
// Lets the user back up all settings (+ memory channels) to a file on the PC,
// edit them there, restore after a clean-erase flash, or share just a section
// (e.g. [memories]) with another operator.

// Serialize current settings + memory channels into a malloc'd, NUL-terminated
// INI text blob (caller must free()). *out_len receives the byte length
// (excluding the NUL) if non-NULL. Returns NULL on allocation failure.
char *config_io_export(size_t *out_len);

// Parse an INI-style config blob (NUL-terminated, MUTABLE — it is modified in
// place) and MERGE it into NVS: only keys/sections present in the text are
// applied; everything else is left untouched. Memory channels take effect
// immediately; other settings take effect on the next restart. The settings
// are flushed to NVS before returning. Returns the number of keys/slots applied.
int config_io_import(char *text);

#ifdef __cplusplus
}
#endif
