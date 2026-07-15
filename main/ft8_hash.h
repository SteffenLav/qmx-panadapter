// ft8_hash.h - FT8 callsign hash table (nonstandard-callsign support).
//
// FT8 messages have no room for a nonstandard callsign (PJ4/K1ABC, DK7CVD/P,
// ...) in most message types, so the protocol transmits a HASH of it instead
// and relies on every receiver keeping a table of full calls it has recently
// heard (or sent) to resolve the hash back to text. Without a table, ft8_lib
// decodes every hashed call as "<...>" - which is exactly the "three dots"
// field report: a special-call station answers our CQ, the message shows as
// "<...> PJ4/K1ABC" (our own call arrives as a 12-bit hash!), and the QSO
// machine never recognises it as addressed to us.
//
// This module is the ftx_callsign_hash_interface_t implementation the whole
// firmware shares: every decode and encode that goes through ft8_hash_if()
// automatically populates the table (ft8_lib calls save_hash for each full
// callsign it packs or unpacks), and lookups resolve 22/12/10-bit hashes.
//
// NOTE: ft8_lib's bundled reference implementation (reference/test_main.c)
// probes the table starting at (hash * 23) % SIZE - but 12- and 10-bit hashes
// are the TOP bits of the 22-bit hash (n12 = n22 >> 10), so a 12-bit lookup
// starts its probe at a different index than the add did and misses. This
// implementation does a plain linear scan of all entries instead (256 tiny
// comparisons, dozens of times per 15 s slot - negligible) with round-robin
// eviction, which also survives a full table where open addressing without
// deletion would break down.
//
// Thread safety: decode runs concurrently on the decode task (core 1) AND the
// core-0 LDPC worker; encodes come from the LVGL and ft8 tasks. All table
// access is serialised by an internal mutex.

#pragma once

#include "ft8/message.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocate the table (PSRAM) and create the mutex. Idempotent; called at
// ft8_task startup. Until this has run, ft8_hash_if() returns NULL, which
// every ftx_message_* function accepts (hashed calls then just stay "<...>").
void ft8_hash_init(void);

// The shared hash interface, or NULL before ft8_hash_init(). Pass to every
// ftx_message_encode*/decode* call.
ftx_callsign_hash_interface_t *ft8_hash_if(void);

// Explicitly add a callsign (computes its own n22 - same base-38/multiply
// hash as ft8_lib's save_callsign). Used to seed OUR OWN call at FT8 entry:
// it never appears in anything we decode, but nonstandard-call answers carry
// it as a 12-bit hash that must resolve for the reply to be recognised.
void ft8_hash_seed(const char *callsign);

// Boot self-test (call from the FT8 self-test task): seeds a call, round-trips
// a nonstandard-call message through encode+decode, verifies the hash resolves.
// Returns true on pass. Logs details either way.
bool ft8_hash_selftest(void);

#ifdef __cplusplus
}
#endif
