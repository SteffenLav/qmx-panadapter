// cat_restore - WHAT to send to put the radio back where the band scan found it.
//
// The CAT band-list scan walks the QMX's own "Band config." menus, and
// qmx_term.c's header already records what that costs: "leave the menus, and the
// radio is on 160 m whatever band it started on." The scan restored nothing, so
// every CAT link-up quietly dumped the operator on 160 m in whatever mode the
// menus left (John W5JSS, 2026-10-01: WSPR page on 20 m, radio turning up on
// 1.837700 MHz in CW).
//
// ⛔ THIS BRANCH CANNOT BE REACHED ON THE BENCH. The restore only fires when the
// menus actually move the dial, and on the bench QMX they do not - the frequency
// and mode are identical before and after, so the save is proven and the restore
// has never run. Separating the decision from the CDC writes is what makes the
// decision testable at all; test/cat_restore_harness.c links this very function.
// The writes still need a radio, and cat.c carries a deliberate test entry
// point for that.
//
// The rule is "only act if it actually moved", so a radio the menus left alone
// is never written to. That matters: a needless FA write at link-up would move
// a dial the operator had just set.
//
// Portable: no ESP-IDF dependencies.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     send_freq;    // emit FA<freq>;
    uint32_t freq_hz;
    bool     send_mode;    // emit MD<digit>;
    char     mode_digit;
} cat_restore_plan_t;

// `pre_*` is what the radio reported before the scan, `now_*` what it reports
// after. A zero pre_freq means the pre-scan query never answered - nothing is
// restored then, because the alternative is writing a frequency we invented.
// A mode digit outside '1'..'9' is likewise "not known", never "restore it".
cat_restore_plan_t cat_restore_plan(uint32_t pre_freq, char pre_mode,
                                    uint32_t now_freq, char now_mode);

#ifdef __cplusplus
}
#endif
