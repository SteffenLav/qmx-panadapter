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

// Convert an address held in NETWORK byte order (the layout lwip and esp_netif
// use: byte 0 of the uint32 is the FIRST octet) into the host-order convention
// net_ipv4_parse() produces, where 192.168.1.8 is 0xC0A80108.
//
// ⛔ THIS EXISTS BECAUSE OMITTING IT SHIPPED A BUG (Mark G4MEM, v1.8.7, within
// hours of release). cloudlog_upload.c took the device's own IP and netmask
// straight from esp_netif_get_ip_info() and compared them against a parsed
// literal. On this little-endian CPU 192.168.1.8 arrives from esp_netif as
// 0x0801A8C0 while net_ipv4_parse() returns 0xC0A80108 - byte-reversed - and the
// netmask is worse: 255.255.255.0 arrives as 0x00FFFFFF, so the masked equality
// compared the WRONG END of the address. Every LAN upload was refused with
// "is not on this network".
//
// ⚠ The unit tests did not catch it and could not have: the harness built both
// sides with its own host-order helper, so it compared a convention against
// itself. The defect was in the CALLER'S conversion, outside the function under
// test. That is what this wrapper is for - it drags the boundary inside the
// portable file where the harness can reach it.
uint32_t net_ipv4_from_network_order(uint32_t net_order);

// The decision. True only when sending in the clear is acceptable: we know our
// own address and mask, we know the target's address, and the target is on our
// own subnet.
//
// Refuses on every "don't know" - an unset address or a zero netmask would
// otherwise match the whole internet, which is the one failure this file exists
// to prevent.
bool net_plaintext_allowed(uint32_t target_ip, uint32_t our_ip, uint32_t netmask);

// The TX INTERLOCK's decision (T206): may a request that arrived from
// `peer_ip` key the transmitter? True only when we know our own address and
// mask and the peer sits on our own subnet, or the peer is this device itself.
//
// ⚠ IT IS NOT AUTHENTICATION AND MUST NEVER BE DESCRIBED AS ANY. It stops the
// internet. It does not stop a guest on the same WiFi, a phone on the same
// hotspot, or anyone who has the WiFi key - all of those are "on the subnet".
// Its whole value is that a request forwarded in from outside the LAN cannot
// put RF on the antenna.
//
// ⛔ SEPARATE FROM net_plaintext_allowed() ON PURPOSE, even though the masked
// comparison is the same today. One answers "may credentials go out in the
// clear", the other "may this request transmit"; they differ already (loopback
// is allowed here and meaningless there) and a future change to one must not
// silently move the other.
//
// Refuses on every "don't know", like net_plaintext_allowed(): a zero netmask
// would match every address on earth, and an unknown peer is exactly the case
// this exists to refuse.
bool net_tx_peer_allowed(uint32_t peer_ip, uint32_t our_ip, uint32_t netmask);

#ifdef __cplusplus
}
#endif
