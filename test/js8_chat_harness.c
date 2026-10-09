/* Host test for main/js8_chat.c - the JS8 conversation store.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main \
 *       -o js8_chat_harness test/js8_chat_harness.c main/js8_chat.c \
 *       && ./js8_chat_harness
 *
 * The store is small, so the interesting part is not that it holds messages -
 * it is the four ways it can quietly ruin a page:
 *
 *   1. A growing free-text run printed once per slot instead of updated. That
 *      defect already shipped once in the slot log (fixed in 9f386b88), and
 *      an append-only store would reintroduce it in the UI.
 *   2. The same run split into several messages because its audio offset
 *      drifted a few Hz between slots.
 *   3. Two different transmissions on the same offset merged into one.
 *   4. A finished run still marked live, so the page shows a message that
 *      stopped a minute ago as still arriving.
 */
#include <stdio.h>
#include <string.h>

#include "js8_chat.h"

static int fails;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static js8_chat_msg_t mk(js8_chat_kind_t kind, int64_t first, int64_t last,
                         int hz, const char *sender, const char *text,
                         int frames, bool active)
{
    js8_chat_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = kind;
    m.first_utc = first;
    m.last_utc = last;
    m.freq_hz = hz;
    m.frames = frames;
    m.snr_db = JS8_CHAT_NO_SNR;
    m.active = active;
    snprintf(m.sender, sizeof(m.sender), "%s", sender ? sender : "");
    snprintf(m.text, sizeof(m.text), "%s", text ? text : "");
    return m;
}

static void t_growing_run_updates_in_place(void)
{
    printf("growing run updates in place\n");
    js8_chat_clear();
    js8_chat_msg_t a = mk(JS8_CHAT_FREETEXT, 1000, 1000, 1188, "", "HELLO", 1, true);
    js8_chat_add(&a);
    js8_chat_msg_t b = mk(JS8_CHAT_FREETEXT, 1000, 1015, 1188, "", "HELLO THERE", 2, true);
    js8_chat_add(&b);
    js8_chat_msg_t c = mk(JS8_CHAT_FREETEXT, 1000, 1030, 1190, "OZ1LAV", "HELLO THERE OM", 3, true);
    js8_chat_add(&c);

    CHECK(js8_chat_count() == 1, "three feeds of one run gave %d messages, want 1",
          js8_chat_count());
    js8_chat_msg_t got;
    CHECK(js8_chat_at(0, &got), "nothing at index 0");
    CHECK(strcmp(got.text, "HELLO THERE OM") == 0, "text is '%s', want the newest", got.text);
    CHECK(got.frames == 3, "frames is %d, want 3", got.frames);
    /* The sender arrives late, from an ident heard on the same offset. */
    CHECK(strcmp(got.sender, "OZ1LAV") == 0, "sender is '%s', want OZ1LAV", got.sender);
}

static void t_offset_drift_is_one_run(void)
{
    printf("offset drift within tolerance stays one run\n");
    js8_chat_clear();
    js8_chat_msg_t a = mk(JS8_CHAT_FREETEXT, 2000, 2000, 1500, "", "A", 1, true);
    js8_chat_add(&a);
    /* 14 Hz: inside js8_reasm's own 15 Hz tolerance. */
    js8_chat_msg_t b = mk(JS8_CHAT_FREETEXT, 2000, 2015, 1514, "", "AB", 2, true);
    js8_chat_add(&b);
    CHECK(js8_chat_count() == 1, "14 Hz of drift split the run into %d", js8_chat_count());

    /* 40 Hz away is a different signal even in the same slot. */
    js8_chat_msg_t c = mk(JS8_CHAT_FREETEXT, 2000, 2015, 1540, "", "Z", 1, true);
    js8_chat_add(&c);
    CHECK(js8_chat_count() == 2, "40 Hz away merged: %d messages, want 2", js8_chat_count());
}

static void t_later_transmission_same_offset_is_separate(void)
{
    printf("a later transmission on the same offset is a separate message\n");
    js8_chat_clear();
    js8_chat_msg_t a = mk(JS8_CHAT_FREETEXT, 3000, 3015, 1188, "G4GHL", "FIRST", 2, false);
    js8_chat_add(&a);
    js8_chat_msg_t b = mk(JS8_CHAT_FREETEXT, 3120, 3120, 1188, "G4GHL", "SECOND", 1, true);
    js8_chat_add(&b);
    CHECK(js8_chat_count() == 2, "same offset, different start merged: %d, want 2",
          js8_chat_count());
    js8_chat_msg_t got;
    js8_chat_at(0, &got);
    CHECK(strcmp(got.text, "SECOND") == 0, "newest is '%s', want SECOND", got.text);
    js8_chat_at(1, &got);
    CHECK(strcmp(got.text, "FIRST") == 0, "older is '%s', want FIRST", got.text);
}

static void t_kinds_do_not_merge(void)
{
    printf("a directed message does not merge with a free-text run\n");
    js8_chat_clear();
    js8_chat_msg_t a = mk(JS8_CHAT_FREETEXT, 4000, 4000, 1188, "", "QSL", 1, true);
    js8_chat_add(&a);
    js8_chat_msg_t b = mk(JS8_CHAT_DIRECTED, 4000, 4000, 1188, "G4GHL",
                          "G4GHL LA7HKA HEARTBEAT SNR", 1, false);
    js8_chat_add(&b);
    CHECK(js8_chat_count() == 2, "kinds merged: %d messages, want 2", js8_chat_count());
}

static void t_ring_keeps_the_newest(void)
{
    printf("the ring wraps and keeps the newest\n");
    js8_chat_clear();
    char buf[32];
    for (int i = 0; i < JS8_CHAT_MAX_MSGS + 7; i++) {
        snprintf(buf, sizeof(buf), "MSG%d", i);
        js8_chat_msg_t m = mk(JS8_CHAT_DIRECTED, 5000 + i, 5000 + i, 1000 + i,
                              "X", buf, 1, false);
        js8_chat_add(&m);
    }
    CHECK(js8_chat_count() == JS8_CHAT_MAX_MSGS, "count is %d, want %d",
          js8_chat_count(), JS8_CHAT_MAX_MSGS);
    js8_chat_msg_t got;
    js8_chat_at(0, &got);
    snprintf(buf, sizeof(buf), "MSG%d", JS8_CHAT_MAX_MSGS + 6);
    CHECK(strcmp(got.text, buf) == 0, "newest is '%s', want %s", got.text, buf);
    js8_chat_at(JS8_CHAT_MAX_MSGS - 1, &got);
    snprintf(buf, sizeof(buf), "MSG%d", 7);
    CHECK(strcmp(got.text, buf) == 0, "oldest is '%s', want %s", got.text, buf);
    CHECK(!js8_chat_at(JS8_CHAT_MAX_MSGS, &got), "read past the end succeeded");
}

static void t_tick_clears_active(void)
{
    printf("tick stops marking a finished run as live\n");
    js8_chat_clear();
    js8_chat_msg_t a = mk(JS8_CHAT_FREETEXT, 6000, 6000, 1188, "", "QSL", 1, true);
    js8_chat_add(&a);
    js8_chat_tick(6030, 60);
    js8_chat_msg_t got;
    js8_chat_at(0, &got);
    CHECK(got.active, "cleared after 30 s with a 60 s quiet window");
    js8_chat_tick(6060, 60);
    js8_chat_at(0, &got);
    CHECK(!got.active, "still live 60 s after the last frame");
}

static void t_empty_store_is_safe(void)
{
    printf("the accessors are safe on an empty store\n");
    js8_chat_clear();
    js8_chat_msg_t got;
    CHECK(js8_chat_count() == 0, "count is %d on an empty store", js8_chat_count());
    CHECK(!js8_chat_at(0, &got), "index 0 read something from an empty store");
    CHECK(!js8_chat_at(-1, &got), "index -1 read something");
    js8_chat_tick(1, 60);   /* must not fault */
}

int main(void)
{
    if (!js8_chat_init()) { printf("js8_chat_init failed\n"); return 1; }

    t_growing_run_updates_in_place();
    t_offset_drift_is_one_run();
    t_later_transmission_same_offset_is_separate();
    t_kinds_do_not_merge();
    t_ring_keeps_the_newest();
    t_tick_clears_active();
    t_empty_store_is_safe();

    printf(fails ? "\nFAIL (%d)\n" : "\nPASS\n", fails);
    return fails ? 1 : 0;
}
