/* Host test for main/util/gps_autogrid.c - when the GPS may rewrite the
 * station's Maidenhead grid.
 *
 * Build (from the repo root):
 *   gcc -O2 -Wall -I main/util \
 *       -o gps_autogrid_harness test/gps_autogrid_harness.c main/util/gps_autogrid.c \
 *       && ./gps_autogrid_harness
 *
 * This is worth a harness because the value goes ON THE AIR: my_grid is in
 * every FT8 CQ and TX1, every ADIF record and every LoTW upload. The failure
 * modes are not crashes - they are a locator that flickers between two squares
 * in consecutive transmissions, and an NVS write every second.
 */
#include <stdio.h>
#include <string.h>

#include "gps_autogrid.h"

static int fails;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Feed the same grid for `secs` seconds; return how many times it fired. */
static int feed(gps_autogrid_t *st, const char *gps, const char *stored, int secs)
{
    int fired = 0;
    for (int i = 0; i < secs; i++)
        if (gps_autogrid_step(st, gps, stored)) fired++;
    return fired;
}

static void t_waits_for_a_stable_fix(void)
{
    printf("a new grid is not believed until it has held for a minute\n");
    gps_autogrid_t st; gps_autogrid_init(&st);
    CHECK(feed(&st, "JO65MR", "JO45AB", GPS_AUTOGRID_STABLE_S - 1) == 0,
          "fired before %d s", GPS_AUTOGRID_STABLE_S);
    CHECK(gps_autogrid_step(&st, "JO65MR", "JO45AB"),
          "did not fire at %d s", GPS_AUTOGRID_STABLE_S);
}

static void t_a_wandering_fix_never_fires(void)
{
    printf("a grid that keeps changing never fires\n");
    gps_autogrid_t st; gps_autogrid_init(&st);
    int fired = 0;
    for (int i = 0; i < 10 * GPS_AUTOGRID_STABLE_S; i++) {
        /* The sixth character is about 4 km across, so a settling fix really
         * does walk between neighbouring subsquares. */
        const char *g = (i % 2) ? "JO65MR" : "JO65MS";
        if (gps_autogrid_step(&st, g, "JO45AB")) fired++;
    }
    CHECK(fired == 0, "a wandering fix fired %d times", fired);
}

static void t_same_grid_is_never_rewritten(void)
{
    printf("the stored grid is not rewritten with itself\n");
    gps_autogrid_t st; gps_autogrid_init(&st);
    CHECK(feed(&st, "JO65MR", "JO65MR", 10 * GPS_AUTOGRID_STABLE_S) == 0,
          "rewrote a grid that already matched");
}

static void t_case_does_not_make_a_new_grid(void)
{
    printf("case alone is not a different square\n");
    gps_autogrid_t st; gps_autogrid_init(&st);
    /* ⛔ A hand-typed grid may be all upper or all lower. Comparing case
     * sensitively would rewrite the setting every minute, forever. */
    CHECK(feed(&st, "JO65MR", "jo65mr", 10 * GPS_AUTOGRID_STABLE_S) == 0,
          "treated a case difference as a move");
}

static void t_empty_stored_grid_is_filled(void)
{
    printf("a station with no grid set gets one\n");
    gps_autogrid_t st; gps_autogrid_init(&st);
    CHECK(feed(&st, "JO65MR", "", GPS_AUTOGRID_STABLE_S) == 1,
          "did not fill an empty grid exactly once");
}

static void t_losing_the_fix_restarts_the_count(void)
{
    printf("losing the fix restarts the count\n");
    gps_autogrid_t st; gps_autogrid_init(&st);
    feed(&st, "JO65MR", "JO45AB", GPS_AUTOGRID_STABLE_S - 1);
    /* ⛔ 59 seconds of agreement must NOT be banked across an outage: the
     * receiver may come back somewhere else, and one reading would then be
     * enough to rewrite what goes out on the air. */
    CHECK(!gps_autogrid_step(&st, NULL, "JO45AB"), "fired on a lost fix");
    CHECK(!gps_autogrid_step(&st, "JO65MR", "JO45AB"),
          "banked the count across a lost fix");
    CHECK(feed(&st, "JO65MR", "JO45AB", GPS_AUTOGRID_STABLE_S - 2) == 0,
          "fired early after the fix came back");
    CHECK(gps_autogrid_step(&st, "JO65MR", "JO45AB"),
          "never fired after a full stable period");
}

static void t_moving_to_a_new_square_fires_once(void)
{
    printf("moving to a new square fires once, then settles\n");
    gps_autogrid_t st; gps_autogrid_init(&st);
    CHECK(feed(&st, "JO65MR", "JO45AB", GPS_AUTOGRID_STABLE_S) == 1,
          "the move did not fire exactly once");
    /* The caller has now written it, so `stored` agrees. */
    CHECK(feed(&st, "JO65MR", "JO65MR", 10 * GPS_AUTOGRID_STABLE_S) == 0,
          "kept firing after the write");
}

static void t_null_state_is_safe(void)
{
    printf("a NULL state does not fault\n");
    CHECK(!gps_autogrid_step(NULL, "JO65MR", "JO45AB"), "NULL state returned true");
}

int main(void)
{
    t_waits_for_a_stable_fix();
    t_a_wandering_fix_never_fires();
    t_same_grid_is_never_rewritten();
    t_case_does_not_make_a_new_grid();
    t_empty_stored_grid_is_filled();
    t_losing_the_fix_restarts_the_count();
    t_moving_to_a_new_square_fires_once();
    t_null_state_is_safe();

    printf(fails ? "\nFAIL (%d)\n" : "\nPASS\n", fails);
    return fails ? 1 : 0;
}
