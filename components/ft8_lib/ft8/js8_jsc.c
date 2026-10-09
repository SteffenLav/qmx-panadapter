#include "js8_jsc.h"

#include <string.h>

/* JS8 free-text decoding. The algorithm follows JS8Call's unpackDataMessage(),
 * huffDecode() and JSC::decompress() (GPL-3, Jordan Sherer KN4CRD); the
 * tables are generated from that source by tools/gen_js8_jsc_tables.py. This
 * is a C rewrite rather than a port of the Qt original - the shapes are
 * different enough (QVector<bool>, QMap, QString) that a transliteration
 * would have been worse code - but every constant here came from reading it,
 * not from guessing. */

/* ---- word lookup ----------------------------------------------------- */

const char* js8_jsc_word(uint32_t index)
{
    if (index >= kJS8_JSC_words) return NULL;      /* above the truncation */

    uint32_t blk = index / kJS8_JSC_stride;
    uint32_t off = kJS8_JSC_index[blk];
    uint32_t skip = index - blk * kJS8_JSC_stride;

    /* Skip at most stride-1 NUL-terminated strings. Bounded by blob_len so a
     * corrupted index cannot walk off the array. */
    while (skip--) {
        while (off < kJS8_JSC_blob_len && kJS8_JSC_blob[off] != '\0') off++;
        if (off >= kJS8_JSC_blob_len) return NULL;
        off++;
    }
    if (off >= kJS8_JSC_blob_len) return NULL;
    return &kJS8_JSC_blob[off];
}

/* ---- padding --------------------------------------------------------- */

/* The encoder appends a single 0 and then all 1s, and it is guaranteed to
 * have room for at least that one 0: packHuffMessage()'s fill loop stops
 * while `length + charBits < frameSize` STRICTLY, so the content never
 * reaches the full width and pad is always >= 1. That is why seeking back to
 * the last 0 is safe and cannot eat a content bit. */
int js8_jsc_unpad(const uint8_t* bits, int n_bits)
{
    if (!bits || n_bits <= 0) return -1;
    for (int i = n_bits - 1; i >= 0; i--)
        if (!bits[i]) return i;        /* content is [0, i) */
    return -1;                         /* no 0 at all: not a padded frame */
}

/* ---- Huffman --------------------------------------------------------- */

int js8_jsc_decode_huff(const uint8_t* bits, int n_bits,
                        char* out, size_t out_len)
{
    if (!bits || !out || out_len == 0) return -1;
    out[0] = '\0';

    int pos = 0, n = 0;
    while (pos < n_bits) {
        /* The code is prefix-free (the generator refuses a table that is
         * not), so at most one entry can match here and the search order does
         * not matter. JS8Call's own loop keeps scanning the remaining keys
         * after a match - its author called it naive - which is equivalent
         * for a prefix-free code. */
        const js8_huff_entry_t* hit = NULL;
        int hit_len = 0;
        for (uint32_t k = 0; k < kJS8_Huff_count; k++) {
            const char* code = kJS8_Huff[k].code;
            int len = (int)strlen(code);
            if (len > n_bits - pos) continue;
            int ok = 1;
            for (int b = 0; b < len; b++) {
                if (bits[pos + b] != (uint8_t)(code[b] == '1')) { ok = 0; break; }
            }
            if (ok) { hit = &kJS8_Huff[k]; hit_len = len; break; }
        }
        /* No match means the remaining bits are not a whole code - the tail
         * of a truncated frame. Stop and keep what was read, which is what
         * the reference does. */
        if (!hit) break;

        size_t cl = strlen(hit->ch);
        if ((size_t)n + cl + 1 > out_len) break;
        memcpy(out + n, hit->ch, cl);
        n += (int)cl;
        out[n] = '\0';
        pos += hit_len;
    }
    return n;
}

/* ---- JSC word coding ------------------------------------------------- */

/* (s,c)-dense coding: b=4 bits per symbol, s=7 terminal values, c=9
 * continuation values. An index is a run of continuation symbols (>= s)
 * closed by one terminal symbol (< s), and the run length k selects a base
 * offset. These five numbers are the whole scheme. */
#define JSC_B   4
#define JSC_S   7
#define JSC_C   9           /* (1 << JSC_B) - JSC_S */
/* The protocol's full index space, which is NOT our table size. An index
 * between kJS8_JSC_words and this is a legitimate word we simply do not
 * carry; at or above this the frame is malformed. Conflating the two would
 * either throw away readable text or print rubbish. */
#define JSC_INDEX_SPACE 262144u

#define JSC_MAX_SYMBOLS (70 / JSC_B + 2)

int js8_jsc_decode_words(const uint8_t* bits, int n_bits,
                         char* out, size_t out_len)
{
    if (!bits || !out || out_len == 0) return -1;
    out[0] = '\0';

    uint32_t base[8];
    base[0] = 0;
    base[1] = JSC_S;
    for (int k = 2; k < 8; k++) {
        uint32_t span = JSC_S;
        for (int m = 1; m < k; m++) span *= JSC_C;
        base[k] = base[k - 1] + span;
    }

    /* Pass 1: 4-bit symbols. A terminal symbol (< s) is followed by ONE extra
     * bit; when that bit is set, a space follows this word. The separator is
     * therefore carried outside the index, which is why it has to be
     * collected here rather than discovered during decoding. */
    uint8_t sym[JSC_MAX_SYMBOLS];
    uint8_t sep[JSC_MAX_SYMBOLS];
    int n_sym = 0;
    memset(sep, 0, sizeof(sep));

    int i = 0;
    while (i + JSC_B <= n_bits && n_sym < JSC_MAX_SYMBOLS) {
        uint32_t v = 0;
        for (int b = 0; b < JSC_B; b++) v = (v << 1) | (bits[i + b] & 1u);
        i += JSC_B;
        sym[n_sym] = (uint8_t)v;
        if (v < JSC_S) {
            if (i < n_bits && bits[i]) sep[n_sym] = 1;
            i += 1;                  /* the separator bit is always consumed */
        }
        n_sym++;
    }

    /* Pass 2: symbols to indices to words. */
    int n = 0;
    int start = 0;
    while (start < n_sym) {
        int k = 0;
        uint32_t j = 0;
        while (start + k < n_sym && sym[start + k] >= JSC_S) {
            j = j * JSC_C + (sym[start + k] - JSC_S);
            k++;
            if (k >= 8) break;       /* base[] has eight entries */
        }
        if (k >= 8) break;
        if (j >= JSC_INDEX_SPACE) break;
        if (start + k >= n_sym) break;          /* run with no terminator */
        j = j * JSC_S + sym[start + k] + base[k];
        if (j >= JSC_INDEX_SPACE) break;        /* malformed, stop */

        const char* word = js8_jsc_word(j);
        /* ⭐ A legitimate index above our truncation point becomes a marker,
         * NOT a reason to stop. One rare word must not cost the rest of the
         * sentence - that is the whole point of truncating at a band
         * boundary rather than refusing to decode. */
        const char* piece = word ? word : JS8_JSC_UNKNOWN_WORD;

        size_t pl = strlen(piece);
        if ((size_t)n + pl + 1 > out_len) break;
        memcpy(out + n, piece, pl);
        n += (int)pl;
        out[n] = '\0';

        if (sep[start + k]) {
            if ((size_t)n + 2 > out_len) break;
            out[n++] = ' ';
            out[n] = '\0';
        }
        start += k + 1;
    }
    return n;
}

/* ---- the whole frame ------------------------------------------------- */

bool js8_jsc_frame_to_text(const uint8_t frame[9], char* out, size_t out_len)
{
    if (!frame || !out || out_len == 0) return false;
    out[0] = '\0';

    /* 72 bits, MSB first, exactly as js8_message.c reads every other frame. */
    uint8_t bits[72];
    for (int i = 0; i < 72; i++)
        bits[i] = (uint8_t)((frame[i / 8] >> (7 - (i % 8))) & 1u);

    if (!bits[0]) return false;                 /* not a data frame */

    /* ⛔ THE OFFSETS HERE ARE THE EASIEST THING TO GET WRONG, so they are
     * spelled out. The reference drops bit 0, then takes bits[1 .. n-1] of
     * what remains, where n is the index of the LAST ZERO in that remainder -
     * so the coding flag at index 0 of the remainder is skipped and the pad
     * marker at n ends it. In this array that is content = bits[2 .. n] with
     * n measured from index 1. */
    const uint8_t* rest = &bits[1];             /* 71 bits */
    int n_rest = 71;
    bool compressed = rest[0] != 0;

    int last_zero = js8_jsc_unpad(rest, n_rest);
    if (last_zero <= 0) return false;           /* no pad, or pad at the flag */

    const uint8_t* content = &rest[1];
    int n_content = last_zero - 1;
    if (n_content <= 0) return false;

    int n = compressed
          ? js8_jsc_decode_words(content, n_content, out, out_len)
          : js8_jsc_decode_huff(content, n_content, out, out_len);
    return n > 0;
}
