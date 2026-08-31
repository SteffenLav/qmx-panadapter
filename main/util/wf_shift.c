/* See wf_shift.h for why this is portable and what each case means. */
#include "wf_shift.h"

static void add_clear(wf_shift_plan_t *p, int x, int n)
{
    if (n <= 0 || p->n_clear >= 2) return;
    p->clear[p->n_clear].x = x;
    p->clear[p->n_clear].n = n;
    p->n_clear++;
}

bool wf_shift_plan(const wf_shift_cfg_t *c, int dx, wf_shift_plan_t *out)
{
    if (!c || !out) return false;
    for (int i = 0; i < 2; i++) { out->clear[i].x = 0; out->clear[i].n = 0; }
    out->n_clear = 0;
    out->move = false;
    out->src_x = out->dst_x = out->keep_n = 0;
    out->view_x = c ? c->view_x : 0;

    if (c->screen_w <= 0 || c->canvas_w < c->screen_w || c->margin < 0) return false;
    if (c->view_x < 0 || c->view_x + c->screen_w > c->canvas_w)         return false;
    if (dx == 0) return true;                       /* nothing to do, not an error */

    const int W  = c->screen_w;
    const int ad = (dx < 0) ? -dx : dx;

    /* Nothing survives: the new window and the old one do not overlap at all. */
    if (ad >= W) {
        out->view_x = c->margin;
        add_clear(out, 0, c->canvas_w);
        return true;
    }

    const int keep = W - ad;
    const int nvx  = c->view_x + dx;

    /* The cheap path: the margin absorbs it. Nothing moves; only the strip
     * entering the view is blanked. */
    if (nvx >= 0 && nvx + W <= c->canvas_w) {
        out->view_x = nvx;
        /* Entering columns are at the LEADING edge of the travel. */
        if (dx > 0) add_clear(out, nvx + W - dx, dx);
        else        add_clear(out, nvx, ad);
        return true;
    }

    /* The margin is exhausted, so the surviving columns are physically moved
     * back to the middle and the view is re-centred.
     *
     * dx > 0: the view moved right, so what survives is the RIGHT part of the
     *         old window and it becomes the LEFT part of the new one.
     * dx < 0: the mirror image - the LEFT part survives and lands |dx| in. */
    out->move   = true;
    out->keep_n = keep;
    out->view_x = c->margin;
    if (dx > 0) { out->src_x = c->view_x + dx; out->dst_x = c->margin; }
    else        { out->src_x = c->view_x;      out->dst_x = c->margin + ad; }

    /* Everything that is not the surviving block is blanked - both margins and
     * the part of the window the surviving block does not fill. */
    add_clear(out, 0, out->dst_x);
    add_clear(out, out->dst_x + keep, c->canvas_w - out->dst_x - keep);
    return true;
}
