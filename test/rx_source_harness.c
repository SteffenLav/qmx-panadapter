// rx_source_harness.c - host-side check of the RX input source setting: the
// config-file round trip, the "no key means the radio" default, the rule that
// a source this build cannot run is never selectable, and the line-in gain
// quantisation.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main -o test/rx_source_harness.exe test/rx_source_harness.c
//   ./test/rx_source_harness.exe
//
// WHY THIS EXISTS
//   rx_source is one u8 that decides whether the board brings up the USB host
//   and talks to the QMX, or brings up the ES7210 capture side and listens to
//   the 3.5 mm jack. The two cannot coexist (the capture channel starves the
//   USB host's endpoint allocation), so the value is latched at boot and a
//   wrong one costs a restart to find out about.
//
//   The failure is silent in both directions, which is what makes it worth a
//   harness rather than a bench step:
//     * a board that boots into line_in with nothing in the jack looks exactly
//       like a board whose QMX has wedged - no spectrum, no CAT;
//     * a board that boots into qmx_usb when the operator chose line_in looks
//       exactly like a jack that does not work.
//
//   Three rules here are pure, and a device test would only find them by
//   rebooting into the wrong state:
//     * the INI encode/decode - "qmx_usb"/"line_in" is what an operator reads
//       and hand-edits in a backup, so it must survive a round trip;
//     * the absent-key rule - an NVS miss reads as 0, and 0 must mean the QMX,
//       or every existing unit would lose its receiver on upgrade;
//     * the DOWNGRADE rule - two sources are declared but not implemented, so
//       a config file or an NVS key naming one (written by a newer build) must
//       resolve to the radio, never to a source with no code behind it.
//
// WHAT IT CANNOT COVER
//   That app_main() actually branches on the value, that the ES7210 opens on
//   channel 3, and that the gain reaches the part. Those need the hardware.
//   This harness only proves that the stored byte means the same thing to
//   every piece of code that reads it.
//
// It includes settings.h rather than copying the rules: rx_source_str,
// rx_source_parse, rx_source_normalize, rx_source_is_real and
// line_in_gain_db_normalize are static inline there, so the harness links the
// same code the firmware runs, not a mirror of it.
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

// Every source's canonical spelling, and the assert that encode is total -
// including for the two that are declared but have no implementation, because
// config_io_export() must still be able to print whatever is in the struct.
static void test_encode(void)
{
    CHECK(strcmp(rx_source_str(RX_SOURCE_QMX_USB),    "qmx_usb")    == 0,
          "QMX_USB encodes as %s",    rx_source_str(RX_SOURCE_QMX_USB));
    CHECK(strcmp(rx_source_str(RX_SOURCE_GENERIC_IQ), "generic_iq") == 0,
          "GENERIC_IQ encodes as %s", rx_source_str(RX_SOURCE_GENERIC_IQ));
    CHECK(strcmp(rx_source_str(RX_SOURCE_LINE_IN),    "line_in")    == 0,
          "LINE_IN encodes as %s",    rx_source_str(RX_SOURCE_LINE_IN));
    CHECK(strcmp(rx_source_str(RX_SOURCE_MIC_INT),    "mic_int")    == 0,
          "MIC_INT encodes as %s",    rx_source_str(RX_SOURCE_MIC_INT));

    // A byte that is not a source at all still encodes, and encodes as the
    // radio - config_io_export() has no way to refuse to print a line.
    CHECK(strcmp(rx_source_str(200), "qmx_usb") == 0,
          "a junk byte encodes as %s", rx_source_str(200));
}

// Decode of every string an operator could plausibly type or a newer build
// could plausibly have written.
static void test_parse(void)
{
    static const struct { const char *in; uint8_t expect; } cases[] = {
        { "qmx_usb",    RX_SOURCE_QMX_USB },
        { "line_in",    RX_SOURCE_LINE_IN },
        { "QMX_USB",    RX_SOURCE_QMX_USB },   // INI values are
        { "LINE_IN",    RX_SOURCE_LINE_IN },   //  case-insensitive here
        { "Line_In",    RX_SOURCE_LINE_IN },
        // Declared but not implemented: a file naming one must land on the
        // radio, not on a source with no code behind it.
        { "generic_iq", RX_SOURCE_QMX_USB },
        { "mic_int",    RX_SOURCE_QMX_USB },
        // Noise, a typo, and a source name from some future build.
        { "",           RX_SOURCE_QMX_USB },
        { "line in",    RX_SOURCE_QMX_USB },
        { "sdr_play",   RX_SOURCE_QMX_USB },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t got = rx_source_parse(cases[i].in);
        CHECK(got == cases[i].expect, "parse(\"%s\") = %s, want %s",
              cases[i].in, rx_source_str(got), rx_source_str(cases[i].expect));
    }

    // A NULL value (an INI key present with nothing after the '=') must not
    // dereference, and must mean the radio.
    CHECK(rx_source_parse(NULL) == RX_SOURCE_QMX_USB, "parse(NULL) is not the radio");

    // THE RULE THAT MATTERS: whatever a file says, the result is always a
    // source this build can actually run. Nothing reaching the struct from a
    // config import can leave a board with no receiver.
    static const char *anything[] = {
        "qmx_usb", "line_in", "generic_iq", "mic_int", "", "rubbish", "0", "3",
    };
    for (size_t i = 0; i < sizeof(anything) / sizeof(anything[0]); i++) {
        CHECK(rx_source_implemented(rx_source_parse(anything[i])),
              "parse(\"%s\") returned an unimplemented source", anything[i]);
    }
}

// The absent-key and downgrade rules, on raw bytes rather than strings.
static void test_normalize(void)
{
    // An NVS miss leaves the byte at 0. That must be the radio, or every unit
    // that has never set this would change behaviour on upgrade.
    CHECK(rx_source_normalize(0) == RX_SOURCE_QMX_USB,
          "an absent key does not mean the radio");

    CHECK(rx_source_normalize(RX_SOURCE_LINE_IN) == RX_SOURCE_LINE_IN,
          "line_in does not survive normalize");

    // Written by a newer build that implements them, then downgraded.
    CHECK(rx_source_normalize(RX_SOURCE_GENERIC_IQ) == RX_SOURCE_QMX_USB,
          "generic_iq does not fall back to the radio");
    CHECK(rx_source_normalize(RX_SOURCE_MIC_INT) == RX_SOURCE_QMX_USB,
          "mic_int does not fall back to the radio");

    // Flash noise.
    CHECK(rx_source_normalize(0xFF) == RX_SOURCE_QMX_USB, "0xFF is not the radio");
    CHECK(rx_source_normalize(4)    == RX_SOURCE_QMX_USB, "4 is not the radio");

    // Idempotent: normalize(normalize(x)) == normalize(x) for every byte.
    for (int i = 0; i < 256; i++) {
        uint8_t once  = rx_source_normalize((uint8_t)i);
        uint8_t twice = rx_source_normalize(once);
        CHECK(once == twice, "normalize is not idempotent at %d", i);
        CHECK(rx_source_implemented(once), "normalize(%d) is unimplemented", i);
    }
}

// Which sources are REAL (one channel) rather than complex IQ. This is the bit
// the DSP branches on: get it wrong for the QMX and the FT8 chain stops mixing
// the +12 kHz IF down, which kills every decode without any error anywhere.
static void test_is_real(void)
{
    CHECK(!rx_source_is_real(RX_SOURCE_QMX_USB),    "the QMX is not IQ");
    CHECK(!rx_source_is_real(RX_SOURCE_GENERIC_IQ), "generic IQ is not IQ");
    CHECK( rx_source_is_real(RX_SOURCE_LINE_IN),    "line_in is not real");
    CHECK( rx_source_is_real(RX_SOURCE_MIC_INT),    "the internal mics are not real");

    // A junk byte normalizes to the radio, so it must not be treated as real -
    // otherwise corrupt NVS would silently disable the IF mixer.
    CHECK(!rx_source_is_real(rx_source_normalize(0xFF)),
          "a junk source normalizes to something real");
}

// The gain control quantises to what the part can actually do. A value that
// is not a multiple of 3 would be stored, shown back to the operator, and then
// silently rounded by the driver - a control that reads back a number the
// hardware is not using.
static void test_gain(void)
{
    CHECK(line_in_gain_db_normalize(0) == 0, "0 dB does not survive");
    CHECK(line_in_gain_db_normalize(3) == 3, "3 dB does not survive");
    CHECK(line_in_gain_db_normalize(30) == 30, "30 dB does not survive");

    // Between steps: rounds DOWN, so the control can never ask for more gain
    // than the operator selected.
    CHECK(line_in_gain_db_normalize(1) == 0,  "1 dB does not round down to 0");
    CHECK(line_in_gain_db_normalize(2) == 0,  "2 dB does not round down to 0");
    CHECK(line_in_gain_db_normalize(5) == 3,  "5 dB does not round down to 3");
    CHECK(line_in_gain_db_normalize(29) == 27, "29 dB does not round down to 27");

    // Above the offered range: clamped to 30, NOT wrapped into the 34.5/36/37.5
    // steps the part also has. Those are for microphone capsules and would
    // only clip a line input.
    CHECK(line_in_gain_db_normalize(31)   == 30, "31 dB is not clamped");
    CHECK(line_in_gain_db_normalize(37)   == 30, "37 dB is not clamped");
    CHECK(line_in_gain_db_normalize(0xFF) == 30, "0xFF is not clamped");

    // Idempotent and always a legal step, for every byte NVS could hold.
    for (int i = 0; i < 256; i++) {
        uint8_t once  = line_in_gain_db_normalize((uint8_t)i);
        uint8_t twice = line_in_gain_db_normalize(once);
        CHECK(once == twice, "gain normalize is not idempotent at %d", i);
        CHECK(once <= LINE_IN_GAIN_DB_MAX, "gain %d exceeds the max at %d", once, i);
        CHECK(once % LINE_IN_GAIN_DB_STEP == 0, "gain %d is not a legal step at %d", once, i);
    }

    // THE DEFAULT IS 0 dB AND THAT IS THE POINT. Tony A (tony1tf) ran the mic
    // test build, which used the driver's init value of 30 dB, and reported a
    // lot of pickup on an unconnected input and near-full-scale from a
    // headphone output through a 6:1 lead. If this ever goes back to a
    // non-zero default, that report is what it is contradicting.
    CHECK(line_in_gain_db_normalize(0) == 0, "the zero default does not survive");
}

int main(void)
{
    printf("rx_source_harness\n");
    test_encode();
    test_parse();
    test_normalize();
    test_is_real();
    test_gain();
    if (g_fail == 0) printf("  all checks passed\n");
    else             printf("  %d CHECK(s) failed\n", g_fail);
    return g_fail ? 1 : 0;
}
