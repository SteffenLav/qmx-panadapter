// Message hygiene for anything that is about to key the radio.
//
// Portable on purpose (no ESP deps) so test/ft8_cq_encode_harness.c links these
// exact functions rather than a copy of them.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Trim leading/trailing whitespace AND collapse every interior run to a single
// space, in place.
//
// The collapse is the load-bearing half. FT8's 77-bit encoder tokenises on
// spaces, so one extra space does not produce a slightly untidy message - it
// produces a DIFFERENT message. "CQ  POTA WB0LQW" (two spaces) encodes cleanly
// as a signal report to a hashed callsign, keys the radio for the full 12.6 s,
// and lands at the far end as "CQ  <...> +00". Field-hit: Don WB0LQW,
// 2026-08-14, three presets, reproduced exactly by the harness.
void ft8_msg_normalize(char *s);

// True if `decoded` (what a receiving station would actually see, obtained by
// decoding the payload we are about to transmit) is a faithful enough rendering
// of `typed` to be worth keying the radio for. `my_call` may be NULL/empty.
//
// Deliberately NOT a byte comparison. The 77-bit protocol legitimately drops
// tokens it has no room for - "CQ POTA PJ4/K1ABC" transmits as "CQ PJ4/K1ABC",
// and refusing to key that would be worse than the message it is guarding
// against. What must survive is MEANING: a CQ must still be a CQ, and it must
// still carry the operator's callsign. Don's message failed both.
bool ft8_msg_roundtrip_ok(const char *typed, const char *decoded, const char *my_call);

#ifdef __cplusplus
}
#endif
