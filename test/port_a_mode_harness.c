// port_a_mode_harness.c - host-side check of PORT.A's ownership setting: the
// config-file round trip and the "no key means relay" default.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main -o test/port_a_mode_harness.exe test/port_a_mode_harness.c
//   ./test/port_a_mode_harness.exe
//
// WHY THIS EXISTS
//   port_a_mode is one u8 that decides whether GPIO53/54 are driven as relay
//   outputs or handed to a UART. Getting it wrong does not fail loudly: a unit
//   that boots into the wrong mode either never hears the GPS (silent, looks
//   like "no satellites") or drives pins that a GPS is transmitting into
//   (contention, which reads like a flaky cable). Neither shows up as a crash,
//   and both are only discoverable with the hardware in front of you.
//
//   Two halves are pure and cannot be got wrong by a device test that has to
//   be run to be seen:
//     * the INI encode/decode - "relay"/"unit_gps" is what an operator reads
//       and edits in a backup file by hand, so it must survive a round trip
//       and must not be able to name a third state;
//     * the absent-key rule - an NVS miss reads as 0, and 0 must mean RELAY,
//       or every existing unit would move a pin the first time it upgraded.
//
// WHAT IT CANNOT COVER
//   The on-device half of the round trip (config_io_export() printing the line,
//   config_io_import() applying it LAST through the sequencer) needs a running
//   firmware - bench steps I1-I3. This harness only proves that the value on
//   both sides of that file means what they both say it means.
//
// It includes settings.h rather than copying the rules: port_a_mode_str,
// port_a_mode_parse and port_a_mode_normalize are static inline there, so the
// harness links the same code the firmware runs, not a mirror of it.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "storage/settings.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

// Decode of every string an operator could plausibly type, and the assert that
// encode(decode(x)) is one of exactly the two documented values.
static void test_round_trip(void)
{
    static const struct { const char *in; uint8_t expect; } cases[] = {
        { "relay",     PORT_A_MODE_RELAY },
        { "unit_gps",  PORT_A_MODE_UNIT_GPS },
        { "RELAY",     PORT_A_MODE_RELAY },   // INI keys and values are
        { "UNIT_GPS",  PORT_A_MODE_UNIT_GPS },//  case-insensitive here
        { "Unit_GPS",  PORT_A_MODE_UNIT_GPS },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t got = port_a_mode_parse(cases[i].in);
        CHECK(got == cases[i].expect, "parse(%s) = %d, want %d",
              cases[i].in, got, cases[i].expect);
        // encode(decode(x)) is the canonical spelling of the same mode - the
        // case-insensitive inputs must land back on the file's own two words.
        CHECK(strcmp(port_a_mode_str(got), port_a_mode_str(cases[i].expect)) == 0,
              "encode(parse(%s)) = %s, want %s", cases[i].in,
              port_a_mode_str(got), port_a_mode_str(cases[i].expect));
    }

    // Both directions, on the enum values themselves: encode must be total.
    CHECK(strcmp(port_a_mode_str(PORT_A_MODE_RELAY), "relay") == 0,
          "RELAY encodes as %s", port_a_mode_str(PORT_A_MODE_RELAY));
    CHECK(strcmp(port_a_mode_str(PORT_A_MODE_UNIT_GPS), "unit_gps") == 0,
          "UNIT_GPS encodes as %s", port_a_mode_str(PORT_A_MODE_UNIT_GPS));
}

// Anything that is not a mode this build implements resolves to relay.
static void test_normalize(void)
{
    CHECK(port_a_mode_normalize(0) == PORT_A_MODE_RELAY,
          "absent key (NVS miss leaves 0) did not resolve to relay");
    CHECK(port_a_mode_normalize(PORT_A_MODE_RELAY) == PORT_A_MODE_RELAY,
          "relay did not survive normalize");
    CHECK(port_a_mode_normalize(PORT_A_MODE_UNIT_GPS) == PORT_A_MODE_UNIT_GPS,
          "unit_gps did not survive normalize");
    CHECK(port_a_mode_normalize(2) == PORT_A_MODE_RELAY,
          "reserved Grove slot 2 resolved to something drivable");
    CHECK(port_a_mode_normalize(200) == PORT_A_MODE_RELAY,
          "NVS noise resolved to something drivable");
    CHECK(port_a_mode_normalize(255) == PORT_A_MODE_RELAY,
          "0xFF resolved to something drivable");

    // The guarantee the upgrade path rests on: a unit that has never stored
    // the key must boot exactly as it did before the setting existed.
    uint8_t absent_key_value = 0;   // what nvs_get_u8 leaves behind on a miss
    CHECK(port_a_mode_normalize(absent_key_value) == PORT_A_MODE_RELAY,
          "an upgrade must not move a pin");
}

// Decode is total over the strings an edited file can carry: no input selects
// a third state, and the two documented ones survive being written back.
static void test_no_third_state(void)
{
    static const char *junk[] = { "", "gps", "uart", "grove", "1", "0",
                                  "true", "unit-gps", "unit gps" };
    for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        uint8_t got = port_a_mode_parse(junk[i]);
        CHECK(got == PORT_A_MODE_RELAY || got == PORT_A_MODE_UNIT_GPS,
              "parse(\"%s\") = %d, a mode no code path handles", junk[i], got);
    }
    CHECK(port_a_mode_parse(NULL) == PORT_A_MODE_RELAY,
          "a missing value must not be a mode");
    CHECK(port_a_mode_parse(port_a_mode_str(PORT_A_MODE_UNIT_GPS)) == PORT_A_MODE_UNIT_GPS,
          "unit_gps did not round trip through its own string");
    CHECK(port_a_mode_parse(port_a_mode_str(PORT_A_MODE_RELAY)) == PORT_A_MODE_RELAY,
          "relay did not round trip through its own string");
}

int main(void)
{
    printf("port_a_mode harness\n\n");
    test_round_trip();
    test_normalize();
    test_no_third_state();

    printf("\n%s\n", g_fail ? "FAILURES ABOVE" : "ALL PASS");
    return g_fail ? 1 : 0;
}
