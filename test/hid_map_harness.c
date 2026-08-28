// hid_map_harness.c - host-side verification of hid_report_map_parse() and
// hid_field_signed() (main/hid_report_map.c), which decide how a BLE mouse's
// movement reports are laid out.
//
// Build + run (from the repo root):
//   gcc -O2 -Wall -Wextra -I main -o test/hid_map.exe test/hid_map_harness.c main/hid_report_map.c
//   ./test/hid_map.exe
//
// WHY THIS EXISTS
//   The thing being replaced was an assumption: bt_hid_mouse.c treated every report
//   of five bytes or more as the 12-bit packed layout captured off one Logitech
//   M240. A mouse using 16-bit movement decodes under that assumption into numbers
//   in the thousands, and because the cursor is clamped to the screen the pointer
//   races sideways and pins itself to the top edge - which is what Samuel W7STF
//   reported and what case 3 below reproduces numerically.
//
//   It cannot be tested on the device without owning every mouse. The descriptors
//   are public and fixed, so it is testable here, and case 3 in particular is the
//   whole point: it fails loudly under the OLD assumption and passes under the
//   parser.
//
// MUTATION RESULTS for the KEYBOARD cases (2026-08-28)
//   Caught: Output/Feature items advancing the input bit cursor (3 failures) -
//   the LED report sits between the modifier byte and the keycodes, so getting
//   that wrong moves every keystroke; and the Variable/Array discriminator
//   inverted (2), which swaps the modifier bitmap for the keycode slots.
//
//   SURVIVED FIRST TIME, and it was a REAL GAP: deleting the bit-cursor reset at
//   a Report ID boundary left every case passing, because case 7's keyboard is
//   the FIRST report and its cursor starts at 0 anyway. Case 9 is the same
//   device with the mouse first, where the omission puts the modifier byte at
//   bit 24 instead of 0 - it now fails with 3 failures. A combo device is
//   exactly where this matters and the first draft could not see it.
//
//   SURVIVED and EQUIVALENT: dropping the INPUT_CONSTANT filter changes nothing
//   for any realistic descriptor, because the reserved byte is declared
//   Constant+VARIABLE with Report Size 8 and so is rejected by both branches on
//   their own terms. The filter is kept because the HID spec says padding is not
//   data; do not delete it, and do not contrive a test to kill this one.
//
// WHY IT COMPILES THE REAL FILE
//   Mirroring the parser here would let the two drift, and a drifted copy of a
//   parser is worse than no test: it would keep passing while the device got it
//   wrong. Same reasoning as test/spot_sig_harness.c.

#include <stdio.h>
#include <string.h>
#include "hid_report_map.h"

static int g_fail = 0;

static void check(const char *what, long got, long want)
{
    if (got != want) {
        printf("  FAIL %-34s got %ld want %ld\n", what, got, want);
        g_fail++;
    }
}

// ---------------------------------------------------------------- descriptors

// 1. The standard boot-protocol mouse: 3 buttons + 5 bits padding, then two
//    8-bit relative axes. This is the shape in the USB HID spec's own appendix.
static const uint8_t DESC_BOOT[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00, 0x25, 0x01,
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)      <- 3 bits of buttons
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x01,        //     Input (Cnst,Ary,Abs)      <- 5 bits of padding
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data,Var,Rel)      <- X then Y, 8 bits each
    0xC0, 0xC0
};

// 2. 16-bit movement with a wheel in a SEPARATE input item, and a Report ID -
//    the layout the old code decoded into nonsense.
static const uint8_t DESC_16BIT_ID[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, 0x02,        // Report ID (2)
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x81, 0x02,   // 5 button bits
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,   // 3 bits padding  -> byte boundary
    0x05, 0x01,
    0x09, 0x30, 0x09, 0x31,               // X, Y
    0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F,   // 16-bit logical range
    0x75, 0x10, 0x95, 0x02, 0x81, 0x06,   // Report Size 16, Count 2
    0x09, 0x38,                           // Usage (Wheel)
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06,   // wheel, 8 bits, separate item
    0xC0, 0xC0
};

// 3. The 12-bit packed layout captured off a Logitech M240 on hardware
//    (bt_hid_mouse.c records the bytes it saw). X and Y share a byte.
static const uint8_t DESC_12BIT[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,  // 8 button bits
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                          // 8 bits padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x00, 0xF8, 0x26, 0xFF, 0x07,
    0x75, 0x0C, 0x95, 0x02, 0x81, 0x06,                          // 12 bits x2
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0
};

// The bench mouse (Logitech, de:32:38:0b:83:5a), taken from its own Report Map as
// read off the hardware on 2026-08-12: 3 button bits, 13 padding bits, then two
// 12-bit relative fields for X and Y, then wheel and pan. What makes it the case
// worth having is the SECOND top-level collection: a real mouse descriptor does not
// end with the mouse, and total_bits used to run to the end of the whole
// descriptor - 152 bits for a report that is genuinely 56 - so a length check
// against it discarded every report the mouse sent.
static const uint8_t DESC_TWO_COLLECTIONS[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x95, 0x03, 0x75, 0x01, 0x15, 0x00, 0x25, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x81, 0x02,               // 3 button bits
    0x95, 0x0D, 0x81, 0x03,                                       // 13 bits padding
    0x05, 0x01, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07,
    0x75, 0x0C, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x06,   // 12 bits x2
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01,
    0x09, 0x38, 0x81, 0x06,                                       // wheel, 8 bits
    0x0A, 0x38, 0x02, 0x81, 0x06,                                 // pan, 8 bits
    0xC0, 0xC0,
    // A vendor collection on its own report ID - must not touch report 2's size.
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x11,
    0x75, 0x08, 0x95, 0x13, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x09, 0x01, 0x81, 0x00,                                       // 19 bytes vendor
    0xC0
};

static void test_fallback(void)
{
    printf("F. fallback decode (no usable Report Map)\n");
    hid_mouse_move_t m;

    /* Microsoft Surface Arc, 9 bytes with 16-bit movement. These are REAL
       reports out of Kevin KW6E's diagnostic log, not constructed - which is the
       point, because the layout they disprove was also "known" from real
       hardware.

       Run the first one through the 12-bit M240 arithmetic that used to handle
       every report of five bytes or more and it gives X=-1280, Y=0: a big jump
       the wrong way and no vertical movement at all. In his words, "connects and
       scrolls perfectly, but moving the mouse pointer is erratic". */
    const uint8_t arc_a[] = { 0x00, 0x06, 0x00, 0x0b, 0x00, 0xff, 0xff, 0x00, 0x00 };
    if (!hid_fallback_decode(arc_a, sizeof arc_a, &m)) { printf("  FAIL returned false\n"); g_fail++; }
    else { check("arc +X", m.dx, 6); check("arc +Y", m.dy, 11); check("arc wheel-", m.wheel, -1); }

    const uint8_t arc_b[] = { 0x00, 0xf5, 0xff, 0xfe, 0xff, 0x01, 0x00, 0x00, 0x00 };
    if (hid_fallback_decode(arc_b, sizeof arc_b, &m)) {
        check("arc -X", m.dx, -11); check("arc -Y", m.dy, -2); check("arc wheel+", m.wheel, 1);
    }

    /* ⚠ CONSTRUCTED, not captured - and it earns its place. Every movement in
       Kevin's log is small enough that the high byte is only sign extension, so
       an 8-bit read of the low byte returns the identical answer and the capture
       cannot tell 8-bit from 16-bit at all. A mutation run proved that: dropping
       the high byte broke nothing. A fast flick genuinely exceeds 127, so this
       pins the field WIDTH, which the real data cannot. */
    const uint8_t arc_fast[] = { 0x00, 0x40, 0x01, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00 };
    if (hid_fallback_decode(arc_fast, sizeof arc_fast, &m)) {
        check("arc wide X", m.dx,  320);    /* 0x0140 - low byte alone reads 64 */
        check("arc wide Y", m.dy, -256);    /* 0xff00 - low byte alone reads 0  */
    }

    /* Idle report: no movement at all. A layout error shows up here first, as a
       cursor that drifts on its own with the mouse untouched. */
    const uint8_t arc_idle[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (hid_fallback_decode(arc_idle, sizeof arc_idle, &m)) {
        check("idle dx", m.dx, 0); check("idle dy", m.dy, 0); check("idle wheel", m.wheel, 0);
    }

    /* Logitech M240, 7 bytes, 12-bit packed with X and Y sharing a nibble.
       Captured on hardware 2026-08-09. This must keep working: it is the mouse
       the old code was written for, and breaking it to fix Kevin's would just
       move the bug to someone else. */
    const uint8_t m240_a[] = { 0x00, 0x00, 0xd9, 0x0f, 0xfd, 0x00, 0x00 };
    if (hid_fallback_decode(m240_a, sizeof m240_a, &m)) {
        check("m240 X", m.dx, -39); check("m240 Y", m.dy, -48);
    }
    const uint8_t m240_b[] = { 0x00, 0x00, 0x02, 0xe0, 0xff, 0x00, 0x00 };
    if (hid_fallback_decode(m240_b, sizeof m240_b, &m)) {
        check("m240 X2", m.dx, 2); check("m240 Y2", m.dy, -2);
    }

    /* Boot protocol, 3-4 bytes. */
    const uint8_t boot[] = { 0x00, 0x05, 0xfb, 0x01 };
    if (hid_fallback_decode(boot, sizeof boot, &m)) {
        check("boot X", m.dx, 5); check("boot Y", m.dy, -5); check("boot wheel", m.wheel, 1);
    }

    /* Too short to carry movement: refused, not read past the end. */
    const uint8_t stub[] = { 0x00, 0x01 };
    if (hid_fallback_decode(stub, sizeof stub, &m)) { printf("  FAIL 2-byte report accepted\n"); g_fail++; }
}

int main(void)
{
    hid_mouse_layout_t L;

    test_fallback();

    printf("1. boot-protocol mouse\n");
    if (!hid_report_map_parse(DESC_BOOT, sizeof DESC_BOOT, &L)) {
        printf("  FAIL parse returned false\n"); g_fail++;
    } else {
        check("report_id", L.report_id, 0);
        check("x_bit",     L.x_bit,     8);      // 3 button bits + 5 padding
        check("y_bit",     L.y_bit,    16);
        check("x_bits",    L.x_bits,    8);
        check("have_wheel",L.have_wheel,0);
        // 00 05 FB  ->  X=+5, Y=-5
        const uint8_t r[] = { 0x00, 0x05, 0xFB };
        check("decode X", hid_field_signed(r, sizeof r, L.x_bit, L.x_bits),  5);
        check("decode Y", hid_field_signed(r, sizeof r, L.y_bit, L.y_bits), -5);
    }

    printf("2. 16-bit movement, report ID, wheel in its own item\n");
    if (!hid_report_map_parse(DESC_16BIT_ID, sizeof DESC_16BIT_ID, &L)) {
        printf("  FAIL parse returned false\n"); g_fail++;
    } else {
        check("report_id",  L.report_id,  2);
        check("x_bit",      L.x_bit,      8);    // 5 buttons + 3 padding
        check("y_bit",      L.y_bit,     24);
        check("x_bits",     L.x_bits,    16);
        check("have_wheel", L.have_wheel, 1);
        check("wheel_bit",  L.wheel_bit, 40);
        check("wheel_bits", L.wheel_bits, 8);
        // payload after the ID byte: buttons, X=+300 (0x012C), Y=-2 (0xFFFE), wheel=-1
        const uint8_t r[] = { 0x00, 0x2C, 0x01, 0xFE, 0xFF, 0xFF };
        check("decode X", hid_field_signed(r, sizeof r, L.x_bit, L.x_bits), 300);
        check("decode Y", hid_field_signed(r, sizeof r, L.y_bit, L.y_bits),  -2);
        check("decode wheel",
              hid_field_signed(r, sizeof r, L.wheel_bit, L.wheel_bits), -1);

        // THE REGRESSION THAT MATTERS - and this harness corrected me on what it
        // actually is, which is why it is asserted rather than described.
        //
        // Decoding a 16-bit mouse as 12-bit packed reads Y FOUR BITS EARLY: it takes
        // Y's low byte shifted up by four and picks up X's high nibble in the bottom
        // bits. So Y comes out about SIXTEEN TIMES too large, while X comes out
        // CORRECT by coincidence - the low 12 bits of a small 16-bit two's-complement
        // value are that same value in 12-bit two's complement.
        //
        // That is exactly the reported symptom, and more precisely than "wrong
        // values": a 16x vertical gain drives the cursor into the top or bottom edge
        // on any movement at all, and the clamp in hid_cursor.c holds it there - so
        // the pointer appears to move only left and right, along the very top of the
        // screen. My first version of this test asserted "runaway values in the
        // thousands" and FAILED, because for small movements Y is merely 16x out.
        static const int mv[][2] = { {2,-2}, {5,5}, {20,-10}, {100,40} };
        for (size_t k = 0; k < sizeof mv / sizeof mv[0]; k++) {
            int X = mv[k][0], Y = mv[k][1];
            uint8_t q[6] = { 0, (uint8_t)(X & 0xFF), (uint8_t)((X >> 8) & 0xFF),
                                (uint8_t)(Y & 0xFF), (uint8_t)((Y >> 8) & 0xFF), 0 };
            // the parser must get both right...
            check("parser X", hid_field_signed(q, sizeof q, L.x_bit, L.x_bits), X);
            check("parser Y", hid_field_signed(q, sizeof q, L.y_bit, L.y_bits), Y);
            // ...and the old assumption must be wrong on Y by roughly 16x, which is
            // what made the pointer unusable rather than merely inaccurate.
            int oy = ((q[2] >> 4) & 0x0F) | (q[3] << 4);
            if (oy & 0x800) oy -= 0x1000;
            double ratio = (double)oy / (double)Y;
            if (ratio < 12.0 || ratio > 24.0) {
                printf("  FAIL old-layout Y gain was %.1fx for Y=%d (expected ~16x)\n",
                       ratio, Y);
                g_fail++;
            }
        }
        printf("     old assumption: X right by coincidence, Y ~16x too large\n");
    }

    printf("3. 12-bit packed (Logitech M240)\n");
    if (!hid_report_map_parse(DESC_12BIT, sizeof DESC_12BIT, &L)) {
        printf("  FAIL parse returned false\n"); g_fail++;
    } else {
        check("report_id", L.report_id, 0);
        check("x_bit",     L.x_bit,    16);      // 8 button bits + 8 padding
        check("y_bit",     L.y_bit,    28);      // 16 + 12
        check("x_bits",    L.x_bits,   12);
        check("have_wheel",L.have_wheel,1);
        check("wheel_bit", L.wheel_bit,40);
        // The capture from hardware: 00 00 d9 0f fd 00 00 -> X=-39 Y=-48
        const uint8_t r[] = { 0x00, 0x00, 0xd9, 0x0f, 0xfd, 0x00, 0x00 };
        check("decode X (hw capture)", hid_field_signed(r, sizeof r, L.x_bit, L.x_bits), -39);
        check("decode Y (hw capture)", hid_field_signed(r, sizeof r, L.y_bit, L.y_bits), -48);
        const uint8_t r2[] = { 0x00, 0x00, 0x02, 0xe0, 0xff, 0x00, 0x00 };
        check("decode X (hw capture 2)", hid_field_signed(r2, sizeof r2, L.x_bit, L.x_bits), 2);
        check("decode Y (hw capture 2)", hid_field_signed(r2, sizeof r2, L.y_bit, L.y_bits), -2);
    }

    // Mutation testing found this branch uncovered: none of the three descriptors
    // above contains a FOUR-byte item, so "size code 3 means 4 bytes, not 3" - the
    // classic HID item-parsing trap - was never exercised, and breaking it on
    // purpose still passed. Real descriptors do use 4-byte items for 32-bit logical
    // ranges, and getting the length wrong desynchronises the whole item stream, so
    // this covers it: a 4-byte Logical Maximum (0x27) sits before X and Y, and a
    // 3-byte read would put them in the wrong place.
    printf("4. a 4-byte item must not desynchronise the stream\n");
    {
        static const uint8_t DESC_LONGDATA[] = {
            0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
            0x05, 0x09, 0x19, 0x01, 0x29, 0x08,
            0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,  // 8 button bits
            0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
            0x17, 0x00, 0x00, 0x00, 0x80,        // Logical Minimum, 4 bytes
            0x27, 0xFF, 0xFF, 0xFF, 0x7F,        // Logical Maximum, 4 bytes
            0x75, 0x10, 0x95, 0x02, 0x81, 0x06,  // Report Size 16, Count 2
            0xC0, 0xC0
        };
        hid_mouse_layout_t M;
        if (!hid_report_map_parse(DESC_LONGDATA, sizeof DESC_LONGDATA, &M)) {
            printf("  FAIL parse returned false\n"); g_fail++;
        } else {
            check("x_bit after 4-byte items",  M.x_bit,  8);
            check("y_bit after 4-byte items",  M.y_bit, 24);
            check("x_bits after 4-byte items", M.x_bits, 16);
            const uint8_t r[] = { 0x00, 0x2C, 0x01, 0xFE, 0xFF };
            check("decode X", hid_field_signed(r, sizeof r, M.x_bit, M.x_bits), 300);
            check("decode Y", hid_field_signed(r, sizeof r, M.y_bit, M.y_bits),  -2);
        }
    }

    printf("5. malformed input must be refused, not guessed\n");
    {
        hid_mouse_layout_t T;
        if (hid_report_map_parse(NULL, 0, &T))                       { printf("  FAIL NULL\n"); g_fail++; }
        if (hid_report_map_parse(DESC_BOOT, 4, &T))                  { printf("  FAIL truncated\n"); g_fail++; }
        const uint8_t only_buttons[] = { 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
                                         0x95, 0x03, 0x75, 0x01, 0x81, 0x02 };
        if (hid_report_map_parse(only_buttons, sizeof only_buttons, &T)) {
            printf("  FAIL a descriptor with no X/Y was accepted\n"); g_fail++;
        }
        const uint8_t long_item[] = { 0xFE, 0x02, 0x00, 0x00, 0x00 };
        if (hid_report_map_parse(long_item, sizeof long_item, &T))    { printf("  FAIL long item\n"); g_fail++; }
        // A field running past the end of the report reads 0 rather than off the end
        const uint8_t tiny[] = { 0x00 };
        check("out-of-range field", hid_field_signed(tiny, sizeof tiny, 8, 16), 0);
    }

    printf("5. bench mouse: 12-bit X/Y and a second collection after it\n");
    if (!hid_report_map_parse(DESC_TWO_COLLECTIONS, sizeof DESC_TWO_COLLECTIONS, &L)) {
        printf("  FAIL parse returned false\n"); g_fail++;
    } else {
        // These four are the values the device logged for this mouse, so this case
        // ties the harness to real hardware rather than to my reading of the spec.
        check("report_id",  L.report_id,  2);
        check("x_bit",      L.x_bit,     16);
        check("y_bit",      L.y_bit,     28);
        check("x_bits",     L.x_bits,    12);
        check("have_wheel", L.have_wheel, 1);
        check("wheel_bit",  L.wheel_bit, 40);
        // THE POINT OF THIS CASE: report 2 is 3+13+24+8+8 = 56 bits, and the vendor
        // collection that follows must not be counted. 152 was the old answer.
        check("total_bits", L.total_bits, 56);

        // A real 7-byte report off the wire: 00 00 0b e0 fe 00 00, which the device
        // rejected as "56 bits, map declares 152" before this was fixed.
        const uint8_t r[] = { 0x00, 0x00, 0x0B, 0xE0, 0xFE, 0x00, 0x00 };
        check("report fits", (int)(sizeof r * 8) >= (int)L.total_bits, 1);
        check("decode X", hid_field_signed(r, sizeof r, L.x_bit, L.x_bits),   11);
        check("decode Y", hid_field_signed(r, sizeof r, L.y_bit, L.y_bits),  -18);
    }


    // ---------------------------------------------------------------------
    printf("6. keyboard report: the standard boot-style layout\n");
    {
        // The keyboard descriptor every HOGP keyboard publishes, near enough
        // verbatim from HID 1.11 Appendix B.1 - modifier bitmap, one reserved
        // byte, five LED output bits + padding, then six keycode slots.
        static const uint8_t DESC_KBD[] = {
            0x05, 0x01,        // Usage Page (Generic Desktop)
            0x09, 0x06,        //   Usage (Keyboard)
            0xA1, 0x01,        //   Collection (Application)
            0x05, 0x07,        //     Usage Page (Keyboard/Keypad)
            0x19, 0xE0,        //     Usage Minimum (LeftControl)
            0x29, 0xE7,        //     Usage Maximum (Right GUI)
            0x15, 0x00, 0x25, 0x01,
            0x75, 0x01,        //     Report Size (1)
            0x95, 0x08,        //     Report Count (8)
            0x81, 0x02,        //     INPUT (Data,Var,Abs)  <- the modifier byte
            0x95, 0x01, 0x75, 0x08,
            0x81, 0x03,        //     INPUT (Cnst,Var,Abs)  <- reserved byte
            0x95, 0x05, 0x75, 0x01,
            0x05, 0x08,        //     Usage Page (LEDs)
            0x19, 0x01, 0x29, 0x05,
            0x91, 0x02,        //     OUTPUT - must NOT move the input cursor
            0x95, 0x01, 0x75, 0x03,
            0x91, 0x03,        //     OUTPUT padding
            0x95, 0x06, 0x75, 0x08,
            0x15, 0x00, 0x25, 0x65,
            0x05, 0x07,        //     Usage Page (Keyboard/Keypad)
            0x19, 0x00, 0x29, 0x65,
            0x81, 0x00,        //     INPUT (Data,Ary,Abs)  <- the six keycodes
            0xC0
        };
        hid_kbd_layout_t K;
        if (!hid_report_map_parse_keyboard(DESC_KBD, sizeof DESC_KBD, &K)) {
            printf("  FAIL parse returned false\n"); g_fail++;
        } else {
            check("report_id",  K.report_id,  0);
            check("mod_bit",    K.mod_bit,    0);
            check("mod_bits",   K.mod_bits,   8);
            // THE OUTPUT ITEMS MUST NOT ADVANCE THE INPUT CURSOR. If they do,
            // key_bit lands past 16 and every keystroke decodes as the wrong key.
            check("key_bit",    K.key_bit,   16);
            check("key_bits",   K.key_bits,   8);
            check("key_count",  K.key_count,  6);
            check("total_bits", K.total_bits, 64);

            // A real boot report: Shift held, "a" pressed.
            const uint8_t r[] = { 0x02, 0x00, 0x04, 0, 0, 0, 0, 0 };
            check("mod byte",  hid_field_signed(r, sizeof r, K.mod_bit, K.mod_bits), 2);
            check("slot 0",    hid_field_signed(r, sizeof r, K.key_bit, K.key_bits), 4);
            check("slot 1 empty",
                  hid_field_signed(r, sizeof r, K.key_bit + K.key_bits, K.key_bits), 0);
        }
    }

    // ---------------------------------------------------------------------
    printf("7. combo keyboard+touchpad: each half found, with its own report ID\n");
    {
        // The shape Don N2VGU's Rii i4 has: one Report Map, two report IDs. The
        // keyboard parse must return report 1 and the MOUSE parse report 2 - if
        // either picks up the other's fields the device routes notifications to
        // the wrong decoder, which is the bug being fixed.
        static const uint8_t DESC_COMBO[] = {
            0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
            0x85, 0x01,                     //   REPORT ID 1 - keyboard
            0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
            0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
            0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
            0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
            0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
            0xC0,
            0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
            0x85, 0x02,                     //   REPORT ID 2 - mouse
            0x09, 0x01, 0xA1, 0x00,
            0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
            0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
            0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
            0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
            0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
            0xC0, 0xC0
        };
        hid_kbd_layout_t K;
        if (!hid_report_map_parse_keyboard(DESC_COMBO, sizeof DESC_COMBO, &K)) {
            printf("  FAIL keyboard parse returned false\n"); g_fail++;
        } else {
            check("kbd report_id", K.report_id, 1);
            check("kbd mod_bit",   K.mod_bit,   0);
            check("kbd key_bit",   K.key_bit,  16);
            check("kbd key_count", K.key_count, 6);
        }
        hid_mouse_layout_t M;
        if (!hid_report_map_parse(DESC_COMBO, sizeof DESC_COMBO, &M)) {
            printf("  FAIL mouse parse returned false\n"); g_fail++;
        } else {
            // The mouse must be found on report 2 and must not be confused by
            // the keyboard collection that precedes it.
            check("mouse report_id", M.report_id,  2);
            check("mouse x_bit",     M.x_bit,      8);
            check("mouse x_bits",    M.x_bits,     8);
            check("mouse wheel",     M.have_wheel, 1);
        }
    }

    // ---------------------------------------------------------------------
    printf("8. a mouse-only map has no keyboard, and must say so\n");
    {
        hid_kbd_layout_t K;
        check("mouse-only refused",
              hid_report_map_parse_keyboard(DESC_TWO_COLLECTIONS,
                                            sizeof DESC_TWO_COLLECTIONS, &K) ? 1 : 0, 0);
        check("and not marked valid", K.valid ? 1 : 0, 0);
        // Truncation must be refused rather than half-parsed.
        static const uint8_t DESC_KBD_CUT[] = {
            0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
            0x19, 0xE0, 0x29, 0xE7, 0x75, 0x01, 0x95
        };
        check("truncated refused",
              hid_report_map_parse_keyboard(DESC_KBD_CUT, sizeof DESC_KBD_CUT, &K) ? 1 : 0, 0);
    }

    // ---------------------------------------------------------------------
    printf("9. combo the OTHER way round: mouse first, keyboard second\n");
    {
        // Case 7 has the keyboard first, which hides a bug: the bit cursor
        // starts at 0 anyway, so failing to RESET it at a Report ID boundary
        // costs nothing there. Found by mutation testing - deleting the reset
        // left case 7 passing. With the mouse first, the keyboard's fields sit
        // 24 bits into a payload that does not contain them, and every
        // keystroke decodes as the wrong key.
        static const uint8_t DESC_COMBO2[] = {
            0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
            0x85, 0x01,                     //   REPORT ID 1 - mouse (24 bits)
            0x09, 0x01, 0xA1, 0x00,
            0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
            0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
            0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
            0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
            0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
            0xC0, 0xC0,
            0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
            0x85, 0x02,                     //   REPORT ID 2 - keyboard
            0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
            0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
            0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
            0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
            0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
            0xC0
        };
        hid_kbd_layout_t K;
        if (!hid_report_map_parse_keyboard(DESC_COMBO2, sizeof DESC_COMBO2, &K)) {
            printf("  FAIL keyboard parse returned false\n"); g_fail++;
        } else {
            check("kbd report_id", K.report_id, 2);
            // THE POINT: 0 and 16, not 24 and 40.
            check("kbd mod_bit",   K.mod_bit,   0);
            check("kbd key_bit",   K.key_bit,  16);
            check("kbd total",     K.total_bits, 64);
        }
        hid_mouse_layout_t M;
        if (!hid_report_map_parse(DESC_COMBO2, sizeof DESC_COMBO2, &M)) {
            printf("  FAIL mouse parse returned false\n"); g_fail++;
        } else {
            check("mouse report_id", M.report_id, 1);
            check("mouse x_bit",     M.x_bit,     8);
        }
    }
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail,
           g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
