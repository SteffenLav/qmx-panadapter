// "Check my log" - Don WB0LQW (#263).
//
// His words: "I wish POTA and SOTA had a feature allowing you to test your
// file for syntax and completeness without submitting it for credit". While
// working out the ADIF changes he made two trips to a park purely to get fresh
// contacts to test with, because POTA rightly refuses contacts made from your
// armchair. POTA and SOTA are not going to add a dry run; nothing stops the
// Tab5 checking its own file.
//
// ⛔ THE ONE RULE: this can only ever say a record is not OBVIOUSLY incomplete.
// It must NEVER read as "POTA will accept this" - it does not know their rules,
// it cannot see their database, and a confident green tick that turns into a
// rejected activation is worse than no check at all. Every string this produces
// is phrased as what is MISSING or MALFORMED, never as approval.
//
// Portable: no ESP-IDF, no LVGL, no allocation. The caller extracts the fields
// (adif_log_extract_field() on the device) and this decides; that split is what
// lets test/adif_check_harness.c link the REAL function rather than a copy of
// its logic.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// One problem per bit, so a record can carry several.
#define ADIF_CHK_NO_CALL          (1u << 0)   // no CALL - the record is unusable
#define ADIF_CHK_NO_DATE          (1u << 1)   // no QSO_DATE
#define ADIF_CHK_BAD_DATE         (1u << 2)   // QSO_DATE not 8 digits
#define ADIF_CHK_NO_TIME          (1u << 3)   // no TIME_ON
#define ADIF_CHK_BAD_TIME         (1u << 4)   // TIME_ON not 4 or 6 digits
#define ADIF_CHK_NO_BAND          (1u << 5)   // no BAND
#define ADIF_CHK_NO_MODE          (1u << 6)   // no MODE
#define ADIF_CHK_NO_STATION_CALL  (1u << 7)   // no STATION_CALLSIGN - who worked them
#define ADIF_CHK_BAD_MY_REF       (1u << 8)   // MY_SIG_INFO does not look like a reference
#define ADIF_CHK_BAD_THEIR_REF    (1u << 9)   // SIG_INFO does not look like a reference
#define ADIF_CHK_NO_MY_REF        (1u << 10)  // activating, but this record names no park

// NULL and "" mean the same thing here (absent) - adif_log_extract_field()
// leaves the buffer empty on a miss, and a caller that memsets is as valid as
// one that passes NULL.
typedef struct {
    const char *call;
    const char *qso_date;
    const char *time_on;
    const char *band;
    const char *mode;
    const char *station_call;
    const char *my_sig_info;    // the park/summit WE were activating
    const char *sig_info;       // theirs (park-to-park)
} adif_check_fields_t;

// `activating` = the caller believes this log is an activation, so a record
// with no MY_SIG_INFO earns ADIF_CHK_NO_MY_REF. Off for an ordinary log, where
// its absence is entirely correct and flagging it would be noise.
uint32_t adif_check_record(const adif_check_fields_t *f, bool activating);

// A short, plain sentence for the highest-priority bit set, or NULL if none.
// Deliberately returns ONE - a row listing six complaints is not read.
const char *adif_check_first_problem(uint32_t flags);

// Runs the same cases as test/adif_check_harness.c, ON THE DEVICE, and returns
// the number that failed (0 = all passed). Exists because the bench machine
// this was written on has NO HOST C COMPILER, so the harness could not be run
// here - and shipping a checker whose logic nobody had executed, into a feature
// an operator leans on before submitting an activation, is not acceptable.
// Reached via /api/cmd {"action":"adif_check_test"}; logs each failure.
int adif_check_selftest(void);
