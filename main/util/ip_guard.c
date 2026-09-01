#include "ip_guard.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Parsing and formatting
// ---------------------------------------------------------------------------

bool ip_guard_parse(const char *s, uint32_t *out)
{
    if (!s || !out) return false;

    uint32_t addr = 0;
    for (int octet = 0; octet < 4; octet++) {
        if (octet) {
            if (*s != '.') return false;
            s++;
        }
        if (*s < '0' || *s > '9') return false;

        // At most three digits, so "1921" cannot be read as 192 with a stray 1
        // left over for the next octet to trip on - it is refused here.
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3) return false;
            v = v * 10u + (unsigned)(*s - '0');
            s++;
        }
        if (v > 255u) return false;
        addr = (addr << 8) | v;
    }
    if (*s != '\0') return false;   // trailing anything is a typo, not an address

    *out = addr;
    return true;
}

void ip_guard_format(uint32_t addr, char buf[16])
{
    if (!buf) return;
    snprintf(buf, 16, "%u.%u.%u.%u",
             (unsigned)((addr >> 24) & 0xFFu), (unsigned)((addr >> 16) & 0xFFu),
             (unsigned)((addr >>  8) & 0xFFu), (unsigned)( addr        & 0xFFu));
}

bool ip_guard_mask_valid(uint32_t mask)
{
    if (mask == 0) return false;              // 0.0.0.0 is not a network
    uint32_t inv = ~mask;
    if (inv & (inv + 1u)) return false;       // the ones must be contiguous
    // /31 and /32 leave no usable host range, so on a LAN they are a typo -
    // 255.255.255.255 in the mask field being the common one.
    return inv >= 3u;
}

// A usable unicast host address. 0.0.0.0, loopback, multicast and the class-E
// block are all things a static LAN address can never legitimately be, and each
// of them is a plausible slip of the finger.
static bool usable_unicast(uint32_t a)
{
    if (a == 0u || a == 0xFFFFFFFFu) return false;
    uint32_t top = (a >> 24) & 0xFFu;
    if (top == 0u || top == 127u || top >= 224u) return false;
    return true;
}

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

static bool lease_known(const ip_guard_cfg_t *l)
{
    return l && l->ip[0] != '\0';
}

ip_guard_result_t ip_guard_check(const ip_guard_cfg_t *want,
                                 const ip_guard_cfg_t *lease,
                                 ip_guard_cfg_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!want || want->ip[0] == '\0') return IP_GUARD_DHCP;

    uint32_t ip;
    if (!ip_guard_parse(want->ip, &ip) || !usable_unicast(ip))
        return IP_GUARD_BAD_IP;

    // Mask: what the operator typed, else the lease's own, else /24. The lease
    // is a far better default than a guess - a /16 or /22 site network is
    // exactly where a hardcoded /24 quietly puts the gateway out of reach.
    uint32_t mask = 0;
    if (want->mask[0]) {
        if (!ip_guard_parse(want->mask, &mask)) return IP_GUARD_BAD_MASK;
    } else if (!(lease_known(lease) && lease->mask[0] &&
                 ip_guard_parse(lease->mask, &mask) && ip_guard_mask_valid(mask))) {
        mask = 0xFFFFFF00u;
    }
    if (!ip_guard_mask_valid(mask)) return IP_GUARD_BAD_MASK;

    uint32_t net = ip & mask;
    if (ip == net || ip == (net | ~mask)) return IP_GUARD_NOT_A_HOST;

    // THE CHECK THIS FILE EXISTS FOR. Judged with the LEASE's mask when it has
    // one: the question is whether the browser currently talking to the device
    // would still be able to reach it, and that is decided by the network the
    // device is on now, not by the mask being proposed.
    if (lease_known(lease)) {
        uint32_t lip, lmask;
        if (ip_guard_parse(lease->ip, &lip)) {
            if (!(lease->mask[0] && ip_guard_parse(lease->mask, &lmask) &&
                  ip_guard_mask_valid(lmask)))
                lmask = mask;
            if ((ip & lmask) != (lip & lmask)) return IP_GUARD_OFF_SUBNET;
        }
    }

    // Gateway: typed, else inherited from the lease. A gateway outside the
    // chosen subnet cannot be ARPed for, so nothing off-LAN would ever work -
    // SNTP, the feeds and every upload would fail with the device apparently up.
    uint32_t gw = 0;
    bool have_gw = false;
    if (want->gw[0]) {
        if (!ip_guard_parse(want->gw, &gw)) return IP_GUARD_BAD_GW;
        have_gw = true;
    } else if (lease_known(lease) && lease->gw[0] && ip_guard_parse(lease->gw, &gw)) {
        have_gw = (gw != 0u);
    }
    if (have_gw && (gw & mask) != net) return IP_GUARD_GW_OFF_SUBNET;

    // DNS: typed, else the lease's, else the gateway - which is what a home
    // router almost always is, and what wifi.c already falls back to.
    uint32_t dns = 0;
    bool have_dns = false;
    if (want->dns[0]) {
        if (!ip_guard_parse(want->dns, &dns)) return IP_GUARD_BAD_DNS;
        have_dns = true;
    } else if (lease_known(lease) && lease->dns[0] && ip_guard_parse(lease->dns, &dns)) {
        have_dns = (dns != 0u);
    } else if (have_gw) {
        dns = gw;
        have_dns = true;
    }

    if (out) {
        ip_guard_format(ip, out->ip);
        ip_guard_format(mask, out->mask);
        if (have_gw)  ip_guard_format(gw,  out->gw);
        if (have_dns) ip_guard_format(dns, out->dns);
    }
    return IP_GUARD_OK;
}

void ip_guard_explain(ip_guard_result_t r,
                      const ip_guard_cfg_t *want,
                      const ip_guard_cfg_t *lease,
                      char *buf, size_t n)
{
    if (!buf || n == 0) return;
    const char *ip = (want && want->ip[0]) ? want->ip : "(blank)";

    switch (r) {
    case IP_GUARD_OK:
        snprintf(buf, n, "Static address accepted.");
        break;
    case IP_GUARD_DHCP:
        snprintf(buf, n, "Address left blank - the Tab5 will use DHCP.");
        break;
    case IP_GUARD_BAD_IP:
        snprintf(buf, n, "'%s' is not a usable address. Type four numbers "
                         "0-255 separated by dots, for example 192.168.1.50.", ip);
        break;
    case IP_GUARD_BAD_MASK:
        snprintf(buf, n, "'%s' is not a usable subnet mask. Nearly every home "
                         "network uses 255.255.255.0.",
                 (want && want->mask[0]) ? want->mask : "(blank)");
        break;
    case IP_GUARD_BAD_GW:
        snprintf(buf, n, "'%s' is not a valid gateway address.",
                 (want && want->gw[0]) ? want->gw : "(blank)");
        break;
    case IP_GUARD_BAD_DNS:
        snprintf(buf, n, "'%s' is not a valid DNS server address.",
                 (want && want->dns[0]) ? want->dns : "(blank)");
        break;
    case IP_GUARD_NOT_A_HOST:
        snprintf(buf, n, "%s is this network's own address or its broadcast "
                         "address, so no device can use it. Pick a number in "
                         "between.", ip);
        break;
    case IP_GUARD_OFF_SUBNET:
        snprintf(buf, n, "%s is not on this network. The Tab5 is on %s right "
                         "now, so setting %s would make it unreachable from "
                         "this browser, and the only way back would be a "
                         "factory reset.",
                 ip, (lease && lease->ip[0]) ? lease->ip : "another network", ip);
        break;
    case IP_GUARD_GW_OFF_SUBNET: {
        // Name the gateway even when it was inherited rather than typed -
        // "(inherited)" tells the operator nothing they can act on, and an
        // inherited gateway is exactly how this refusal usually arises.
        const char *gw = (want && want->gw[0]) ? want->gw
                       : (lease && lease->gw[0]) ? lease->gw : "(none)";
        snprintf(buf, n, "The gateway %s is not reachable from %s with that "
                         "subnet mask, so the Tab5 would have no way off the "
                         "local network.", gw, ip);
        break;
    }
    default:
        snprintf(buf, n, "The static address was refused.");
        break;
    }
    buf[n - 1] = '\0';
}
