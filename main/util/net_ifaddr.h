// net_ifaddr - this device's own IPv4 address and netmask, in HOST order.
//
// ⛔ ONE PLACE FOR THE BYTE-ORDER TRAP. esp_netif stores NETWORK order, and
// handing it straight to net_same_subnet() against a host-order address
// compares the WRONG END: on this little-endian CPU 192.168.1.8 arrives as
// 0x0801A8C0 while net_ipv4_parse() returns 0xC0A80108, and 255.255.255.0
// arrives as 0x00FFFFFF. That shipped in v1.8.7 and Mark G4MEM found it within
// hours - every LAN upload refused with "is not on this network".
//
// It then existed as an identical static own_ipv4() in net/ota_update.c and in
// adif/cloudlog_upload.c. Both are GONE, folded into this one function, when
// the TX interlock (T206) needed a third copy. net_guard.h's own note says the
// conversion belongs in one place so a host harness can reach it; two private
// copies of the caller side was the same mistake one layer up.
//
// NOT portable (it calls esp_netif), which is why it is not in net_guard.c -
// that file stays free of ESP-IDF so test/net_guard_harness.c can link it.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The STA interface's address and mask, converted to host order. False when
// there is no STA netif or it has no address yet - callers must treat that as
// "we do not know", never as a match.
bool net_own_ipv4(uint32_t *ip_out, uint32_t *mask_out);

#ifdef __cplusplus
}
#endif
