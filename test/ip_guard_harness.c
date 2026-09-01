/* Host test for ip_guard - the rule deciding whether a STATIC IP configuration
 * is safe to store (#307, Randy N4OPI).
 *
 * Build (from the repo root):
 *   gcc -I main/util -o ip_guard_harness test/ip_guard_harness.c \
 *       main/util/ip_guard.c && ./ip_guard_harness
 *
 * Why this exists: the fault being guarded against does not crash anything, does
 * not appear on the screen, and is not visible in any log the operator can
 * reach - because reaching it needs the web UI, which is the exact thing a wrong
 * static address takes away. The recovery is a factory reset, which costs the
 * WiFi password, the callsign, the memory channels and the LoTW private key. So
 * the check gets exactly one chance to be right, at save time, and it has to be
 * right without hardware.
 *
 * It links the REAL functions, not a copy. A harness that mirrors the code under
 * test only ever proves the mirror.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ip_guard.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

/* A compound literal, so its address can be taken inline. A function returning
 * a struct by value cannot: the result is not an lvalue, so taking the address
 * of one does not compile. */
#define CFG(i_, m_, g_, d_) (&(ip_guard_cfg_t){ .ip = i_, .mask = m_, .gw = g_, .dns = d_ })

static const char *rname(ip_guard_result_t r)
{
    switch (r) {
    case IP_GUARD_OK:            return "OK";
    case IP_GUARD_DHCP:          return "DHCP";
    case IP_GUARD_BAD_IP:        return "BAD_IP";
    case IP_GUARD_BAD_MASK:      return "BAD_MASK";
    case IP_GUARD_BAD_GW:        return "BAD_GW";
    case IP_GUARD_BAD_DNS:       return "BAD_DNS";
    case IP_GUARD_NOT_A_HOST:    return "NOT_A_HOST";
    case IP_GUARD_OFF_SUBNET:    return "OFF_SUBNET";
    case IP_GUARD_GW_OFF_SUBNET: return "GW_OFF_SUBNET";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
static void test_parse(void)
{
    printf("parse: strict dotted quad\n");
    uint32_t v = 0;

    CHECK(ip_guard_parse("0.0.0.0", &v) && v == 0u, "0.0.0.0");
    CHECK(ip_guard_parse("192.168.1.50", &v) && v == 0xC0A80132u, "got %08x", v);
    CHECK(ip_guard_parse("255.255.255.255", &v) && v == 0xFFFFFFFFu, "all ones");
    CHECK(ip_guard_parse("10.0.0.1", &v) && v == 0x0A000001u, "10.0.0.1");

    /* Every one of these is a real thing an operator types on a touchscreen. */
    CHECK(!ip_guard_parse("192.168.1", &v),      "three octets must be refused");
    CHECK(!ip_guard_parse("192.168.1.", &v),     "trailing dot");
    CHECK(!ip_guard_parse(".192.168.1.1", &v),   "leading dot");
    CHECK(!ip_guard_parse("192.168.1.256", &v),  "256 is not an octet");
    CHECK(!ip_guard_parse("192.168.1.1.1", &v),  "five octets");
    CHECK(!ip_guard_parse("192.168.1.5x", &v),   "trailing character");
    CHECK(!ip_guard_parse("192.168.1.5 ", &v),   "trailing space");
    CHECK(!ip_guard_parse(" 192.168.1.5", &v),   "leading space");
    CHECK(!ip_guard_parse("192.168..5", &v),     "empty octet");
    CHECK(!ip_guard_parse("192.168.1.1921", &v), "four digits in an octet");
    CHECK(!ip_guard_parse("", &v),               "empty string");
    CHECK(!ip_guard_parse("dhcp", &v),           "a word");
    CHECK(!ip_guard_parse(NULL, &v),             "NULL");
    CHECK(!ip_guard_parse("-1.0.0.1", &v),       "negative");

    /* An octet is at most THREE digits, and that cap is not merely cosmetic:
     * without it a long enough run of digits overflows `unsigned` and wraps
     * back into 0-255, so a nonsense address would be accepted as a real one.
     * 4294967296 is 2^32, which wraps to exactly 0.
     * (Found by mutation testing - loosening the cap to four digits survived
     * every other check in this file, because the 0-255 range test catches
     * "1921" on its own and nothing here reached the cap itself.) */
    CHECK(!ip_guard_parse("192.168.1.0050", &v),
          "four digits must be refused even when the value is in range");
    CHECK(!ip_guard_parse("192.168.1.4294967296", &v),
          "an octet that overflows to 0 must not be accepted");
    CHECK(!ip_guard_parse("192.168.1.4294967297", &v),
          "an octet that overflows to 1 must not be accepted");

    /* Leading zeros are accepted as decimal. inet_aton would read 010 as OCTAL
     * 8; we do not, and the difference must be deliberate rather than found in
     * the field. */
    CHECK(ip_guard_parse("192.168.001.010", &v) && v == 0xC0A8010Au,
          "leading zeros are decimal, got %08x", v);

    char buf[16];
    ip_guard_format(0xC0A80132u, buf);
    CHECK(strcmp(buf, "192.168.1.50") == 0, "format gave '%s'", buf);
    ip_guard_format(0u, buf);
    CHECK(strcmp(buf, "0.0.0.0") == 0, "format zero gave '%s'", buf);
    ip_guard_format(0xFFFFFFFFu, buf);
    CHECK(strcmp(buf, "255.255.255.255") == 0, "format ones gave '%s'", buf);
}

static void test_mask(void)
{
    printf("mask: contiguous, and leaves a host range\n");
    CHECK(ip_guard_mask_valid(0xFFFFFF00u), "/24");
    CHECK(ip_guard_mask_valid(0xFFFF0000u), "/16");
    CHECK(ip_guard_mask_valid(0xFFFFFC00u), "/22 - a real site network");
    CHECK(ip_guard_mask_valid(0xFFFFFFFCu), "/30 is the smallest usable");
    CHECK(ip_guard_mask_valid(0x80000000u), "/1");

    CHECK(!ip_guard_mask_valid(0u),          "0.0.0.0");
    CHECK(!ip_guard_mask_valid(0xFFFFFFFFu), "/32 - the common typo");
    CHECK(!ip_guard_mask_valid(0xFFFFFFFEu), "/31 has no host range");
    CHECK(!ip_guard_mask_valid(0xFF00FF00u), "non-contiguous");
    CHECK(!ip_guard_mask_valid(0x00FFFFFFu), "reversed");
    CHECK(!ip_guard_mask_valid(0xFFFFFF01u), "trailing stray bit");
}

/* The whole point of the file. */
static void test_off_subnet(void)
{
    printf("THE fatal case: a well-formed address on the wrong network\n");
    ip_guard_cfg_t lease = *CFG("192.168.4.23", "255.255.255.0",
                               "192.168.4.1", "192.168.4.1");
    ip_guard_cfg_t out;

    /* Randy's exact trap: a perfectly valid address, on the subnet everyone
     * assumes a home LAN uses, while this LAN is 192.168.4.x. */
    ip_guard_result_t r = ip_guard_check(CFG("192.168.1.50", "", "", ""),
                                         &lease, &out);
    CHECK(r == IP_GUARD_OFF_SUBNET, "192.168.1.50 on a 192.168.4.0/24 lease "
                                    "must be refused, got %s", rname(r));

    /* And the message has to name the numbers, or it is not actionable. */
    char msg[256];
    ip_guard_explain(r, CFG("192.168.1.50", "", "", ""), &lease, msg, sizeof(msg));
    CHECK(strstr(msg, "192.168.1.50") != NULL, "message names the address: %s", msg);
    CHECK(strstr(msg, "192.168.4.23") != NULL, "message names the lease: %s", msg);

    /* Same address, same network - the ordinary case must still pass. */
    r = ip_guard_check(CFG("192.168.4.50", "", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_OK, "same-subnet address must pass, got %s", rname(r));

    /* A wider lease mask legitimately makes a "different" third octet the same
     * network. The check must follow the LEASE's mask, not assume /24. */
    ip_guard_cfg_t wide = *CFG("10.20.4.23", "255.255.0.0", "10.20.0.1", "");
    r = ip_guard_check(CFG("10.20.9.50", "", "", ""), &wide, &out);
    CHECK(r == IP_GUARD_OK, "10.20.9.50 is on 10.20.0.0/16, got %s", rname(r));
    CHECK(strcmp(out.mask, "255.255.0.0") == 0,
          "the /16 must be inherited, not overwritten with /24: '%s'", out.mask);
    r = ip_guard_check(CFG("10.21.9.50", "", "", ""), &wide, &out);
    CHECK(r == IP_GUARD_OFF_SUBNET, "10.21.x is off a /16 at 10.20, got %s", rname(r));

    /* A narrow proposed mask must not be able to talk its way past the lease:
     * 192.168.4.50/30 spans .48-.51, which is still inside the lease's /24, so
     * the subnet check has nothing to object to and a gateway inside that /30
     * is accepted. */
    r = ip_guard_check(CFG("192.168.4.50", "255.255.255.252", "192.168.4.49", ""),
                       &lease, &out);
    CHECK(r == IP_GUARD_OK, "narrow mask, same lease network, got %s", rname(r));

    /* Same narrow mask with the gateway left BLANK inherits the lease's
     * 192.168.4.1 - which a /30 at .50 genuinely cannot reach. Refusing is
     * correct: storing it would give a device that joins the network, answers
     * on the LAN, and can reach nothing beyond it. */
    r = ip_guard_check(CFG("192.168.4.50", "255.255.255.252", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_GW_OFF_SUBNET,
          "an inherited gateway outside a narrow mask must be refused, got %s",
          rname(r));
    ip_guard_explain(r, CFG("192.168.4.50", "255.255.255.252", "", ""),
                     &lease, msg, sizeof(msg));
    CHECK(strstr(msg, "192.168.4.1") != NULL,
          "the message must name the INHERITED gateway, not say '(inherited)': %s",
          msg);

    /* With NO lease to judge against, the subnet check cannot run and must not
     * pretend to - configuring in advance for another network stays possible. */
    r = ip_guard_check(CFG("192.168.1.50", "", "", ""), NULL, &out);
    CHECK(r == IP_GUARD_OK, "no lease means syntax only, got %s", rname(r));
    r = ip_guard_check(CFG("192.168.1.50", "", "", ""), CFG("", "", "", ""), &out);
    CHECK(r == IP_GUARD_OK, "blank lease means syntax only, got %s", rname(r));
}

static void test_fill_from_lease(void)
{
    printf("fill-in: the lease answers, not a guessed /24\n");
    ip_guard_cfg_t lease = *CFG("192.168.4.23", "255.255.255.0",
                               "192.168.4.1", "192.168.4.53");
    ip_guard_cfg_t out;

    ip_guard_result_t r = ip_guard_check(CFG("192.168.4.50", "", "", ""),
                                         &lease, &out);
    CHECK(r == IP_GUARD_OK, "got %s", rname(r));
    CHECK(strcmp(out.ip,   "192.168.4.50")  == 0, "ip '%s'",   out.ip);
    CHECK(strcmp(out.mask, "255.255.255.0") == 0, "mask '%s'", out.mask);
    CHECK(strcmp(out.gw,   "192.168.4.1")   == 0, "gw '%s'",   out.gw);
    CHECK(strcmp(out.dns,  "192.168.4.53")  == 0, "dns '%s'",  out.dns);

    /* No DNS anywhere: fall back to the gateway, which is what wifi.c does. */
    ip_guard_cfg_t nodns = *CFG("192.168.4.23", "255.255.255.0", "192.168.4.1", "");
    r = ip_guard_check(CFG("192.168.4.50", "", "", ""), &nodns, &out);
    CHECK(r == IP_GUARD_OK, "got %s", rname(r));
    CHECK(strcmp(out.dns, "192.168.4.1") == 0, "dns falls back to gw, got '%s'",
          out.dns);

    /* What the operator typed always wins over the lease. */
    r = ip_guard_check(CFG("192.168.4.50", "255.255.255.0",
                            "192.168.4.9", "8.8.8.8"), &lease, &out);
    CHECK(r == IP_GUARD_OK, "got %s", rname(r));
    CHECK(strcmp(out.gw,  "192.168.4.9") == 0, "typed gw wins: '%s'",  out.gw);
    CHECK(strcmp(out.dns, "8.8.8.8")     == 0, "typed dns wins: '%s'", out.dns);

    /* An off-LAN DNS is legitimate; an off-LAN GATEWAY is not. */
    r = ip_guard_check(CFG("192.168.4.50", "", "192.168.9.1", ""), &lease, &out);
    CHECK(r == IP_GUARD_GW_OFF_SUBNET, "unreachable gateway, got %s", rname(r));

    /* No lease at all: the /24 guess is the documented fallback, and gw/dns
     * stay blank rather than being invented. */
    r = ip_guard_check(CFG("192.168.1.50", "", "", ""), NULL, &out);
    CHECK(r == IP_GUARD_OK, "got %s", rname(r));
    CHECK(strcmp(out.mask, "255.255.255.0") == 0, "mask '%s'", out.mask);
    CHECK(out.gw[0] == 0 && out.dns[0] == 0, "gw/dns must stay blank");
}

static void test_syntax_and_hosts(void)
{
    printf("syntax and host addresses\n");
    ip_guard_cfg_t out;
    ip_guard_cfg_t lease = *CFG("192.168.4.23", "255.255.255.0", "192.168.4.1", "");

    ip_guard_result_t r = ip_guard_check(CFG("", "", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_DHCP, "blank address means DHCP, got %s", rname(r));
    CHECK(out.ip[0] == 0 && out.mask[0] == 0 && out.gw[0] == 0 && out.dns[0] == 0,
          "DHCP must clear the whole set");
    r = ip_guard_check(NULL, &lease, &out);
    CHECK(r == IP_GUARD_DHCP, "NULL means DHCP, got %s", rname(r));

    r = ip_guard_check(CFG("192.168.4", "", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_BAD_IP, "half an address, got %s", rname(r));
    r = ip_guard_check(CFG("127.0.0.1", "", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_BAD_IP, "loopback, got %s", rname(r));
    r = ip_guard_check(CFG("224.0.0.1", "", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_BAD_IP, "multicast, got %s", rname(r));
    r = ip_guard_check(CFG("0.0.0.0", "", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_BAD_IP, "the any address, got %s", rname(r));

    r = ip_guard_check(CFG("192.168.4.50", "255.255.0.255", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_BAD_MASK, "non-contiguous mask, got %s", rname(r));
    r = ip_guard_check(CFG("192.168.4.50", "255.255.255.255", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_BAD_MASK, "/32, got %s", rname(r));

    /* The two addresses in every subnet that no host may take. */
    r = ip_guard_check(CFG("192.168.4.0", "255.255.255.0", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_NOT_A_HOST, "network address, got %s", rname(r));
    r = ip_guard_check(CFG("192.168.4.255", "255.255.255.0", "", ""), &lease, &out);
    CHECK(r == IP_GUARD_NOT_A_HOST, "broadcast address, got %s", rname(r));

    r = ip_guard_check(CFG("192.168.4.50", "", "not.an.address", ""), &lease, &out);
    CHECK(r == IP_GUARD_BAD_GW, "bad gateway text, got %s", rname(r));
    r = ip_guard_check(CFG("192.168.4.50", "", "", "8.8.8"), &lease, &out);
    CHECK(r == IP_GUARD_BAD_DNS, "bad DNS text, got %s", rname(r));

    /* Every result must produce a NUL-terminated sentence, including into a
     * buffer far too small for it. */
    char tiny[8];
    for (int i = IP_GUARD_OK; i <= IP_GUARD_GW_OFF_SUBNET; i++) {
        memset(tiny, 'x', sizeof(tiny));
        ip_guard_explain((ip_guard_result_t)i, CFG("192.168.1.50", "", "", ""),
                         &lease, tiny, sizeof(tiny));
        CHECK(memchr(tiny, 0, sizeof(tiny)) != NULL,
              "explain(%s) did not terminate in a short buffer", rname(i));
    }
    ip_guard_explain(IP_GUARD_OFF_SUBNET, NULL, NULL, tiny, sizeof(tiny));
    CHECK(memchr(tiny, 0, sizeof(tiny)) != NULL, "explain(NULL,NULL) terminated");
}

int main(void)
{
    printf("ip_guard harness\n\n");
    test_parse();
    test_mask();
    test_off_subnet();
    test_fill_from_lease();
    test_syntax_and_hosts();

    printf("\n%s\n", g_fail ? "FAILURES ABOVE" : "all checks passed");
    return g_fail ? 1 : 0;
}
