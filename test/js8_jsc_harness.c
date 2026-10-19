/* Host test for JS8 free-text decoding (J7) - js8_jsc.c and its tables.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -o js8_jsc_harness test/js8_jsc_harness.c \
 *       components/ft8_lib/ft8/js8_jsc.c \
 *       components/ft8_lib/ft8/js8_jsc_tables.c \
 *       -lm && ./js8_jsc_harness
 *
 * ⭐ THE VECTORS ARE HAND-COMPUTED FROM THE CODING RULES, not captured from
 * this implementation. That is the whole point: a round-trip against our own
 * encoder would prove only that we agree with ourselves, which is the trap
 * docs/js8-feasibility.md names as risk 1 for the whole JS8 effort.
 *
 * The (s,c)-dense scheme is b=4, s=7, c=9, giving
 *     base = [0, 7, 70, 637, 5740, 51667, 465010]
 * An index is a run of continuation symbols (>= 7) closed by one terminal
 * symbol (< 7); the run length k picks base[k]. Worked examples, checked
 * against jsc_map.cpp:
 *     index   0  -> symbols [0]           -> "E"
 *     index   7  -> symbols [7, 0]        -> "H"
 *     index  70  -> symbols [7, 7, 0]     -> "JS8"
 *     index 637  -> symbols [7, 7, 7, 0]  -> ":-P"
 * and a terminal symbol is followed by one extra bit which, when set, means a
 * space follows that word.
 *
 * ⛔ 51667 is base[5] exactly - the first index needing 24 bits. The table is
 * truncated there, so anything above it must render as a marker and must NOT
 * stop the decode. Both are tested.
 */
#include <stdio.h>
#include <string.h>

#include "js8_jsc.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

/* ---- building a frame, bit by bit ------------------------------------ */

typedef struct { uint8_t b[72]; int n; } bitbuf_t;

static void bb_bit(bitbuf_t* f, int v) { if (f->n < 72) f->b[f->n++] = (uint8_t)(v & 1); }

static void bb_sym(bitbuf_t* f, int v)      /* one 4-bit dense symbol */
{
    for (int i = 3; i >= 0; i--) bb_bit(f, (v >> i) & 1);
}

static void bb_code(bitbuf_t* f, const char* bits)   /* "1001" */
{
    for (const char* p = bits; *p; p++) bb_bit(f, *p == '1');
}

/* Close the frame the way the encoder does: one 0, then all 1s to 72. The
 * encoder always has room for that 0 - its fill loop stops strictly before
 * the frame width - which is why the decoder can seek back to the last 0. */
static void bb_pad_to_frame(bitbuf_t* f, uint8_t frame[9])
{
    bb_bit(f, 0);
    while (f->n < 72) bb_bit(f, 1);
    memset(frame, 0, 9);
    for (int i = 0; i < 72; i++)
        if (f->b[i]) frame[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
}

/* A data frame: is-data = 1, then the coding flag, then content. */
static void bb_start(bitbuf_t* f, int compressed)
{
    f->n = 0;
    bb_bit(f, 1);
    bb_bit(f, compressed);
}

/* ---- the tables themselves ------------------------------------------- */

static void test_tables_are_what_the_coding_expects(void)
{
    printf("tables\n");
    CHECK(kJS8_JSC_words == 51667,
          "truncation is base[5] = 51667, got %u\n", (unsigned)kJS8_JSC_words);
    CHECK(kJS8_Huff_count == 44, "44 Huffman entries, got %u\n",
          (unsigned)kJS8_Huff_count);

    /* Spot indices, each one a band boundary, read out of jsc_map.cpp by
     * hand. If the generator ever drops a line again these move. */
    struct { uint32_t i; const char* w; } spot[] = {
        { 0, "E" }, { 1, "T" }, { 6, "S" }, { 7, "H" }, { 8, "R" },
        { 70, "JS8" }, { 636, ":-)" }, { 637, ":-P" },
        { 5739, "LLAG" }, { 5740, "LINCOLN" }, { 51666, "ILSW" },
    };
    for (unsigned k = 0; k < sizeof(spot) / sizeof(spot[0]); k++) {
        const char* got = js8_jsc_word(spot[k].i);
        CHECK(got && !strcmp(got, spot[k].w), "word %u = '%s', expected '%s'\n",
              (unsigned)spot[k].i, got ? got : "(null)", spot[k].w);
    }

    /* The first index we deliberately do not carry. */
    CHECK(js8_jsc_word(51667) == NULL, "51667 is above the cut\n");
    CHECK(js8_jsc_word(262143) == NULL, "262143 is above the cut\n");

    /* ⚠ The blob must be valid UTF-8: 32 of the source entries are single
     * Latin-1 high bytes, and a lone 0xA1 would draw as garbage on an LVGL
     * label and break the JSON of /api/status. The generator re-encodes them,
     * so no byte in 0x80..0xBF may appear without a lead byte before it. */
    int bad_utf8 = 0;
    for (uint32_t i = 0; i < kJS8_JSC_blob_len; i++) {
        unsigned char c = (unsigned char)kJS8_JSC_blob[i];
        if (c >= 0x80 && c < 0xC0) {
            unsigned char prev = i ? (unsigned char)kJS8_JSC_blob[i - 1] : 0;
            if (prev < 0xC0) bad_utf8++;
        }
    }
    CHECK(bad_utf8 == 0, "%d stray UTF-8 continuation bytes in the blob\n", bad_utf8);
}

/* ---- Huffman --------------------------------------------------------- */

static void test_huffman_frame(void)
{
    printf("Huffman frames [10x]\n");
    uint8_t frame[9];
    char out[JS8_JSC_TEXT_MAX];

    /* "E" = 100, "T" = 1101, " " = 01 - straight out of varicode.cpp's
     * hufftable. "ET" is therefore 100 1101. */
    bitbuf_t f; bb_start(&f, 0);
    bb_code(&f, "100");
    bb_code(&f, "1101");
    bb_pad_to_frame(&f, frame);
    CHECK(js8_jsc_frame_to_text(frame, out, sizeof(out)), "decode ET\n");
    CHECK(!strcmp(out, "ET"), "got '%s', expected 'ET'\n", out);

    /* With a space: "E T" = 100 01 1101. */
    bb_start(&f, 0);
    bb_code(&f, "100");
    bb_code(&f, "01");
    bb_code(&f, "1101");
    bb_pad_to_frame(&f, frame);
    CHECK(js8_jsc_frame_to_text(frame, out, sizeof(out)), "decode 'E T'\n");
    CHECK(!strcmp(out, "E T"), "got '%s', expected 'E T'\n", out);

    /* A longer one, so the pad is short and the walk is exercised:
     * "THE" = 1101 00011 100 ... wait, H is 00011 and E is 100. */
    bb_start(&f, 0);
    bb_code(&f, "1101");      /* T */
    bb_code(&f, "00011");     /* H */
    bb_code(&f, "100");       /* E */
    bb_pad_to_frame(&f, frame);
    CHECK(js8_jsc_frame_to_text(frame, out, sizeof(out)), "decode THE\n");
    CHECK(!strcmp(out, "THE"), "got '%s', expected 'THE'\n", out);
}

/* ---- word coding ----------------------------------------------------- */

static void test_word_frame(void)
{
    printf("word frames [11x]\n");
    uint8_t frame[9];
    char out[JS8_JSC_TEXT_MAX];

    /* index 70 = "JS8": symbols [7, 7, 0], terminal followed by a separator
     * bit. Separator 0 = no trailing space. */
    bitbuf_t f; bb_start(&f, 1);
    bb_sym(&f, 7); bb_sym(&f, 7); bb_sym(&f, 0); bb_bit(&f, 0);
    bb_pad_to_frame(&f, frame);
    CHECK(js8_jsc_frame_to_text(frame, out, sizeof(out)), "decode JS8\n");
    CHECK(!strcmp(out, "JS8"), "got '%s', expected 'JS8'\n", out);

    /* "JS8" + separator, then index 0 = "E". The space is carried by the bit
     * AFTER the terminal symbol, not by a word of its own. */
    bb_start(&f, 1);
    bb_sym(&f, 7); bb_sym(&f, 7); bb_sym(&f, 0); bb_bit(&f, 1);
    bb_sym(&f, 0); bb_bit(&f, 0);
    bb_pad_to_frame(&f, frame);
    CHECK(js8_jsc_frame_to_text(frame, out, sizeof(out)), "decode 'JS8 E'\n");
    CHECK(!strcmp(out, "JS8 E"), "got '%s', expected 'JS8 E'\n", out);

    /* index 7 = "H", a two-symbol run. */
    bb_start(&f, 1);
    bb_sym(&f, 7); bb_sym(&f, 0); bb_bit(&f, 0);
    bb_pad_to_frame(&f, frame);
    CHECK(js8_jsc_frame_to_text(frame, out, sizeof(out)), "decode H\n");
    CHECK(!strcmp(out, "H"), "got '%s', expected 'H'\n", out);

    /* index 637 = ":-P", a four-symbol run - and proof that k is read from
     * the run length rather than assumed. */
    bb_start(&f, 1);
    bb_sym(&f, 7); bb_sym(&f, 7); bb_sym(&f, 7); bb_sym(&f, 0); bb_bit(&f, 0);
    bb_pad_to_frame(&f, frame);
    CHECK(js8_jsc_frame_to_text(frame, out, sizeof(out)), "decode :-P\n");
    CHECK(!strcmp(out, ":-P"), "got '%s', expected ':-P'\n", out);
}

static void test_a_word_above_the_cut_does_not_stop_the_line(void)
{
    printf("a word above the truncation becomes a marker, not an ending\n");
    uint8_t frame[9];
    char out[JS8_JSC_TEXT_MAX];

    /* 51667 is base[5]: five continuation symbols then a terminal, all zero
     * relative, i.e. [7,7,7,7,7,0]. Then a separator and index 0 = "E".
     *
     * ⛔ THIS IS THE TEST THAT JUSTIFIES TRUNCATING AT ALL. If one unknown
     * word ended the decode, every line containing a rare word would be lost
     * and a 309 KB table would be worth far less than it looks. */
    bitbuf_t f; bb_start(&f, 1);
    bb_sym(&f, 7); bb_sym(&f, 7); bb_sym(&f, 7);
    bb_sym(&f, 7); bb_sym(&f, 7); bb_sym(&f, 0); bb_bit(&f, 1);
    bb_sym(&f, 0); bb_bit(&f, 0);
    bb_pad_to_frame(&f, frame);
    CHECK(js8_jsc_frame_to_text(frame, out, sizeof(out)), "decode with a gap\n");
    CHECK(!strcmp(out, JS8_JSC_UNKNOWN_WORD " E"),
          "got '%s', expected '%s E'\n", out, JS8_JSC_UNKNOWN_WORD);
}

/* ---- the frame envelope ---------------------------------------------- */

static void test_the_envelope(void)
{
    printf("envelope: flags and padding\n");
    uint8_t frame[9];
    char out[JS8_JSC_TEXT_MAX];

    /* A non-data frame must be refused outright - every other frame type
     * starts with a 0 and belongs to js8_text.c. */
    bitbuf_t f; f.n = 0;
    bb_bit(&f, 0); bb_bit(&f, 1); bb_bit(&f, 1);
    bb_code(&f, "100");
    bb_pad_to_frame(&f, frame);
    CHECK(!js8_jsc_frame_to_text(frame, out, sizeof(out)),
          "a frame starting 0 is not ours\n");

    /* All ones: no zero anywhere, so there is no pad marker and the frame
     * cannot be a padded data frame. Must refuse rather than decode noise. */
    memset(frame, 0xFF, sizeof(frame));
    CHECK(!js8_jsc_frame_to_text(frame, out, sizeof(out)),
          "an all-ones frame has no pad marker\n");

    /* unpad on its own: content then 0 then 1s. */
    uint8_t bits[10] = { 1, 0, 1, 1, 0, 1, 1, 1, 1, 1 };
    CHECK(js8_jsc_unpad(bits, 10) == 4,
          "last zero at 4, got %d\n", js8_jsc_unpad(bits, 10));
    uint8_t ones[4] = { 1, 1, 1, 1 };
    CHECK(js8_jsc_unpad(ones, 4) == -1, "no zero -> -1\n");
}

static void test_output_buffer_is_respected(void)
{
    printf("a short output buffer truncates instead of overrunning\n");
    uint8_t frame[9];
    char tiny[4];

    bitbuf_t f; bb_start(&f, 1);
    bb_sym(&f, 7); bb_sym(&f, 7); bb_sym(&f, 0); bb_bit(&f, 1);  /* "JS8 " */
    bb_sym(&f, 0); bb_bit(&f, 0);                                /* "E"    */
    bb_pad_to_frame(&f, frame);
    js8_jsc_frame_to_text(frame, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny), "stayed inside %u bytes: '%s'\n",
          (unsigned)sizeof(tiny), tiny);
    CHECK(!strcmp(tiny, "JS8"), "kept what fit: '%s'\n", tiny);
}

int main(void)
{
    printf("=== JS8 free-text (JSC) harness ===\n\n");
    test_tables_are_what_the_coding_expects();
    test_huffman_frame();
    test_word_frame();
    test_a_word_above_the_cut_does_not_stop_the_line();
    test_the_envelope();
    test_output_buffer_is_respected();
    printf("\n%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
