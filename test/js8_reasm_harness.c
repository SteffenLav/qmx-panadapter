/* Host test for js8_reasm.c and the frame-type reader it depends on.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I components/ft8_lib -I components/ft8_lib/ft8 \
 *       -o js8_reasm_harness test/js8_reasm_harness.c \
 *       components/ft8_lib/ft8/js8_reasm.c \
 *       components/ft8_lib/ft8/js8_jsc.c \
 *       components/ft8_lib/ft8/js8_jsc_tables.c \
 *       components/ft8_lib/ft8/js8_message.c \
 *       components/ft8_lib/ft8/js8_codec.c \
 *       components/ft8_lib/ft8/js8_tables.c \
 *       components/ft8_lib/ft8/text.c \
 *       -lm && ./js8_reasm_harness
 *
 * The risks docs/js8-feasibility.md names for J7 are exactly the cases below:
 * a stuck partial message, a sender who vanishes mid-transmission, and two
 * senders interleaving. All three are reachable without a radio, which is why
 * the state machine is a separate module rather than code inside ft8_test.c.
 *
 * ⚠ TEXT DECODING MOVED. When this harness was written the tables were out of
 * reach on licence grounds, so the module could only count frames. The
 * project relicensed to GPL-3 on 2026-10-09 and js8_reasm.c now decodes each
 * frame on arrival (js8_jsc.c). The frames below are synthetic - their
 * payloads are arbitrary fill - so they decode to nothing, which is exactly
 * what keeps these tests about WHO, HOW MANY and WHEN. The text path has its
 * own vectors in test/js8_jsc_harness.c, hand-computed from the coding rules.
 */
#include <stdio.h>
#include <string.h>

#include "js8_reasm.h"

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (!(cond)) { g_fail++;                        \
        printf("  FAIL: "); printf(__VA_ARGS__);    \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

/* A frame whose top bits say `type`. The remaining bits are arbitrary: this
 * module never looks at the payload, and a test that fed it real packed text
 * would be testing the codec instead. */
static void mk(uint8_t f[JS8_FRAME_BYTES], unsigned top3, uint8_t fill)
{
    memset(f, fill, JS8_FRAME_BYTES);
    f[0] = (uint8_t)((f[0] & 0x1Fu) | ((top3 & 7u) << 5));
}

/* A data frame that CANNOT decode to text: every bit set, so there is no pad
 * zero to seek back to and js8_jsc_frame_to_text refuses it.
 *
 * ⚠ This exists because an earlier version of this file asserted that
 * arbitrary fill decodes to nothing. It does not - 0xBB decodes as perfectly
 * good Huffman garbage (" N TTTTTTTT..."), which broke four tests the moment
 * text decoding was wired in. Tests about WHO and HOW MANY use this; the text
 * path has its own hand-computed vectors in test/js8_jsc_harness.c. */
static void mk_notext(uint8_t f[JS8_FRAME_BYTES], unsigned top3)
{
    memset(f, 0xFF, JS8_FRAME_BYTES);
    f[0] = (uint8_t)((f[0] & 0x1Fu) | ((top3 & 7u) << 5));
}

/* ---- the frame-type reader ------------------------------------------- */

static void test_frame_type_folds_the_data_frames(void)
{
    printf("frame type: data frames use two bits, not three\n");
    uint8_t f[JS8_FRAME_BYTES];

    /* 000..011 are three-bit types and must come back exactly. */
    mk(f, 0, 0x00); CHECK(js8_frame_type(f) == JS8_FRAME_HEARTBEAT, "000 -> heartbeat\n");
    mk(f, 1, 0x00); CHECK(js8_frame_type(f) == JS8_FRAME_COMPOUND, "001 -> compound\n");
    mk(f, 2, 0x00); CHECK(js8_frame_type(f) == JS8_FRAME_COMPOUND_DIRECTED, "010 -> compound directed\n");
    mk(f, 3, 0x00); CHECK(js8_frame_type(f) == JS8_FRAME_DIRECTED, "011 -> directed\n");

    /* ⭐ THE BUG THIS CATCHES. 100 and 101 are one type and 110 and 111 are
     * one type, because the third bit is payload. Reading all three gave
     * phantom types 5 and 7, and would have split one sender's frames across
     * two buckets on the value of a message bit. */
    mk(f, 4, 0x00); CHECK(js8_frame_type(f) == JS8_FRAME_DATA, "100 -> data\n");
    mk(f, 5, 0x00); CHECK(js8_frame_type(f) == JS8_FRAME_DATA,
                          "101 -> data (got %d)\n", (int)js8_frame_type(f));
    mk(f, 6, 0x00); CHECK(js8_frame_type(f) == JS8_FRAME_DATA_COMPRESSED, "110 -> data compressed\n");
    mk(f, 7, 0x00); CHECK(js8_frame_type(f) == JS8_FRAME_DATA_COMPRESSED,
                          "111 -> data compressed (got %d)\n", (int)js8_frame_type(f));

    /* The payload bit must not leak into the answer either way round. */
    mk(f, 5, 0xFF); CHECK(js8_frame_type(f) == JS8_FRAME_DATA, "101 with payload ones\n");
    mk(f, 7, 0xFF); CHECK(js8_frame_type(f) == JS8_FRAME_DATA_COMPRESSED, "111 with payload ones\n");

    CHECK(js8_frame_is_data(JS8_FRAME_DATA), "is_data(DATA)\n");
    CHECK(js8_frame_is_data(JS8_FRAME_DATA_COMPRESSED), "is_data(DATA_COMPRESSED)\n");
    CHECK(!js8_frame_is_data(JS8_FRAME_DIRECTED), "!is_data(DIRECTED)\n");
    CHECK(!js8_frame_is_data(JS8_FRAME_HEARTBEAT), "!is_data(HEARTBEAT)\n");
}

/* ---- attribution ------------------------------------------------------ */

static void test_a_run_takes_its_name_from_an_earlier_frame(void)
{
    printf("attribution: the callsign comes from a frame in an EARLIER slot\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dir[JS8_FRAME_BYTES], dat[JS8_FRAME_BYTES];
    mk(dir, 3, 0x11);
    mk_notext(dat, 4);

    /* Slot 0: a directed frame identifies the station. Not stored, but it is
     * the only thing that can name what follows. */
    CHECK(!js8_reasm_add(&r, 1000, 1200, dir, JS8_FRAME_DIRECTED, "OZ9JEP"),
          "a directed frame is not part of a data run\n");
    CHECK(js8_reasm_active(&r) == 0, "and starts no run\n");

    /* Slots 1-3: free text on the same offset. */
    for (int i = 1; i <= 3; i++)
        CHECK(js8_reasm_add(&r, 1000 + 15 * i, 1203, dat, JS8_FRAME_DATA, NULL),
              "data frame %d taken\n", i);

    CHECK(js8_reasm_active(&r) == 1, "one run (got %d)\n", js8_reasm_active(&r));
    const js8_reasm_run_t* run = js8_reasm_at(&r, 0);
    CHECK(run && run->n_seen == 3, "3 frames seen\n");
    CHECK(run && !strcmp(run->sender, "OZ9JEP"), "named '%s'\n",
          run ? run->sender : "(null)");

    char line[64];
    CHECK(js8_reasm_describe(run, line, sizeof(line)), "describe\n");
    CHECK(!strcmp(line, "OZ9JEP [data 3f]"), "line '%s'\n", line);
}

/* ⭐ THE ON-AIR CASE, 2026-10-10. A real station called CQ on 1494 Hz; our own
 * anonymous free text on 1506 Hz was displayed as "OZ7TKM: STATUSCHK". The
 * operator said "could be anybody" and was right. 12 Hz is inside the 15 Hz
 * JOIN tolerance but outside the 8 Hz ATTRIBUTION tolerance, which is the
 * whole reason the two are now separate numbers. */
static void test_a_distant_identification_does_not_name_our_run(void)
{
    printf("attribution: 12 Hz away is NOT close enough to borrow a callsign\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dir[JS8_FRAME_BYTES], dat[JS8_FRAME_BYTES];
    mk(dir, 3, 0x11);
    mk_notext(dat, 4);

    /* OZ7TKM identifies on 1494 Hz. */
    js8_reasm_add(&r, 3000, 1494, dir, JS8_FRAME_DIRECTED, "OZ7TKM");
    /* Somebody else's free text lands on 1506 Hz - 12 Hz away. */
    CHECK(js8_reasm_add(&r, 3015, 1506, dat, JS8_FRAME_DATA, NULL),
          "the data frame is still taken\n");

    const js8_reasm_run_t* run = js8_reasm_at(&r, 0);
    CHECK(run && !run->sender[0], "run must stay UNIDENTIFIED, got '%s'\n",
          run ? run->sender : "(null)");

    char line[64];
    js8_reasm_describe(run, line, sizeof(line));
    CHECK(!strcmp(line, "1506 Hz [data 1f]"), "reported by offset, got '%s'\n", line);
}

/* Two different stations both inside the attribution tolerance. No guess is
 * honest, so the run stays unidentified rather than picking whichever sat
 * earlier in the array - which is exactly what the old first-match did. */
static void test_two_candidate_callsigns_name_nobody(void)
{
    printf("attribution: two plausible callsigns -> no sender, not a coin toss\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dir[JS8_FRAME_BYTES], dat[JS8_FRAME_BYTES];
    mk(dir, 3, 0x11);
    mk_notext(dat, 4);

    js8_reasm_add(&r, 4000, 1496, dir, JS8_FRAME_DIRECTED, "OZ7TKM");
    js8_reasm_add(&r, 4000, 1502, dir, JS8_FRAME_DIRECTED, "OZ2LAV");
    js8_reasm_add(&r, 4015, 1500, dat, JS8_FRAME_DATA, NULL);

    const js8_reasm_run_t* run = js8_reasm_at(&r, 0);
    CHECK(run && !run->sender[0], "ambiguous -> unidentified, got '%s'\n",
          run ? run->sender : "(null)");
}

/* The nearest of two identifications wins - the same rule find_run() already
 * used, and the one lookup_ident() was missing. Same call twice is NOT
 * ambiguous: one station whose offset moved. */
static void test_nearest_identification_wins(void)
{
    printf("attribution: nearest identification wins, repeats are not ambiguous\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dir[JS8_FRAME_BYTES], dat[JS8_FRAME_BYTES];
    mk(dir, 3, 0x11);
    mk_notext(dat, 4);

    /* Same station seen twice, 6 Hz apart - its offset drifted one bin. */
    js8_reasm_add(&r, 5000, 1494, dir, JS8_FRAME_DIRECTED, "OZ2LAV");
    js8_reasm_add(&r, 5000, 1500, dir, JS8_FRAME_DIRECTED, "OZ2LAV");
    js8_reasm_add(&r, 5015, 1499, dat, JS8_FRAME_DATA, NULL);

    const js8_reasm_run_t* run = js8_reasm_at(&r, 0);
    CHECK(run && !strcmp(run->sender, "OZ2LAV"), "named '%s'\n",
          run ? run->sender : "(null)");
}

static void test_unidentified_run_says_the_frequency(void)
{
    printf("attribution: with no callsign the run is reported by offset\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dat[JS8_FRAME_BYTES]; mk_notext(dat, 6);

    js8_reasm_add(&r, 2000, 1500, dat, JS8_FRAME_DATA_COMPRESSED, NULL);
    const js8_reasm_run_t* run = js8_reasm_at(&r, 0);
    CHECK(run && !run->sender[0], "no sender\n");
    CHECK(run && run->compressed, "compressed coding recorded\n");

    char line[64];
    js8_reasm_describe(run, line, sizeof(line));
    /* ⚠ Deliberately not message-shaped: it must not read as text withheld. */
    CHECK(!strcmp(line, "1500 Hz [data 1f]"), "line '%s'\n", line);
}

/* ---- the three risks the plan named ----------------------------------- */

static void test_a_sender_who_vanishes_is_closed_not_stuck(void)
{
    printf("risk: a sender vanishing mid-transmission must not stick\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dat[JS8_FRAME_BYTES]; mk(dat, 4, 0x44);

    js8_reasm_add(&r, 3000, 1000, dat, JS8_FRAME_DATA, NULL);
    js8_reasm_add(&r, 3015, 1000, dat, JS8_FRAME_DATA, NULL);
    CHECK(js8_reasm_active(&r) == 1, "open after two frames\n");

    /* A pause shorter than the timeout keeps it: a slot lost to QSB is not
     * the end of a message, and closing on the first gap shreds real ones. */
    js8_reasm_tick(&r, 3045);
    CHECK(js8_reasm_active(&r) == 1, "still open after 2 quiet slots\n");

    js8_reasm_tick(&r, 3015 + JS8_REASM_TIMEOUT_SLOTS * 15);
    CHECK(js8_reasm_active(&r) == 0, "closed at the timeout (active=%d)\n",
          js8_reasm_active(&r));
}

static void test_two_senders_interleaving_stay_separate(void)
{
    printf("risk: two senders interleaving must not merge\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t a[JS8_FRAME_BYTES], b[JS8_FRAME_BYTES];
    mk(a, 4, 0x55);
    mk(b, 6, 0x66);

    js8_reasm_add(&r, 4000,  800, a, JS8_FRAME_DATA, "G0ABC");
    js8_reasm_add(&r, 4000, 1600, b, JS8_FRAME_DATA_COMPRESSED, "M0XYZ");
    /* Same slot, two offsets: interleaved in time, separate in frequency. */
    js8_reasm_add(&r, 4015,  805, a, JS8_FRAME_DATA, NULL);
    js8_reasm_add(&r, 4015, 1595, b, JS8_FRAME_DATA_COMPRESSED, NULL);
    js8_reasm_add(&r, 4030,  800, a, JS8_FRAME_DATA, NULL);

    CHECK(js8_reasm_active(&r) == 2, "two runs (got %d)\n", js8_reasm_active(&r));
    int seen800 = 0, seen1600 = 0;
    for (int i = 0; i < js8_reasm_active(&r); i++) {
        const js8_reasm_run_t* run = js8_reasm_at(&r, i);
        if (run->freq_hz < 1200) seen800  = run->n_seen;
        else                     seen1600 = run->n_seen;
    }
    CHECK(seen800 == 3, "low offset has 3 frames (got %d)\n", seen800);
    CHECK(seen1600 == 2, "high offset has 2 frames (got %d)\n", seen1600);
}

static void test_drift_within_tolerance_is_one_run(void)
{
    printf("risk: a station's own drift must not split into several runs\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dat[JS8_FRAME_BYTES]; mk(dat, 4, 0x77);

    /* Inside the tolerance: one run. */
    js8_reasm_add(&r, 5000, 1000, dat, JS8_FRAME_DATA, NULL);
    js8_reasm_add(&r, 5015, 1000 + JS8_REASM_FREQ_TOL_HZ, dat, JS8_FRAME_DATA, NULL);
    CHECK(js8_reasm_active(&r) == 1, "drift of exactly the tolerance stays one run (got %d)\n",
          js8_reasm_active(&r));

    /* Outside it: a different station, and merging them would attribute one
     * operator's message to another. */
    js8_reasm_add(&r, 5030, 1000 + JS8_REASM_FREQ_TOL_HZ * 3, dat, JS8_FRAME_DATA, NULL);
    CHECK(js8_reasm_active(&r) == 2, "beyond the tolerance is a second run (got %d)\n",
          js8_reasm_active(&r));
}

static void test_nearest_run_wins_not_first_in_the_array(void)
{
    printf("risk: a frame between two runs goes to the NEARER one\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dat[JS8_FRAME_BYTES]; mk(dat, 4, 0x88);

    /* ⚠ THE NUMBERS HERE ARE THE TEST. Both runs must be INSIDE the
     * tolerance of the probe frame, or the tolerance filter alone decides and
     * nearest-vs-first is never exercised. The first version of this test
     * used 1000/1028 with a probe at 1026: 26 Hz put the first run outside
     * the 15 Hz window, so a first-match search reached the same answer and
     * the test passed against a deliberately broken implementation. Caught by
     * mutating find_run() to first-match, 2026-10-09.
     *
     * 20 Hz apart so both can exist as separate runs; the probe at 1014 is
     * 14 Hz from the EARLIER one in the array and 6 Hz from the later, so
     * first-match and nearest-match give different answers. */
    js8_reasm_add(&r, 6000, 1000, dat, JS8_FRAME_DATA, "FAR_BUT_FIRST");
    js8_reasm_add(&r, 6000, 1020, dat, JS8_FRAME_DATA, "NEAR_BUT_SECOND");
    CHECK(js8_reasm_active(&r) == 2, "two runs 20 Hz apart (got %d)\n",
          js8_reasm_active(&r));

    js8_reasm_add(&r, 6015, 1014, dat, JS8_FRAME_DATA, NULL);
    for (int i = 0; i < js8_reasm_active(&r); i++) {
        const js8_reasm_run_t* run = js8_reasm_at(&r, i);
        if (run->freq_hz == 1000)
            CHECK(run->n_seen == 1, "the 14 Hz-away run did not take it (got %d)\n",
                  run->n_seen);
        else
            CHECK(run->n_seen == 2, "the 6 Hz-away run took it (got %d)\n",
                  run->n_seen);
    }
}

static void test_a_structured_frame_ends_the_run(void)
{
    printf("a station returning to structured frames ends its free text\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dat[JS8_FRAME_BYTES], dir[JS8_FRAME_BYTES];
    mk(dat, 4, 0x99);
    mk(dir, 3, 0xAA);

    js8_reasm_add(&r, 7000, 1100, dat, JS8_FRAME_DATA, NULL);
    CHECK(js8_reasm_active(&r) == 1, "run open\n");
    js8_reasm_add(&r, 7015, 1100, dir, JS8_FRAME_DIRECTED, "OZ1LAV");
    js8_reasm_tick(&r, 7015);
    CHECK(js8_reasm_active(&r) == 0, "closed by the directed frame (active=%d)\n",
          js8_reasm_active(&r));
}

static void test_overflow_counts_without_storing(void)
{
    printf("a very long run keeps counting after it stops storing\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dat[JS8_FRAME_BYTES]; mk(dat, 4, 0xBB);

    const int n = JS8_REASM_MAX_FRAMES + 5;
    for (int i = 0; i < n; i++)
        js8_reasm_add(&r, 8000 + 15 * i, 1300, dat, JS8_FRAME_DATA, NULL);

    const js8_reasm_run_t* run = js8_reasm_at(&r, 0);
    CHECK(run && run->n_frames == JS8_REASM_MAX_FRAMES, "stored the cap (%d)\n",
          run ? run->n_frames : -1);
    CHECK(run && run->n_seen == n, "counted all %d (got %d)\n", n,
          run ? run->n_seen : -1);
    CHECK(run && run->overflowed, "flagged as overflowed\n");

    char line[64];
    js8_reasm_describe(run, line, sizeof(line));
    CHECK(strstr(line, "+") != NULL, "the line admits the overflow: '%s'\n", line);
}

static void test_mixed_coding_is_flagged_not_split(void)
{
    printf("both codings in one transmission is normal traffic, not a split\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t h[JS8_FRAME_BYTES], c[JS8_FRAME_BYTES];
    mk(h, 4, 0xCC);
    mk(c, 6, 0xDD);

    /* packDataMessage() picks per frame, whichever packs more characters, so
     * a real message mixes them. Splitting the run here would show one
     * sender as two. */
    js8_reasm_add(&r, 9000, 1400, h, JS8_FRAME_DATA, NULL);
    js8_reasm_add(&r, 9015, 1400, c, JS8_FRAME_DATA_COMPRESSED, NULL);
    js8_reasm_add(&r, 9030, 1400, h, JS8_FRAME_DATA, NULL);

    CHECK(js8_reasm_active(&r) == 1, "still one run (got %d)\n", js8_reasm_active(&r));
    const js8_reasm_run_t* run = js8_reasm_at(&r, 0);
    CHECK(run && run->n_seen == 3, "all 3 frames\n");
    CHECK(run && run->mixed_coding, "mixed coding flagged\n");
}

static void test_duplicate_candidates_in_one_slot_are_one_frame(void)
{
    printf("the same frame found several times in one slot is ONE fragment\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dat[JS8_FRAME_BYTES]; mk(dat, 4, 0x5A);

    /* ⛔ THE MEASURED CASE, 2026-10-09 on 14.078: the decoder finds one
     * transmission as several candidates a few Hz apart, each passes CRC-12,
     * and each arrived here as a new fragment. HB9BV's free text came out as
     * "MSG ID 178MSG ID 178MSG ID 178MSG ID 178". */
    for (int i = 0; i < 4; i++)
        js8_reasm_add(&r, 11000, 1270 + i * 3, dat, JS8_FRAME_DATA, NULL);

    CHECK(js8_reasm_active(&r) == 1, "one run (got %d)\n", js8_reasm_active(&r));
    const js8_reasm_run_t* run = js8_reasm_at(&r, 0);
    CHECK(run && run->n_seen == 1, "n_seen is %d, want 1\n", run ? run->n_seen : -1);
    CHECK(run && run->n_dup == 3, "n_dup is %d, want 3\n", run ? run->n_dup : -1);

    /* A DIFFERENT frame in the same slot is a real second fragment. */
    uint8_t other[JS8_FRAME_BYTES]; mk(other, 4, 0xA5);
    js8_reasm_add(&r, 11000, 1272, other, JS8_FRAME_DATA, NULL);
    run = js8_reasm_at(&r, 0);
    CHECK(run && run->n_seen == 2, "a different frame was swallowed (n_seen %d)\n",
          run ? run->n_seen : -1);

    /* The SAME frame in a LATER slot is a station repeating itself, which is
     * real traffic and must still count. */
    js8_reasm_add(&r, 11015, 1270, other, JS8_FRAME_DATA, NULL);
    run = js8_reasm_at(&r, 0);
    CHECK(run && run->n_seen == 3, "a repeat in the next slot was dropped (n_seen %d)\n",
          run ? run->n_seen : -1);
}

static void test_a_full_table_refuses_rather_than_evicting(void)
{
    printf("a full table refuses a new run and says so\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dat[JS8_FRAME_BYTES]; mk(dat, 4, 0xEE);

    for (int i = 0; i < JS8_REASM_MAX_ACTIVE; i++)
        js8_reasm_add(&r, 10000, 500 + i * 100, dat, JS8_FRAME_DATA, NULL);
    CHECK(js8_reasm_active(&r) == JS8_REASM_MAX_ACTIVE, "table full\n");

    bool taken = js8_reasm_add(&r, 10000, 500 + JS8_REASM_MAX_ACTIVE * 100,
                               dat, JS8_FRAME_DATA, NULL);
    CHECK(!taken, "the extra frame was refused\n");
    CHECK(js8_reasm_active(&r) == JS8_REASM_MAX_ACTIVE,
          "no existing run was evicted for it\n");
    CHECK(r.dropped_no_slot == 1, "and it was counted (got %d)\n", r.dropped_no_slot);
}

static void test_a_stale_identification_is_forgotten(void)
{
    printf("an old callsign on an offset must not name a new station's text\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dir[JS8_FRAME_BYTES], dat[JS8_FRAME_BYTES];
    mk(dir, 3, 0x12);
    mk(dat, 4, 0x34);

    js8_reasm_add(&r, 11000, 900, dir, JS8_FRAME_DIRECTED, "OLDCALL");
    /* Long enough later that the identification has expired. Attributing new
     * free text to whoever last used that frequency would put another
     * operator's callsign on someone's message. */
    int64_t later = 11000 + JS8_REASM_TIMEOUT_SLOTS * 15;
    js8_reasm_tick(&r, later);
    js8_reasm_add(&r, later, 900, dat, JS8_FRAME_DATA, NULL);

    const js8_reasm_run_t* run = js8_reasm_at(&r, 0);
    CHECK(run && !run->sender[0], "the run is anonymous, not '%s'\n",
          run ? run->sender : "(null)");
}

/* ---- the text that J7 exists for ------------------------------------- */

static void test_text_accumulates_across_frames(void)
{
    printf("text: frames are decoded on arrival and joined into one message\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dir[JS8_FRAME_BYTES];
    mk(dir, 3, 0x11);
    js8_reasm_add(&r, 12000, 1100, dir, JS8_FRAME_DIRECTED, "OZ9JEP");

    /* Two Huffman frames, built the way js8_jsc_harness builds them: is-data,
     * coding flag 0, codes, one 0, then 1s. "E"=100 and "T"=1101 come
     * straight from varicode.cpp's hufftable. */
    uint8_t f1[JS8_FRAME_BYTES], f2[JS8_FRAME_BYTES];
    {
        const char *b1 = "10" "100"  "0";   /* data, huff, E, pad-zero */
        const char *b2 = "10" "1101" "0";   /* data, huff, T, pad-zero */
        const char *src[2] = { b1, b2 };
        uint8_t *dst[2] = { f1, f2 };
        for (int k = 0; k < 2; k++) {
            uint8_t bits[72];
            int n = 0;
            for (const char *p = src[k]; *p; p++) bits[n++] = (uint8_t)(*p == '1');
            while (n < 72) bits[n++] = 1;
            memset(dst[k], 0, JS8_FRAME_BYTES);
            for (int i = 0; i < 72; i++)
                if (bits[i]) dst[k][i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
        }
    }
    js8_reasm_add(&r, 12015, 1100, f1, JS8_FRAME_DATA, NULL);
    js8_reasm_add(&r, 12030, 1100, f2, JS8_FRAME_DATA, NULL);

    const js8_reasm_run_t *run = js8_reasm_at(&r, 0);
    CHECK(run && run->n_seen == 2, "two data frames (got %d)\n",
          run ? run->n_seen : -1);
    CHECK(run && !strcmp(run->text, "ET"),
          "joined to 'ET', got '%s'\n", run ? run->text : "(null)");
    CHECK(run && !run->text_full, "not flagged truncated\n");

    char line[64];
    js8_reasm_describe(run, line, sizeof(line));
    /* ⭐ The text IS the line once there is text. A decode list showing a
     * frame count beside an unreadable message would be the worst of both. */
    CHECK(!strcmp(line, "OZ9JEP: ET"), "line '%s'\n", line);
}

static void test_a_truncated_message_says_so_even_in_a_short_buffer(void)
{
    printf("text: the truncation marker survives a short caller buffer\n");
    js8_reasm_t r; js8_reasm_init(&r);
    uint8_t dat[JS8_FRAME_BYTES];
    mk(dat, 4, 0xBB);          /* decodes to Huffman text, deliberately */

    for (int i = 0; i < JS8_REASM_MAX_FRAMES + 5; i++)
        js8_reasm_add(&r, 13000 + 15 * i, 1300, dat, JS8_FRAME_DATA, NULL);

    const js8_reasm_run_t *run = js8_reasm_at(&r, 0);
    CHECK(run && (run->text_full || run->overflowed), "flagged as cut\n");

    /* 40 bytes against 240 of text. ⛔ The marker was AFTER the text in the
     * first version and snprintf cut it off, so a message stopped short read
     * as a complete one. This is that test. */
    char line[40];
    js8_reasm_describe(run, line, sizeof(line));
    CHECK(strstr(line, "f+]") != NULL,
          "the line admits the cut even at 40 bytes: '%s'\n", line);
}

int main(void)
{
    printf("=== js8_reasm harness ===\n\n");
    test_frame_type_folds_the_data_frames();
    test_a_run_takes_its_name_from_an_earlier_frame();
    test_unidentified_run_says_the_frequency();
    test_a_sender_who_vanishes_is_closed_not_stuck();
    test_two_senders_interleaving_stay_separate();
    test_drift_within_tolerance_is_one_run();
    test_nearest_run_wins_not_first_in_the_array();
    test_a_structured_frame_ends_the_run();
    test_overflow_counts_without_storing();
    test_mixed_coding_is_flagged_not_split();
    test_duplicate_candidates_in_one_slot_are_one_frame();
    test_a_full_table_refuses_rather_than_evicting();
    test_a_stale_identification_is_forgotten();
    test_text_accumulates_across_frames();
    test_a_truncated_message_says_so_even_in_a_short_buffer();
    test_a_distant_identification_does_not_name_our_run();
    test_two_candidate_callsigns_name_nobody();
    test_nearest_identification_wins();

    printf("\n%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
