/* JS8 free-text ENCODING (J8) - the pack direction of js8_jsc.c.
 *
 * The decoder in js8_jsc.c was validated against real JS8Call transmissions
 * off air, so a text -> frame -> text round trip through it is an outside
 * check, not self-agreement. That is the whole reason this is written second.
 *
 * Frame layout, the same one js8_jsc_frame_to_text() reads, stated in terms of
 * the 72 bits of a JS8 frame:
 *
 *     bit 0        1 = this is a data frame
 *     bit 1        coding: 0 = per-character Huffman, 1 = JSC word coding
 *     bits 2..     content, then a single 0, then all 1s to the end
 *
 * So the content can be at most 69 bits: 72 - 1 flag - 1 coding - 1 pad zero.
 *
 * ⛔ THE PAD ZERO IS NOT OPTIONAL. The decoder finds the end of the content by
 * seeking back to the LAST ZERO, so a frame filled to the brim with no pad bit
 * would have its final content bit eaten. The fill loops below therefore stop
 * STRICTLY before the limit, exactly as the reference encoder does.
 *
 * WHICH CODING. JS8Call picks, per frame, whichever packs MORE characters, and
 * so does this: both are tried and the better one is kept. A real transmission
 * mixes them, which is why both directions had to work before this was useful.
 *
 * ⚠ WHAT WE CANNOT SEND, and it is a smaller set than it looks. Our word table
 * is truncated at kJS8_JSC_words (51667 of the protocol's 262144), so a word
 * outside it cannot be word-coded here - but it is NOT lost: the Huffman path
 * spells any word out character by character, and the receiver has the full
 * list either way. The cost of a rare word is air time, not intelligibility.
 */

#include "js8_jsc.h"
#include "js8_message.h"

#include <ctype.h>
#include <string.h>

#define JSC_B   4
#define JSC_S   7
#define JSC_C   9
#define JSC_INDEX_SPACE 262144u

#define FRAME_BITS     72
#define CONTENT_MAX    (FRAME_BITS - 1 - 1 - 1)   /* 69 - see the note above */

/* ---- the alphabet ---------------------------------------------------- */

/* The Huffman table is the authority on what can be sent: a character with no
 * code has no representation in either coding, because word coding can only
 * emit words that are themselves made of codeable characters. Returns the
 * entry, or NULL. */
static const js8_huff_entry_t* huff_lookup(char c)
{
    char up = (char)toupper((unsigned char)c);
    for (uint32_t k = 0; k < kJS8_Huff_count; k++) {
        const char* ch = kJS8_Huff[k].ch;
        if (ch[0] == up && ch[1] == '\0') return &kJS8_Huff[k];
    }
    return NULL;
}

int js8_jsc_normalize(const char* in, char* out, size_t out_len)
{
    if (!in || !out || out_len == 0) return -1;
    size_t n = 0;
    int last_space = 1;             /* leading spaces are dropped */
    for (const char* p = in; *p && n + 1 < out_len; p++) {
        char c = (char)toupper((unsigned char)*p);
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        if (c == ' ') {
            if (last_space) continue;      /* collapse runs */
            last_space = 1;
        } else {
            if (!huff_lookup(c)) continue; /* unsendable - drop it */
            last_space = 0;
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;   /* and trailing ones */
    out[n] = '\0';
    return (int)n;
}

/* ---- word lookup, the expensive direction ----------------------------- */

/* The blob is ordered by word frequency, not alphabetically, so there is no
 * binary search to be had. Rather than scan it once per word, resolve EVERY
 * word of the candidate text in a single pass: the blob is 312 KB and a frame
 * holds at most a handful of words, so one pass costs far less than a dozen.
 *
 * `words[i]` that is not found keeps index JSC_NOT_FOUND. */
#define JSC_NOT_FOUND  0xFFFFFFFFu

static void resolve_words(const char* const* words, int n_words, uint32_t* idx)
{
    for (int i = 0; i < n_words; i++) idx[i] = JSC_NOT_FOUND;

    uint32_t off = 0, index = 0;
    while (off < kJS8_JSC_blob_len && index < kJS8_JSC_words) {
        const char* w = &kJS8_JSC_blob[off];
        size_t len = strlen(w);
        for (int i = 0; i < n_words; i++) {
            if (idx[i] != JSC_NOT_FOUND) continue;
            if (strcmp(words[i], w) == 0) idx[i] = index;
        }
        off += (uint32_t)len + 1;
        index++;
    }
}

/* Emit index `j` as its (s,c)-dense symbol run. Returns symbols written, or
 * -1 if the index is out of range. Mirrors pass 2 of js8_jsc_decode_words():
 * k continuation symbols (>= S) closed by one terminal (< S), with the run
 * length k selecting the base offset. */
static int jsc_symbols_for_index(uint32_t j, uint8_t* sym, int max_sym)
{
    if (j >= JSC_INDEX_SPACE) return -1;

    uint32_t base[8];
    base[0] = 0;
    base[1] = JSC_S;
    for (int k = 2; k < 8; k++) {
        uint32_t span = JSC_S;
        for (int m = 1; m < k; m++) span *= JSC_C;
        base[k] = base[k - 1] + span;
    }

    int k = 0;
    while (k + 1 < 8 && j >= base[k + 1]) k++;
    if (k + 1 > max_sym) return -1;

    uint32_t v = j - base[k];
    uint8_t term = (uint8_t)(v % JSC_S);
    v /= JSC_S;

    /* The k continuation digits come out least-significant first and are
     * emitted most-significant first, so they are filled in backwards. */
    for (int i = k - 1; i >= 0; i--) {
        sym[i] = (uint8_t)(JSC_S + (v % JSC_C));
        v /= JSC_C;
    }
    sym[k] = term;
    return k + 1;
}

/* ---- the two codings -------------------------------------------------- */

/* Both return the number of CHARACTERS of `text` consumed, and write their
 * bits to `bits` with *n_bits set. Neither writes the pad; the caller does,
 * because the rule is shared. */

static int pack_huff(const char* text, uint8_t* bits, int* n_bits)
{
    int used = 0, chars = 0;
    for (const char* p = text; *p; p++) {
        const js8_huff_entry_t* e = huff_lookup(*p);
        if (!e) continue;
        int len = (int)strlen(e->code);
        /* STRICTLY less - the pad zero must still fit. */
        if (used + len >= CONTENT_MAX + 1) break;
        for (int b = 0; b < len; b++) bits[used + b] = (uint8_t)(e->code[b] == '1');
        used += len;
        chars++;
    }
    *n_bits = used;
    return chars;
}

#define JSC_MAX_WORDS 16

#define JSC_MAX_WORD_LEN 32

static int pack_words(const char* text, uint8_t* bits, int* n_bits)
{
    /* Split into words. The separator is carried OUTSIDE the index, as one
     * extra bit after each terminal symbol, so the spaces are recorded here
     * rather than encoded.
     *
     * ⛔ TOKENISE THE ORIGINAL STRING, NEVER A TRUNCATED COPY. This used to
     * memcpy into a 96-byte buffer and take "a space follows" from that copy.
     * At the cut the flag was wrong in BOTH directions - a word ending exactly
     * at the truncation lost its space, and the accounting then put the next
     * frame one character out, which added one elsewhere. The round-trip
     * harness caught it as 'LONGERMESSAGE' and 'THEREFORE  HAS'. The flag is a
     * fact about the message, so it has to be read from the message. */
    char    wbuf[JSC_MAX_WORDS][JSC_MAX_WORD_LEN];
    const char* wptr[JSC_MAX_WORDS];
    uint8_t wsep[JSC_MAX_WORDS];
    int     wlen[JSC_MAX_WORDS];
    int n_words = 0;

    size_t tl = strlen(text);
    for (size_t i = 0; i < tl && n_words < JSC_MAX_WORDS; ) {
        while (i < tl && text[i] == ' ') i++;
        if (i >= tl) break;
        size_t s = i;
        while (i < tl && text[i] != ' ') i++;
        size_t len = i - s;
        if (len >= JSC_MAX_WORD_LEN) break;   /* too long to word-code */
        memcpy(wbuf[n_words], text + s, len);
        wbuf[n_words][len] = '\0';
        wptr[n_words] = wbuf[n_words];
        wlen[n_words] = (int)len;
        wsep[n_words] = (uint8_t)(i < tl && text[i] == ' ');
        n_words++;
    }
    if (!n_words) { *n_bits = 0; return 0; }

    uint32_t idx[JSC_MAX_WORDS];
    resolve_words(wptr, n_words, idx);

    int used = 0, chars = 0;
    for (int w = 0; w < n_words; w++) {
        if (idx[w] == JSC_NOT_FOUND) break;    /* not in our table - stop here */

        uint8_t sym[12];
        int ns = jsc_symbols_for_index(idx[w], sym, (int)sizeof(sym));
        if (ns < 0) break;

        int need = ns * JSC_B + 1;             /* + the separator bit */
        if (used + need >= CONTENT_MAX + 1) break;

        for (int s = 0; s < ns; s++)
            for (int b = 0; b < JSC_B; b++)
                bits[used + s * JSC_B + b] =
                    (uint8_t)((sym[s] >> (JSC_B - 1 - b)) & 1u);
        used += ns * JSC_B;
        bits[used++] = wsep[w];

        chars += wlen[w] + (wsep[w] ? 1 : 0);
    }
    *n_bits = used;
    return chars;
}

/* ---- the whole job ---------------------------------------------------- */

int js8_jsc_text_to_frame(const char* text, uint8_t frame[9])
{
    if (!text || !frame) return -1;
    if (!text[0]) return 0;

    uint8_t hb[FRAME_BITS], wb[FRAME_BITS];
    int hn = 0, wn = 0;
    int hc = pack_huff(text, hb, &hn);

    /* ⛔ A FRAME THAT STARTS AT A SPACE MUST USE HUFFMAN.
     *
     * Word coding carries the separator as a bit AFTER each word, so it has
     * no way to express a space BEFORE the first one - pack_words() skips a
     * leading space, and the space is then simply gone from the message.
     *
     * MEASURED by the round-trip harness: encoding a two-line message
     * produced 'THIS IS A LONGERMESSAGE ...' (the boundary space eaten) and
     * '... THEREFORE  HAS ...' (the knock-on put the next boundary on a space,
     * which the previous frame's separator bit had already emitted). Both
     * disappear once the leading space is carried, and Huffman has a code for
     * it - two bits, the shortest in the table.
     *
     * The cost is one frame occasionally not word-coded. The alternative is a
     * message whose words run together, which is worse than slightly more air
     * time. */
    int wc = 0;
    if (text[0] != ' ') wc = pack_words(text, wb, &wn);

    /* More characters wins. On a tie prefer Huffman: it always makes progress,
     * where word coding can stop at the first word we do not carry and would
     * then loop forever on the same text. */
    const uint8_t* src = hb;
    int n_src = hn, consumed = hc, compressed = 0;
    if (wc > hc) { src = wb; n_src = wn; consumed = wc; compressed = 1; }

    if (consumed <= 0 || n_src <= 0) return -1;   /* nothing encodable */

    uint8_t bits[FRAME_BITS];
    bits[0] = 1;                                  /* data frame */
    bits[1] = (uint8_t)compressed;
    memcpy(&bits[2], src, (size_t)n_src);
    int pos = 2 + n_src;
    bits[pos++] = 0;                              /* the pad zero */
    while (pos < FRAME_BITS) bits[pos++] = 1;

    memset(frame, 0, 9);
    for (int i = 0; i < FRAME_BITS; i++)
        if (bits[i]) frame[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));

    return consumed;
}

int js8_jsc_text_to_frames(const char* text, uint8_t* frames, uint8_t* itypes,
                           int max_frames)
{
    if (!text || !frames || !itypes || max_frames <= 0) return -1;

    char norm[JS8_JSC_TEXT_MAX * 4];
    if (js8_jsc_normalize(text, norm, sizeof(norm)) <= 0) return 0;

    int n = 0;
    const char* p = norm;
    while (*p && n < max_frames) {
        int used = js8_jsc_text_to_frame(p, frames + (size_t)n * 9);
        /* ⛔ A frame that consumes NOTHING would spin here forever. It can
         * only happen if the leading character has no Huffman code, which
         * normalize() already removed - so this is a backstop, not a path. */
        if (used <= 0) break;
        p += used;
        n++;
    }
    if (n <= 0) return n;

    /* ⛔ THE FLAGS ARE WHAT MAKES THE FAR END JOIN THE FRAMES. Set after the
     * fact, because only now is it known which frame is last - exactly the
     * order JS8Call's own sender works in (it clears FIRST once a frame has
     * gone out and sets LAST when its queue empties).
     *
     * A one-frame message therefore gets FIRST|LAST, not 0; 0 means a middle
     * frame and would leave the receiver waiting for an end. */
    for (int i = 0; i < n; i++) itypes[i] = JS8_ITYPE_MIDDLE;
    itypes[0]     |= JS8_ITYPE_FIRST;
    itypes[n - 1] |= JS8_ITYPE_LAST;
    return n;
}
