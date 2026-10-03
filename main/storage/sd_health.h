// sd_health - does this card actually hold what it says it holds?
//
// The operator, 2026-10-03: "how can i know that my sd card is not corrupted
// in a way that it still works (kind of) but looking at it in explorer show
// various file pieces that should not be there..... is there any way the Tab5
// can varify the health of a inserted SD card??"
//
// ⛔ WHAT THIS CANNOT DO: repair. FatFs ships no chkdsk and ESP-IDF does not
// provide one. Repair stays a PC job, and for a card carrying recovered
// fragments the honest advice is to copy anything wanted off it and reformat
// rather than let chkdsk stitch it back together.
//
// What it CAN do is the test Explorer never performs - actually read the data.
// Explorer shows directory entries; it does not touch the clusters behind
// them, so a card whose contents are unreadable still "looks fine".
//
//   1. READ EVERY FILE, end to end. Unreadable sectors surface here as read
//      errors. This is the slow part and the point of the whole exercise.
//   2. WRITE AND VERIFY a known pattern. The nastiest failure class is a card
//      that accepts a write, reports success, and stores nothing - counterfeit
//      and worn-out cards behave exactly that way and look perfect in a file
//      manager. A round trip through a closed file catches it.
//   3. NAME THE WRECKAGE. FOUND.*, *.CHK and friends are lost cluster chains
//      Windows recovered - the signature of a filesystem cut off mid-write,
//      which is what an abrupt reset does (see project_sd_boot_mount...). They
//      are evidence of history, not of a dying card, and saying so is more use
//      than a bare pass/fail.
//   4. CROSS-CHECK the free space against what the walk actually found. A
//      large disagreement means the FAT does not match reality.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SD_HEALTH_IDLE = 0,
    SD_HEALTH_RUNNING,
    SD_HEALTH_DONE,
    SD_HEALTH_FAILED,      /* could not run at all - no card, or busy */
} sd_health_state_t;

typedef struct {
    sd_health_state_t state;
    /* Progress, so a long walk can say what it is doing rather than appear hung. */
    uint32_t files_seen;
    uint32_t dirs_seen;
    uint64_t bytes_read;
    char     current[324];     /* the path being read right now; sized to hold
                                * the deepest path the walk can build, so a long
                                * name is shown rather than silently cut */

    /* Findings. */
    uint32_t read_errors;      /* files that would not read to the end */
    uint32_t suspect_names;    /* FOUND.*, *.CHK, lost-chain leftovers */
    bool     write_verify_ok;  /* the pattern came back byte-identical */
    bool     write_verify_run;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char     verdict[160];     /* one sentence, for the operator */
} sd_health_report_t;

/* Starts the walk on its own task. Returns false if a card is not mounted or a
 * check is already running. Safe to call from any task. */
bool sd_health_start(void);

/* Ask it to stop early; the report keeps whatever it found. */
void sd_health_cancel(void);

/* A snapshot, safe to call while it runs. */
void sd_health_get(sd_health_report_t *out);
