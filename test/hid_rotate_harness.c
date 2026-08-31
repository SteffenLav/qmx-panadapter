/* Host test for hid_rotate_to_indev() - the mouse pointer's rotation inverse.
 *
 * Build (from the repo root):
 *   gcc -I main/util -o hid_rotate_harness test/hid_rotate_harness.c \
 *       main/util/hid_rotate.c && ./hid_rotate_harness
 *
 * The test is a ROUND TRIP, not a table of expected outputs: it runs our
 * inverse, then LVGL's own forward map copied verbatim from
 * managed_components/lvgl__lvgl/src/indev/lv_indev.c (indev_pointer_proc, v9.2),
 * and asserts the landscape point comes back unchanged. That is the actual
 * invariant, it checks BOTH rotations, and it cannot drift from a hand-written
 * expectation the way a table can.
 *
 * It links the REAL function, not a copy.
 */
#include <stdio.h>
#include "hid_rotate.h"

static int fails = 0;
static void ok(const char *what, int cond)
{
    if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

/* ---- LVGL 9.2 indev_pointer_proc, verbatim ------------------------------- *
 * hor_res / ver_res are the RAW PANEL dimensions (720 x 1280 here), which is
 * the trap: they are NOT what lv_display_get_*_resolution() returns once the
 * display is rotated. Keeping the original names makes the copy checkable
 * against the source. */
static void lvgl_forward(int rot_deg, int32_t hor_res, int32_t ver_res,
                         int32_t *px, int32_t *py)
{
    if (rot_deg == 180 || rot_deg == 270) {
        *px = hor_res - *px - 1;
        *py = ver_res - *py - 1;
    }
    if (rot_deg == 90 || rot_deg == 270) {
        int32_t tmp = *py;
        *py = *px;
        *px = ver_res - tmp - 1;
    }
}

/* Sweep every corner, both edges and a grid across the screen. */
static void sweep(const char *what, hid_rot_t rot, int rot_deg)
{
    const int32_t SW = 1280, SH = 720;      /* landscape, as LVGL reports it */
    const int32_t HOR = SH,  VER = SW;      /* raw panel: 720 x 1280 */
    int bad = 0, n = 0;
    int32_t worst_lx = -1, worst_ly = -1, got_x = 0, got_y = 0;

    for (int32_t ly = 0; ly < SH; ly += 1)
        for (int32_t lx = 0; lx < SW; lx += 1) {
            int32_t ix, iy;
            hid_rotate_to_indev(rot, SW, SH, lx, ly, &ix, &iy);

            /* The point we hand LVGL must be inside the raw panel, or LVGL
             * logs "X is ... greater than hor. res" and clamps. */
            if (ix < 0 || ix >= HOR || iy < 0 || iy >= VER) {
                if (!bad) { worst_lx = lx; worst_ly = ly; got_x = ix; got_y = iy; }
                bad++; n++; continue;
            }

            int32_t ox = ix, oy = iy;
            lvgl_forward(rot_deg, HOR, VER, &ox, &oy);
            n++;
            if (ox != lx || oy != ly) {
                if (!bad) { worst_lx = lx; worst_ly = ly; got_x = ox; got_y = oy; }
                bad++;
            }
        }

    printf("  %-22s %d point(s), %d wrong\n", what, n, bad);
    if (bad) printf("      first: asked (%d,%d) got (%d,%d)\n",
                    (int)worst_lx, (int)worst_ly, (int)got_x, (int)got_y);
    ok(what, bad == 0);
}

int main(void)
{
    printf("every pixel round-trips through LVGL's own map\n");
    sweep("normal  (ROTATION_90)",  HID_ROT_90,  90);
    sweep("flipped (ROTATION_270)", HID_ROT_270, 270);

    printf("\nthe two rotations must NOT agree - that was the bug\n");
    {
        /* Feeding the 90 inverse to a 270 display point-reflects the cursor.
         * If these ever produce the same point the fix has been undone. */
        int32_t ax, ay, bx, by;
        hid_rotate_to_indev(HID_ROT_90,  1280, 720, 300, 200, &ax, &ay);
        hid_rotate_to_indev(HID_ROT_270, 1280, 720, 300, 200, &bx, &by);
        printf("  (300,200) -> normal (%d,%d), flipped (%d,%d)\n",
               (int)ax, (int)ay, (int)bx, (int)by);
        ok("rotations differ", ax != bx || ay != by);

        /* And name the exact symptom: run the WRONG inverse and confirm it
         * lands point-reflected, which is what "the mouse does not flip"
         * looked like on the bench. */
        int32_t wx = ax, wy = ay;
        lvgl_forward(270, 720, 1280, &wx, &wy);
        printf("  90-inverse on a 270 display -> (%d,%d), expected (1279-300, 719-200) = (979,519)\n",
               (int)wx, (int)wy);
        ok("the old behaviour is a 180 point-reflection", wx == 1279 - 300 && wy == 719 - 200);
    }

    printf("\nedges and nonsense\n");
    {
        int32_t x = -7, y = -7;
        hid_rotate_to_indev((hid_rot_t)99, 1280, 720, 0, 0, &x, &y);
        int32_t nx, ny;
        hid_rotate_to_indev(HID_ROT_90, 1280, 720, 0, 0, &nx, &ny);
        ok("unknown rotation falls back to 90", x == nx && y == ny);
        /* NULL outputs must not crash and must not be a precondition. */
        hid_rotate_to_indev(HID_ROT_90,  1280, 720, 5, 5, NULL, NULL);
        hid_rotate_to_indev(HID_ROT_270, 1280, 720, 5, 5, &x, NULL);
        ok("single output still written", x == (720 - 1) - 5);
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall pass\n", fails);
    return fails ? 1 : 0;
}
