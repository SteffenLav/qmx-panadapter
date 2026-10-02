// qmx_prompt_gate - decides WHEN the "Now turn on or reboot your QMX/+" prompt
// may appear.
//
// The prompt is not a cosmetic hint: obeying it at the wrong moment damages the
// session. Powering the QMX on while the Tab5 is still starting starves internal
// RAM for the rest of the session - measured on bench dev 2026-09-22: free_int
// 138 KB at ~7 s, 28 KB by 17.6 s, then flat at 15-20 KB. The web server stops
// answering and the socket table exhausts. It does not recover. The operator:
// "if i pc'ed the qmx too early ... it would wedge immediately".
//
// So the gate is the operator's own documented safe point: app_main has
// finished, AND WiFi has settled.
//
// ⛔ "SETTLED" MUST INCLUDE "NEVER GOING TO CONNECT". The first version of this
// gate (0b9412f, main/ui/ui.c) read
//
//     wifi_settled = !wifi_is_enabled() || wifi_is_connected();
//
// which is false FOREVER on a unit whose WiFi is enabled but never associates -
// no credentials yet, wrong password, AP out of range, operating portable. The
// prompt would then never appear at all, and the operator sees a blank wait with
// no instruction. That is the population least able to guess what to do, and it
// includes exactly the first-boot case. It never showed on the bench because the
// bench always associates (5 boots, 2026-10-02: 15.2-15.8 s, i.e. 5.1-6.2 s
// after "Init complete - main task idle").
//
// The bound is therefore a grace period after boot-complete, not a wait without
// end: QMX_PROMPT_WIFI_GRACE_MS. Generous rather than incremental - 30 s is ~5x
// the slowest settle observed and well past the 17.6 s point where the
// start-up RAM transient has flattened out, so nothing is invited early.
//
// ⛔ AND IT LATCHES. wifi_is_connected() goes false again whenever the link
// drops, which the hosted C6 does (Bryan N0LUF - see the watchdog in wifi.c).
// Without the latch a mid-session WiFi drop would RE-HIDE a prompt the operator
// was reading, and the boot-time hazard it guards against is long past by then.
//
// Portable: no ESP-IDF dependencies, so test/qmx_prompt_gate_harness.c links
// these very functions rather than a copy of them.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// How long after start-up finishes the gate waits for WiFi before giving up on
// it and showing the prompt anyway. See the header above for the measurement.
#define QMX_PROMPT_WIFI_GRACE_MS 30000u

typedef struct {
    bool     latched;    // safe once, safe for the rest of the session
    bool     boot_seen;  // boot_ms below is meaningful
    uint32_t boot_ms;    // when start-up finished
} qmx_prompt_gate_t;

// One tick of the gate. `now_ms` is any monotonic millisecond count; the
// arithmetic is wrap-safe. Returns true once it is safe to invite a power-on.
//
// The caller still has its own reasons to hide the prompt (the radio is already
// answering, a modal is open, the Reader is up) - this answers only the question
// of the MOMENT.
bool qmx_prompt_gate_tick(qmx_prompt_gate_t *g, bool boot_complete,
                          uint32_t now_ms, bool wifi_enabled, bool wifi_connected);

#ifdef __cplusplus
}
#endif
