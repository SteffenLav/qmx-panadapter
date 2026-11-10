// log_free_text.h - clean a line of operator free text for an ADIF field.
//
// Used by the web log window's Name / QTH / Remarks (Gyula HA3HZ, 2026-11-10).
// Header-only so test/log_free_text_harness.c runs the same code on the host.
//
//  - leading and trailing spaces go
//  - control characters go (a newline inside a field would break the
//    one-record-per-line file)
//  - '<' and '>' become spaces. ADIF itself would carry them - fields are
//    length-prefixed - but adif_log_extract_field() finds a field by searching
//    for "<NAME:", so a remark containing "<CALL:" would be read as the call.
//  - capped at dst_sz - 1 BYTES, and never in the middle of a UTF-8 character:
//    a name like "Győző" must not end in half a letter.
#pragma once
#include <stddef.h>

static inline void log_free_text(char *dst, size_t dst_sz, const char *src)
{
    size_t n = 0;
    if (!dst || dst_sz == 0) return;
    while (src && *src == ' ') src++;
    for (; src && *src; src++) {
        unsigned char c = (unsigned char)*src;
        if (c < 0x20 || c == 0x7F) continue;
        if (c == '<' || c == '>') c = ' ';
        if (n + 1 >= dst_sz) break;
        dst[n++] = (char)c;
    }
    // If the text ends inside a multi-byte character, drop that character.
    if (n > 0) {
        size_t k = n - 1;
        while (k > 0 && ((unsigned char)dst[k] & 0xC0) == 0x80) k--;   // back to its first byte
        unsigned char lead = (unsigned char)dst[k];
        size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
        if (lead >= 0x80 && n - k < need) n = k;
    }
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}
