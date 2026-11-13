#pragma once
// Close a stdio stream that was WRITTEN, and say whether the bytes landed.
//
// ⛔ fsync()'S RETURN IS THE ONLY PLACE A SPIFFS FLUSH FAILURE IS REPORTED.
// With CONFIG_SPIFFS_CACHE_WR the last partial page of a write sits in the
// SPIFFS write cache. fsync() -> SPIFFS_fflush() -> spiffs_fflush_cache()
// writes it out, and on failure (ENOSPC) it records the error and then
// RELEASES THE CACHE PAGE ANYWAY - the bytes are discarded. fsync() returns
// -1; nothing sets the FILE's error flag, so ferror() stays 0; and the later
// fclose() finds no cache page left to flush and returns 0.
// (IDF v5.4.4 components/spiffs/spiffs/src/spiffs_hydrogen.c:843-873.)
//
// Every SPIFFS writer in this firmware ignored fsync()'s return, so a full
// partition was reported as a successful write. Field case 2026-11-12, Randy
// N4OPI: qso.adi stopped at 511,846 B with its last record cut off mid-field,
// while the firmware went on logging "Logged QSO #2716..#2721" and counting
// them on screen. A reboot reloaded the real count and the contacts were gone.
// The three log-rewrite paths had the same hole and were worse: they renamed
// the short temp file OVER the good log.
//
// Order matters: ferror() first (an fwrite/fprintf that already failed),
// fflush() (stdio buffer -> SPIFFS), fsync() (SPIFFS cache -> flash), then
// fclose(). The stream is always closed, whatever the result.
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

static inline bool file_commit_close(FILE *f)
{
    if (!f) return false;
    bool ok = (ferror(f) == 0);
    if (ok && fflush(f) != 0)        ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0)              ok = false;
    return ok;
}
