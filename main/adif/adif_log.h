#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

// Completed QSO record passed to adif_log_record().
typedef struct {
    const char *their_call;   // Their callsign (required)
    const char *my_call;      // Our callsign
    const char *my_grid;      // Our Maidenhead grid (4 or 6 char), may be NULL/empty
    const char *their_grid;   // Their grid (may be NULL/empty)
    uint32_t    freq_hz;      // VFO frequency in Hz
    const char *mode;         // "FT8"
    const char *rst_sent;     // Signal report sent (e.g. "-07" or "599")
    const char *rst_rcvd;     // Signal report received (e.g. "-10" or "599")
    time_t      qso_time;     // UTC unix timestamp at QSO completion
    // ARRL Field Day exchange (optional - NULL/empty when not a Field Day QSO).
    const char *my_arrl_class;    // our class, e.g. "16A"
    const char *my_arrl_section;  // our section, e.g. "EMA"
    const char *their_arrl_class; // their class, e.g. "3A"
    const char *their_arrl_section; // their section, e.g. "NNJ"
    // THEIR activation, when we are the chaser: the park/summit the station we
    // worked was activating. Written as SIG/SIG_INFO. NULL/empty for an
    // ordinary QSO. OUR OWN activation is NOT here on purpose - it comes from
    // settings inside adif_log_record(), so no caller can forget it.
    const char *their_sig;        // "POTA" / "SOTA"
    const char *their_sig_info;   // their reference, e.g. "DL-0123"
} adif_qso_t;

// Mount SPIFFS and prepare the ADIF log file. Loads the worked-call cache.
// Call once at boot after NVS is initialised (does not depend on WiFi/CAT).
void adif_log_init(void);

// Append one completed QSO to the ADIF file.
// Thread-safe. Blocks briefly while writing (~1-2 ms on SPIFFS).
void adif_log_record(const adif_qso_t *qso);

// Number of QSOs in the log (in-memory counter, no I/O).
int adif_log_count(void);

// Absolute path to the ADIF file on SPIFFS (e.g. "/spiffs/qso.adi").
const char *adif_log_file_path(void);

// True if `call` appears in any previously logged QSO (ANY band).
// Uses an in-memory cache (up to ADIF_WORKED_CACHE entries) loaded at init.
// O(n) linear scan — fine for the cache size used.
bool adif_log_contains_call(const char *call);

// True if `call` was previously logged ON THE BAND that contains freq_hz.
// The same station on a different band is NOT "worked before" (it's a new
// band-slot), so the auto-answer worked-before filter uses this, not the
// any-band variant above. freq_hz is mapped to an ADIF band internally.
bool adif_log_contains_call_on_band(const char *call, uint32_t freq_hz);

// Erase all logged QSOs and reset the worked-call cache.
void adif_log_clear(void);

// Delete the idx-th QSO record (0-based, log order) - rewrites the file
// without that line, decrements any QRZ/eQSL/LoTW upload cursor that had
// already advanced past it (they are counts into the record sequence, so
// later records would otherwise shift under them), and rebuilds the count +
// worked-call cache. Returns false if idx is out of range or I/O fails.
bool adif_log_delete_record(int idx);

// Delete every record want_gone() returns true for, in ONE file rewrite.
// Returns the number removed, or -1 on failure (log left untouched).
//
// ⛔ Use this for any multi-record delete. Looping adif_log_delete_record() is
// O(N²) - each call rewrites the whole file - and measured 0.25 deletions/s
// with 525 records, i.e. half an hour to clear 500 (#325).
// ⚠ Not for taskLVGL: one rewrite of a large log takes hundreds of ms.
int adif_log_delete_matching(bool (*want_gone)(int idx, const char *raw, void *ctx),
                             void *ctx);

// Replace, add or remove one field in one record. value NULL/"" removes the
// field, which is the honest representation of "never exchanged" - so an
// operator can get a report back to absent, not only to a different number.
// Does NOT touch upload cursors: the remote copy of an already-uploaded QSO is
// unchanged, and rewinding a cursor would re-upload the whole log.
bool adif_log_set_field(int idx, const char *field, const char *value);

// How many logged QSOs carry this MY_SIG_INFO - i.e. how many contacts the
// current activation has. POTA wants 10 for a valid activation, so this is the
// number an activator is actually counting in the field. One pass over the
// file; case-insensitive on the reference.
int adif_log_count_activation(const char *sig_info);

// Read the idx-th completed QSO record (0-based, in log order) as a single
// ADIF line with no trailing newline. Returns false if idx is out of range
// or the file can't be read. out must be sized generously - a record line
// is normally well under 256 bytes.
bool adif_log_get_record(int idx, char *out, size_t out_sz);

// Extract an ADIF field value (<FIELD:len>value) from a single record line as
// returned by adif_log_get_record(). Returns false if the field is absent or
// doesn't fit in out_sz. The "<FIELD:" tag is '<'-anchored, so e.g. "CALL"
// never matches inside "STATION_CALLSIGN".
bool adif_log_extract_field(const char *line, const char *field,
                            char *out, size_t out_sz);

// Merge records from a raw ADIF file (any standard export - WSJT-X,
// ADIFMaster, or our own earlier "ADIF download") into the log. Normalises
// each pretty-printed multi-line record to our own one-line-per-record
// convention, skips a record already present (same CALL+QSO_DATE+TIME_ON -
// re-importing the same file twice is a no-op), and updates the count and
// worked-call cache exactly as adif_log_record() would. Does NOT touch
// upload cursors - the caller decides whether these are already-uploaded
// history or fresh records to send.
// Returns the number of records actually added (0 on an all-duplicate
// import), or -1 if the file could not be written at all.
int adif_log_import(const char *adif_text);

// What an import actually did. "added" alone cannot tell a caller whether an
// import of 0 means "every one of these was already logged" or "not one of them
// could be read", and reporting the first when the second happened is how a
// user ends up trusting a restore that never restored anything. The config
// import next door already carries a comment about exactly this mistake.
typedef struct {
    int found;      // blocks terminated by <EOR> seen in the input
    int added;      // written to the log
    int duplicate;  // already present (same CALL+QSO_DATE+TIME_ON)
    int unreadable; // no CALL, or too long for one record - could not be used
} adif_import_result_t;

// As adif_log_import(), but also reports what happened to every record. *res is
// zeroed first and is filled in even when the return value is -1.
int adif_log_import_ex(const char *adif_text, adif_import_result_t *res);

// Restore the QSO log from the copy the SD auto-archive already keeps on the
// card (/sdcard/qmx-panadapter/qso.adi). Same merge semantics as
// adif_log_import() - existing contacts are skipped, so running it twice is
// harmless - so this is a safe thing to offer as a plain button.
//
// This exists because the backup was previously one-way. Gyula HA3HZ lost his
// log to a clean reinstall with 432 QSOs mirrored on the card and no way to
// reach them without a PC; recovering it needed a file transfer, a browser and
// the knowledge that the file was there. A backup you cannot restore from the
// device is not a backup.
//
// Returns records added, -1 if the card could not be read (no card, no file, a
// read error), or -1 with res->found > 0 if the log could not be written.
int adif_log_import_from_sd(adif_import_result_t *res);

// The ADIF BAND string this module would log for a frequency ("20M", "40M"...),
// or "" if it falls outside every known band. Exposed so callers comparing two
// frequencies "same band?" use the same table the log itself is written from -
// a private copy could disagree with the records it's reasoning about. Returns a
// pointer to static storage; do not free.
const char *adif_log_band_for_freq(uint32_t hz);
