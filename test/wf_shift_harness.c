/* Host test for wf_shift_plan() - moving the waterfall with the viewport (#298).
 *
 * Build (from the repo root):
 *   gcc -I main/util -o wf_shift_harness test/wf_shift_harness.c \
 *       main/util/wf_shift.c && ./wf_shift_harness
 *
 * It links the REAL function, not a copy.
 *
 * THE MODEL CARRIES BOTH FREQUENCY AND TIME, and it took two rewrites to get
 * there - each one because mutations survived:
 *
 *  1. One row, refilled every step. Three mutations lived, including deleting
 *     the blanking of newly exposed columns outright: the refill overwrote the
 *     stale cell before the check saw it. The newest row is always right on the
 *     device too - the ROWS ABOVE carry the lie.
 *
 *  2. Eight rows, frequency only. The same three lived, for a subtler reason: on
 *     the cheap path a canvas column maps to a FIXED absolute frequency, so a
 *     column that leaves the view and returns still holds the right frequency.
 *     It holds it from the wrong TIME - thirty-second-old history sitting in a
 *     row whose neighbours are five seconds old. The waterfall's y axis IS time,
 *     so that is precisely the smear this feature exists to prevent, and a model
 *     without a clock cannot see it.
 *
 * So each cell records the absolute column AND the tick it was written, rows
 * scroll as they do on the device, and a visible cell must be blank or be
 * exactly right about both.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf_shift.h"

#define CANVAS_W 1792
#define SCREEN_W 1280
#define MARGIN    256
#define ROWS        8
#define BLANK   (-1)

static int fails = 0;
static void ok(const char *what, int cond)
{
    if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

typedef struct { int abs; int tick; } cell_t;
static cell_t canvas[ROWS][CANVAS_W];

static void blank_all(void)
{
    for (int r = 0; r < ROWS; r++)
        for (int x = 0; x < CANVAS_W; x++) {
            canvas[r][x].abs = BLANK; canvas[r][x].tick = BLANK;
        }
}

/* Execute a plan exactly as ui.c must: on every row of the buffer. */
static void apply(const wf_shift_plan_t *p)
{
    static cell_t tmp[CANVAS_W];
    for (int r = 0; r < ROWS; r++) {
        if (p->move) {
            memcpy(tmp, canvas[r] + p->src_x, (size_t)p->keep_n * sizeof(cell_t));
            memmove(canvas[r] + p->dst_x, tmp, (size_t)p->keep_n * sizeof(cell_t));
        }
        for (int i = 0; i < p->n_clear; i++)
            for (int k = 0; k < p->clear[i].n; k++) {
                int x = p->clear[i].x + k;
                if (x >= 0 && x < CANVAS_W) {
                    canvas[r][x].abs = BLANK; canvas[r][x].tick = BLANK;
                }
            }
    }
}

static void scroll_and_push(int view_x, int abs0, int tick)
{
    for (int r = ROWS - 1; r > 0; r--) memcpy(canvas[r], canvas[r - 1], sizeof canvas[0]);
    for (int x = 0; x < SCREEN_W; x++) {
        canvas[0][view_x + x].abs  = abs0 + x;
        canvas[0][view_x + x].tick = tick;
    }
}

static void walk(const char *what, const int *steps, int n_steps)
{
    wf_shift_cfg_t c = { CANVAS_W, SCREEN_W, MARGIN, MARGIN };
    int abs0 = 100000, tick = 0;

    blank_all();
    for (int r = 0; r < ROWS; r++)
        for (int x = 0; x < SCREEN_W; x++) {
            canvas[r][c.view_x + x].abs  = abs0 + x;
            canvas[r][c.view_x + x].tick = -r;          /* row r is r ticks old */
        }

    int moves = 0, worst_blank = 0;
    for (int s = 0; s < n_steps; s++) {
        wf_shift_plan_t p;
        ok("plan accepted", wf_shift_plan(&c, steps[s], &p));
        apply(&p);
        if (p.move) moves++;
        c.view_x = p.view_x;
        abs0    += steps[s];
        tick    += 1;
        scroll_and_push(c.view_x, abs0, tick);

        ok("view in bounds", c.view_x >= 0 && c.view_x + SCREEN_W <= CANVAS_W);
        if (p.move)
            ok("move stays in the canvas",
               p.src_x >= 0 && p.src_x + p.keep_n <= CANVAS_W &&
               p.dst_x >= 0 && p.dst_x + p.keep_n <= CANVAS_W);

        /* THE INVARIANT. A visible cell is blank, or it is right about the
         * frequency the viewport now claims is there AND about how old it is.
         * Anything else is history drawn at a place or a time it did not
         * happen. */
        int blanks = 0, row1_blanks = 0;
        for (int r = 0; r < ROWS; r++) {
            int rb = 0;
            for (int x = 0; x < SCREEN_W; x++) {
                cell_t v = canvas[r][c.view_x + x];
                if (v.abs == BLANK) { rb++; continue; }
                if (v.abs != abs0 + x) {
                    printf("      step %d row %d x=%d: frequency %d, should be %d\n",
                           s, r, x, v.abs, abs0 + x);
                    ok("cell shows the right frequency", 0); return;
                }
                if (v.tick != tick - r) {
                    printf("      step %d row %d x=%d: written at tick %d, row is tick %d\n",
                           s, r, x, v.tick, tick - r);
                    ok("cell shows the right moment", 0); return;
                }
            }
            if (rb > blanks) blanks = rb;
            if (r == 1) row1_blanks = rb;    /* was written across the full width last step */
        }
        if (blanks > worst_blank) worst_blank = blanks;

        /* Anything that could survive, must. Measured on ROW 1: it was written
         * across the full width one step ago, so its blanks are exactly what
         * THIS step threw away. Older rows carry the accumulated blanking of
         * every step since, which says nothing about the plan just made -
         * reading the max across rows here reported 4132 failures against code
         * that was right. */
        int ad = abs(steps[s]);
        ok("no more is blanked than had to be",
           row1_blanks <= (ad > SCREEN_W ? SCREEN_W : ad));
    }
    printf("  %-34s %3d step(s), %d memmove(s), worst blank %d px\n",
           what, n_steps, moves, worst_blank);
}

int main(void)
{
    printf("sequences the viewport actually produces\n");
    { int s[30]; for (int i = 0; i < 30; i++) s[i] =  4; walk("push right, 30 x 4 px", s, 30); }
    { int s[30]; for (int i = 0; i < 30; i++) s[i] = -4; walk("push left, 30 x 4 px",  s, 30); }
    { int s[35]; for (int i = 0; i < 32; i++) s[i] = 4;
      s[32] = 998; s[33] = 4; s[34] = 4;   walk("push then page (998 px)", s, 35); }
    { int s[3] = { -998, -4, 998 };        walk("page left then right", s, 3); }
    { int s[40]; for (int i = 0; i < 40; i++) s[i] = (i & 1) ? -37 : 41;
                                           walk("oscillate +41/-37", s, 40); }
    { int s[4] = { 5000, -5000, 1280, -1280 };
                                           walk("jumps with no overlap at all", s, 4); }
    /* The case a frequency-only model cannot see: a column leaves the view and
     * comes back. Right frequency, wrong moment, unless it was blanked. */
    { int s[12] = { 40, -40, 40, -40, 120, -120, 200, -200, 60, -60, 8, -8 };
                                           walk("columns leave and return", s, 12); }

    printf("\nrandom walk, because real tuning is not tidy\n");
    {
        srand(20260831);
        int s[4000];
        for (int i = 0; i < 4000; i++) {
            int r = rand() % 100;
            s[i] = (r < 80) ? (rand() % 9) - 4
                 : (r < 95) ? (rand() % 400) - 200
                            : ((rand() % 2) ? 998 : -998);
        }
        walk("4000 mixed steps", s, 4000);
    }

    printf("\nthe cheap path must actually be taken\n");
    {
        /* If every small shift needed a memmove the margin would be pointless,
         * and that is the whole performance claim - so it is asserted. */
        wf_shift_cfg_t c = { CANVAS_W, SCREEN_W, MARGIN, MARGIN };
        int moves = 0;
        for (int i = 0; i < 60; i++) {
            wf_shift_plan_t p;
            wf_shift_plan(&c, 4, &p);
            if (p.move) moves++;
            c.view_x = p.view_x;
        }
        printf("  60 x 4 px = 240 px of push: %d memmove(s)\n", moves);
        ok("240 px of push needs at most one memmove", moves <= 1);
    }

    printf("\nedges and nonsense\n");
    {
        wf_shift_cfg_t c = { CANVAS_W, SCREEN_W, MARGIN, MARGIN };
        wf_shift_plan_t p;
        ok("dx 0 is a no-op", wf_shift_plan(&c, 0, &p) && !p.move &&
                              p.n_clear == 0 && p.view_x == MARGIN);
        ok("NULL cfg refused", !wf_shift_plan(NULL, 4, &p));
        ok("NULL out refused", !wf_shift_plan(&c, 4, NULL));

        wf_shift_cfg_t bad  = { SCREEN_W, CANVAS_W, MARGIN, 0 };
        ok("canvas smaller than screen refused", !wf_shift_plan(&bad, 4, &p));
        wf_shift_plan(&bad, 4, &p);
        ok("a refusal clears nothing", p.n_clear == 0 && !p.move);

        wf_shift_cfg_t oob  = { CANVAS_W, SCREEN_W, MARGIN, CANVAS_W };
        ok("view already out of bounds refused", !wf_shift_plan(&oob, 4, &p));
        wf_shift_cfg_t zero = { CANVAS_W, 0, MARGIN, MARGIN };
        ok("zero screen width refused", !wf_shift_plan(&zero, 4, &p));
        wf_shift_cfg_t neg  = { CANVAS_W, SCREEN_W, -1, MARGIN };
        ok("negative margin refused", !wf_shift_plan(&neg, 4, &p));

        wf_shift_cfg_t c2 = { CANVAS_W, SCREEN_W, MARGIN, MARGIN };
        wf_shift_plan(&c2, SCREEN_W, &p);
        ok("a full screen of travel blanks everything",
           !p.move && p.n_clear == 1 && p.clear[0].n == CANVAS_W && p.view_x == MARGIN);
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall pass\n", fails);
    return fails ? 1 : 0;
}
