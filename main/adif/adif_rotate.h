#pragma once
// QSO log rotation: move the oldest records of the device log (SPIFFS) to the
// append-only archive on the SD card. See docs/qso-log-rotation-design.md.
//
// Portable C with no ESP-IDF dependency: every byte of I/O goes through
// rot_io_t, so tools/spiffs_host/rot.c runs THIS file against the real SPIFFS
// code with power cuts injected at every flash write. Change the logic here,
// re-run that harness.
//
// The device log's header line carries two fields that say what is on the
// card:
//   <APP_QMXPAN_ARCHIVED:n>N       records moved to the archive so far
//   <APP_QMXPAN_ARCHIVE_BYTES:n>B  archive file size after the last move
// The device log rewrite is the single commit point of a rotation. Before it,
// the moved records are in BOTH files; the next run finds them on the card
// past B, checks byte for byte that they are the head of the device log, and
// finishes the move. Never in neither file.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROT_DEV 0   // /spiffs/qso.adi
#define ROT_TMP 1   // /spiffs/qso.tmp
#define ROT_ARC 2   // the archive on the card

// Rotate when the device log's records exceed ROT_CAP_BYTES, down to
// ROT_LOW_BYTES. The gap stops every boot from rewriting the log for the few
// records logged since the last one.
#define ROT_CAP_BYTES (128 * 1024)
#define ROT_LOW_BYTES (96 * 1024)

typedef struct {
    void *ctx;
    // Work buffer, at least 2 KB. The caller owns it (PSRAM on the device;
    // internal .bss is the scarcest thing this firmware has).
    char *buf;
    size_t buf_sz;
    // mode: "rb", "wb" or "ab". NULL on failure.
    void *(*open)(void *ctx, int which, const char *mode);
    long  (*read)(void *ctx, void *h, void *dst, long n);
    long  (*write)(void *ctx, void *h, const void *src, long n);
    bool  (*seek)(void *ctx, void *h, long off);
    // True only when every byte written through h is durable (fsync checked).
    bool  (*close)(void *ctx, void *h);
    long  (*size)(void *ctx, int which);          // -1 = missing
    bool  (*remove)(void *ctx, int which);
    bool  (*rename_tmp)(void *ctx);               // ROT_TMP -> ROT_DEV
    bool  (*truncate)(void *ctx, int which, long len);   // ROT_ARC only (FAT)
    // SPIFFS admission (util/spiffs_guard.h). Caller of rot_run holds the guard.
    bool  (*reserve)(void *ctx, long need);
    void  (*log)(void *ctx, bool warn, const char *msg);
    // Optional. Called right after a relink committed (the log's header was
    // reset because the card has no archive): the cursor base must go to 0
    // before anything else moves.
    void  (*relinked)(void *ctx);
} rot_io_t;

typedef struct {
    uint32_t archived;    // records on the card
    uint32_t arc_bytes;   // archive size after the last completed move; 0 = never
} rot_hdr_t;

// The archive's own first line. qso.adi on the card is this archive followed
// by the device log's records, so it is a valid ADIF file by itself.
extern const char ROT_ARC_HEADER[];

// Parse the two fields out of a header line. Absent fields read as 0.
void rot_hdr_parse(const char *line, size_t len, rot_hdr_t *h);
// Format the device log's header line, '\n' included. Returns its length.
int  rot_hdr_format(char *out, size_t sz, const rot_hdr_t *h);

typedef enum {
    ROT_NOTHING = 0,   // under the cap, nothing pending
    ROT_DONE,          // records moved; see rotated
    ROT_MISMATCH,      // the card's archive is not this log's; nothing done
    ROT_FULL,          // SPIFFS refused the rewrite; records are on the card, move finishes next run
    ROT_IO,            // a read or write failed; nothing lost, see why
} rot_status_t;

typedef struct {
    rot_status_t status;
    uint32_t rotated;     // records removed from the device log this run
    uint32_t finished;    // of those, already on the card from an interrupted run
    uint32_t held;        // over the cap but kept: an upload has not sent them
    uint32_t dev_records; // records on the device log afterwards
    rot_hdr_t hdr;        // header on the device log afterwards
    long arc_size;        // archive size afterwards (-1 = none)
    bool relinked;        // the link to a missing archive was reset first
    uint32_t relinked_from; // records the old link said were on the card
    char why[128];
} rot_result_t;

// One rotation. max_rotate: how many records from the head may leave the
// device log - the lowest cursor of every configured upload (records an
// upload has not sent must stay where it reads them). UINT32_MAX = no limit.
// Caller holds the SPIFFS guard and has cut any pending fragment.
void rot_run(const rot_io_t *io, uint32_t max_rotate, rot_result_t *r);
