// HID Report Map parsing - see hid_report_map.h for why this exists.
//
// The item stream is a sequence of (prefix, data) pairs. The prefix byte encodes
// size (bits 0-1), type (2-3) and tag (4-7); size 3 means four bytes, not three.
// Only the handful of items a mouse needs are interpreted:
//
//   Global: Usage Page (0x05), Report Size (0x75), Report Count (0x95),
//           Report ID (0x85)
//   Local:  Usage (0x09), Usage Minimum/Maximum (0x1A/0x2A) for ranges
//   Main:   Input (0x81), Collection (0xA1), End Collection (0xC0)
//
// A bit cursor advances by size x count for every Input item, so by the time an
// item declaring X and Y is reached the cursor is that field's bit offset. Padding
// (an Input with the Constant bit set and no usages) advances the cursor like any
// other item, which is exactly why the cursor has to be kept for ALL inputs and
// not only the interesting ones.

#include "hid_report_map.h"

#define USAGE_PAGE_GENERIC_DESKTOP 0x01
#define USAGE_X      0x30
#define USAGE_Y      0x31
#define USAGE_WHEEL  0x38

// Local usages are collected per main item and cleared by it. 12 is generous for a
// mouse (buttons are declared as a range, not one usage each).
#define MAX_LOCAL_USAGES 12

bool hid_report_map_parse(const uint8_t *desc, size_t len, hid_mouse_layout_t *out)
{
    if (!desc || !out) return false;
    for (size_t i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;

    uint16_t usage_page = 0;
    uint8_t  report_size = 0, report_count = 0, report_id = 0;
    uint16_t bit_cursor = 0;
    uint8_t  cur_report_id = 0;      // the ID whose payload bit_cursor belongs to

    uint16_t local_usage[MAX_LOCAL_USAGES];
    int      n_local = 0;

    size_t p = 0;
    while (p < len) {
        uint8_t prefix = desc[p++];
        if (prefix == 0xFE) return false;        // long item: not in any mouse, and
                                                 // guessing its length would be worse
                                                 // than declining
        uint8_t size_code = prefix & 0x03;
        uint8_t nbytes    = (size_code == 3) ? 4 : size_code;
        if (p + nbytes > len) return false;       // truncated

        uint32_t data = 0;
        for (uint8_t b = 0; b < nbytes; b++) data |= (uint32_t)desc[p + b] << (8 * b);
        p += nbytes;

        uint8_t tag_type = prefix & 0xFC;         // tag+type, size bits cleared

        switch (tag_type) {
        case 0x04:   // Usage Page (global)
            usage_page = (uint16_t)data;
            break;
        case 0x74:   // Report Size (global)
            report_size = (uint8_t)data;
            break;
        case 0x94:   // Report Count (global)
            report_count = (uint8_t)data;
            break;
        case 0x84:   // Report ID (global)
            // A new report ID restarts the payload, so the bit cursor restarts too.
            report_id = (uint8_t)data;
            if (report_id != cur_report_id) {
                cur_report_id = report_id;
                bit_cursor = 0;
            }
            break;
        case 0x08:   // Usage (local)
            if (n_local < MAX_LOCAL_USAGES) local_usage[n_local++] = (uint16_t)data;
            break;
        case 0x18:   // Usage Minimum (local)
        case 0x28:   // Usage Maximum (local)
            // Ranges are how buttons are declared. They are not X/Y, and treating
            // the endpoints as usages could match X or Y by accident, so ignore
            // them - but they still contribute to the bit cursor via the Input
            // item below, which is what matters here.
            break;
        case 0x80: { // Input (main)
            uint16_t width = (uint16_t)report_size * (uint16_t)report_count;
            if (usage_page == USAGE_PAGE_GENERIC_DESKTOP && report_count > 0) {
                int xi = -1, yi = -1, wi = -1;
                for (int u = 0; u < n_local; u++) {
                    if (local_usage[u] == USAGE_X     && xi < 0) xi = u;
                    if (local_usage[u] == USAGE_Y     && yi < 0) yi = u;
                    if (local_usage[u] == USAGE_WHEEL && wi < 0) wi = u;
                }
                // X and Y are consecutive fields inside this item, in the order the
                // usages were listed - so field n sits at cursor + n*report_size.
                if (!out->valid && xi >= 0 && yi >= 0) {
                    out->valid     = true;
                    out->report_id = cur_report_id;
                    out->x_bits    = report_size;
                    out->y_bits    = report_size;
                    out->x_bit     = bit_cursor + (uint16_t)(xi * report_size);
                    out->y_bit     = bit_cursor + (uint16_t)(yi * report_size);
                }
                // The wheel is commonly a SEPARATE Input item after the one holding
                // X and Y, so it is looked for on every pass and not only on the
                // one that matched - which is what the first version got wrong.
                if (!out->have_wheel && wi >= 0 &&
                    (out->valid || xi >= 0)) {
                    out->have_wheel = true;
                    out->wheel_bits = report_size;
                    out->wheel_bit  = bit_cursor + (uint16_t)(wi * report_size);
                }
            }
            bit_cursor = (uint16_t)(bit_cursor + width);
            // total_bits is the payload size of the report that carries X and Y -
            // NOT the cursor at the end of the descriptor, which is what it used to
            // be and which made it useless as a length check. A mouse descriptor
            // continues past the mouse collection (a vendor page, a consumer page),
            // so on the bench mouse the old version reported 152 bits for a report
            // that is genuinely 56, and a check against it rejected every report.
            if (out->valid && cur_report_id == out->report_id)
                out->total_bits = bit_cursor;
            n_local = 0;
            break;
        }
        case 0x90:   // Output (main)
        case 0xB0:   // Feature (main)
            // Not part of an input report's payload - do NOT advance the cursor.
            n_local = 0;
            break;
        case 0xA0:   // Collection
        case 0xC0:   // End Collection
            n_local = 0;
            break;
        default:
            // Everything else (logical/physical min-max, unit, etc.) affects
            // interpretation of values, not their position.
            break;
        }

    }

    if (!out->valid) return false;
    // A field wider than 32 bits, or an offset beyond a plausible report, means the
    // parse went wrong somewhere. Refuse rather than hand back nonsense.
    if (out->x_bits == 0 || out->x_bits > 32 || out->y_bits == 0 || out->y_bits > 32 ||
        out->x_bit > 512 || out->y_bit > 512) {
        out->valid = false;
        return false;
    }
    return true;
}

int hid_field_signed(const uint8_t *report, size_t len, uint16_t bit_off, uint8_t bits_wide)
{
    if (!report || bits_wide == 0 || bits_wide > 32) return 0;
    if ((size_t)bit_off + bits_wide > len * 8u) return 0;

    uint32_t v = 0;
    for (uint8_t b = 0; b < bits_wide; b++) {
        uint16_t bit = (uint16_t)(bit_off + b);
        if ((report[bit >> 3] >> (bit & 7)) & 1u) v |= (uint32_t)1u << b;
    }
    // Sign-extend from bits_wide.
    if (bits_wide < 32) {
        uint32_t sign = (uint32_t)1u << (bits_wide - 1);
        if (v & sign) v |= ~(((uint32_t)1u << bits_wide) - 1u);
    }
    return (int)(int32_t)v;
}

bool hid_fallback_decode(const uint8_t *report, size_t len, hid_mouse_move_t *out)
{
    if (!report || !out || len < 3) return false;

    out->dx = out->dy = out->wheel = 0;
    out->buttons = report[0];

    // 16-BIT layout - Microsoft Surface Arc, captured from Kevin KW6E's log:
    //     00 f5 ff fe ff 01 00 00 00   ->  X=-11  Y=-2   wheel=+1
    //     00 06 00 0b 00 ff ff 00 00   ->  X=+6   Y=+11  wheel=-1
    // [buttons][X lo][X hi][Y lo][Y hi][wheel lo][wheel hi][pan][pan]
    // Checked against every distinct report in his capture, both signs.
    if (len >= 9) {
        out->dx = (int)(int16_t)((uint16_t)report[1] | ((uint16_t)report[2] << 8));
        out->dy = (int)(int16_t)((uint16_t)report[3] | ((uint16_t)report[4] << 8));
        out->wheel = (int)(int16_t)((uint16_t)report[5] | ((uint16_t)report[6] << 8));
        return true;
    }

    // 12-BIT PACKED - Logitech M240, captured on hardware 2026-08-09:
    //     00 00 d9 0f fd 00 00   ->  X=-39  Y=-48
    //     00 00 02 e0 ff 00 00   ->  X=+2   Y=-2
    // X and Y SHARE byte 3, a nibble each.
    if (len >= 5) {
        int x = report[2] | ((report[3] & 0x0F) << 8);
        int y = ((report[3] >> 4) & 0x0F) | (report[4] << 4);
        if (x & 0x800) x -= 0x1000;          // 12-bit two's complement
        if (y & 0x800) y -= 0x1000;
        out->dx = x;
        out->dy = y;
        if (len >= 6) out->wheel = (int8_t)report[5];
        return true;
    }

    // Boot protocol: [buttons][dx int8][dy int8][wheel int8].
    out->dx = (int8_t)report[1];
    out->dy = (int8_t)report[2];
    if (len >= 4) out->wheel = (int8_t)report[3];
    return true;
}
