// ip_guard - decides whether a STATIC IP configuration is safe to store.
//
// Static IP shipped in v1.10.5 (#307, Randy N4OPI) with no validation at all,
// and the failure mode is the worst shape a setting can have: a PERFECTLY
// WELL-FORMED address on the WRONG SUBNET. The device comes up, joins the
// access point, logs "static IP applied", and is unreachable from every browser
// on the network. The only place to undo it is the web UI, which is exactly
// what has just been made unreachable - so the recovery is a factory reset,
// which takes the WiFi password, the callsign, the memory channels and the LoTW
// private key with it.
//
// ⭐ THE DEVICE ALREADY KNOWS THE RIGHT ANSWER: ITS OWN DHCP LEASE.
// The operator configures this from a browser that is, by definition, on the
// network the Tab5 must stay on. So the live lease answers both questions:
//   - what a blank mask/gateway/DNS should be (NOT a guessed /24), and
//   - whether the address the operator typed is on this network at all.
// That second check is the one that catches the fatal mistake, and it is worth
// more than every syntax check here put together.
//
// ⛔ THIS RUNS AT SAVE TIME, NOT AT CONNECT TIME - the opposite of net_guard.c,
// and deliberately. net_guard re-decides per upload because a cached verdict
// would leak credentials onto a field LAN. Here the question is "is the
// operator about to lock themselves out of the device they are typing into",
// which can ONLY be asked at the moment they type it: at connect time there is
// no lease to compare against, because a static configuration is precisely one
// that never asks for one.
//
// Configuring for a DIFFERENT network in advance is legitimate, so the caller
// may override a refusal deliberately. The guard's job is to make the fatal
// case require a decision, not to make it impossible.
//
// Portable: no ESP-IDF dependencies, so test/ip_guard_harness.c links these
// very functions rather than a copy of them.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The four dotted-quad strings, stored exactly as qmx_settings_t holds them.
// An empty ip means DHCP, which is the default and the only state an upgraded
// unit can be in.
typedef struct {
    char ip[16];
    char mask[16];
    char gw[16];
    char dns[16];
} ip_guard_cfg_t;

typedef enum {
    IP_GUARD_OK = 0,          // accepted; `out` holds the configuration to store
    IP_GUARD_DHCP,            // empty address - go back to DHCP; `out` is blank
    IP_GUARD_BAD_IP,          // not a usable unicast address
    IP_GUARD_BAD_MASK,        // not a contiguous /1../30 mask
    IP_GUARD_BAD_GW,          // gateway is not a dotted quad
    IP_GUARD_BAD_DNS,         // DNS server is not a dotted quad
    IP_GUARD_NOT_A_HOST,      // the subnet's own network or broadcast address
    IP_GUARD_OFF_SUBNET,      // ⛔ the fatal one: not on the lease's network
    IP_GUARD_GW_OFF_SUBNET,   // gateway unreachable from the chosen address
} ip_guard_result_t;

// Strict dotted quad. Rejects anything with a missing octet, an octet above
// 255, or a trailing character - "192.168.1" and "192.168.1.5x" are both
// refused rather than silently read as far as they parse.
bool ip_guard_parse(const char *s, uint32_t *out);

// Writes a dotted quad into a caller's char[16]. Always NUL-terminated.
void ip_guard_format(uint32_t addr, char buf[16]);

// A netmask must be a contiguous run of ones, and must leave a usable host
// range: /31 and /32 have none, and 0.0.0.0 is not a network.
bool ip_guard_mask_valid(uint32_t mask);

// Judge `want` against the live `lease` (pass NULL, or a cfg with an empty ip,
// when no lease is known - the subnet check is then skipped and only syntax is
// enforced). On IP_GUARD_OK, `out` receives the configuration to STORE, with
// blank fields filled in from the lease. `out` may alias neither input.
ip_guard_result_t ip_guard_check(const ip_guard_cfg_t *want,
                                 const ip_guard_cfg_t *lease,
                                 ip_guard_cfg_t *out);

// A sentence for the operator, naming the actual numbers involved. Always
// NUL-terminates. `lease` may be NULL.
void ip_guard_explain(ip_guard_result_t r,
                      const ip_guard_cfg_t *want,
                      const ip_guard_cfg_t *lease,
                      char *buf, size_t n);

#ifdef __cplusplus
}
#endif
