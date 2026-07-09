#pragma once

// FT8 pileup tracker — manual-only "who's called me and I haven't worked
// yet" list. Populated passively from decode traffic (see ft8_qso.c's
// capture_pileup_callers(), called every decode cycle regardless of state)
// and surfaced via ft8_pileup_modal.c so the operator can go back and work
// someone who called during a busy CQ-run, even after they've stopped
// transmitting and aged out of the live decode list (ft8_screen.c ages rows
// out after FT8_ROW_STALE_SEC=60s; this list has no such expiry — an entry
// only leaves when worked or manually dismissed, or gets evicted for space).
//
// Deliberately NOT automatic: nothing in this module ever arms a TX. Tapping
// an entry in the UI goes through the exact same ft8_tx_modal_show()
// confirmation flow as tapping a live decode-list row.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "ui/ft8_screen.h"   // FT8_CALL_MAX_LEN

#ifdef __cplusplus
extern "C" {
#endif

// Small and touchscreen-list-sized - a real pileup rarely has more waiting
// callers than this at once, and the oldest entry is evicted to make room.
#define FT8_PILEUP_MAX 12

typedef struct {
    char    call[FT8_CALL_MAX_LEN];
    int16_t snr_db;
    int16_t freq_hz;        // their last-heard audio tone, Hz - for parity/reply targeting
    int64_t last_seen_utc;  // slot-start second they were last heard calling us
} ft8_pileup_entry_t;

void ft8_pileup_init(void);

// Upsert a station that addressed a decoded message to us this slot. Cheap;
// safe to call every decode cycle. Updates snr/freq/last_seen if already
// tracked, else inserts (evicting the oldest entry if the list is full).
void ft8_pileup_note_caller(const char *call, int16_t snr_db, int16_t freq_hz,
                            int64_t last_seen_utc);

// Remove a station - called once we commit to working them (from
// ft8_qso_start()/cqrun_answer(), whichever role), or when the operator
// manually dismisses a row. No-op if not present.
void ft8_pileup_remove(const char *call);

// Snapshot for the UI, newest-first. Returns the count written to out
// (<= max). Safe from any task.
int ft8_pileup_get_all(ft8_pileup_entry_t *out, int max);

// Current count - drives the FT8 screen's ADIF-log/Pileup button swap.
int ft8_pileup_count(void);

#ifdef __cplusplus
}
#endif
