// net_guard - decides whether a logbook upload may go out in the CLEAR.
//
// Written for Cloudlog/Wavelog (#171, Mark G4MEM), which unlike QRZ, eQSL and
// LoTW is SELF-HOSTED: the address comes from the operator, so this is the first
// time the firmware sends credentials somewhere it does not already know. Mark
// runs his on his own LAN with no certificate, and asked:
//
//   "Could it be feasible to permit http:// if the connection is to the same
//    subnet that the device is currently connected to (and also allow the
//    https:// connection if the server is remote i.e. on the internet)?"
//
// That is the right rule, and it is importantly NOT a "skip certificate
// verification" switch - the objection to such a switch is that it becomes the
// setting everybody uses, and it cannot tell a safe situation from a dangerous
// one. Same-subnet CAN: the packets never leave the wire the operator owns.
//
// ⛔ THE CHECK MUST RUN PER UPLOAD, AT CONNECT TIME - NEVER ONCE WHEN THE URL IS
// SAVED. Mark's own use case is why: he configures at home, then operates from a
// field site. A verdict cached at save time would let the device post his API key
// in the clear to whatever answers that hostname on a park or hotel LAN.
// Re-deciding every time fails safe - away from home the upload simply refuses
// and waits until he is back on his own network, which is exactly the behaviour
// he asked for.
//
// Portable: no ESP-IDF dependencies, so test/net_guard_harness.c links these
// very functions rather than a copy of them.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_SCHEME_UNKNOWN = 0,
    NET_SCHEME_HTTP,
    NET_SCHEME_HTTPS,
} net_scheme_t;

// Parse an absolute http(s) URL. `host_out` receives the bare host (no port, no
// brackets), `port_out` the explicit port or 0 when absent. Returns false if the
// URL is not http/https or has no host. Path is not returned - callers append
// their own endpoint.
bool net_url_parse(const char *url, net_scheme_t *scheme_out,
                   char *host_out, size_t host_sz, uint16_t *port_out);

// Parse a dotted-quad IPv4 literal. Returns false for anything else (including a
// hostname), so callers can tell "already an address" from "needs DNS".
bool net_ipv4_parse(const char *s, uint32_t *out);

// True when both addresses sit in the same IPv4 subnet.
// All three arguments must be in the SAME byte order as each other; which one
// does not matter, because the operation is a masked equality.
bool net_same_subnet(uint32_t a, uint32_t b, uint32_t netmask);

// The decision. True only when sending in the clear is acceptable: we know our
// own address and mask, we know the target's address, and the target is on our
// own subnet.
//
// Refuses on every "don't know" - an unset address or a zero netmask would
// otherwise match the whole internet, which is the one failure this file exists
// to prevent.
bool net_plaintext_allowed(uint32_t target_ip, uint32_t our_ip, uint32_t netmask);

#ifdef __cplusplus
}
#endif
