#include "js8_text.h"

#include <stdio.h>
#include <string.h>

/* ---- small helpers -------------------------------------------------------
 *
 * Deliberately not pulled from text.c: that file's helpers are shaped around
 * FT8's 77-bit field types, and the three things needed here are three lines
 * each. */

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Split "<a> <b> <rest>" exactly as ft8_qso.c's split_msg3() does, because the
 * text this file produces has to survive that function unchanged and the text
 * it consumes comes from it. Any difference between the two is a bug that only
 * shows up mid-QSO, so the rules are kept identical: single tokens, the rest
 * verbatim, no trailing-space trimming beyond the separators. */
static bool split3(const char* text, char* a, size_t ca, char* b, size_t cb,
                   char* rest, size_t cr)
{
    const char* p = text;
    while (*p == ' ') p++;
    const char* s1 = p;
    while (*p && *p != ' ') p++;
    size_t l1 = (size_t)(p - s1);
    if (l1 == 0 || l1 >= ca) return false;
    memcpy(a, s1, l1);
    a[l1] = '\0';

    while (*p == ' ') p++;
    const char* s2 = p;
    while (*p && *p != ' ') p++;
    size_t l2 = (size_t)(p - s2);
    if (l2 == 0 || l2 >= cb) return false;
    memcpy(b, s2, l2);
    b[l2] = '\0';

    while (*p == ' ') p++;
    snprintf(rest, cr, "%s", p);
    return true;
}

/* "-07" / "+02" / "-7" -> -7. Returns false for anything else, INCLUDING a
 * bare number with no sign: "73" must never parse as a report. */
static bool parse_report(const char* s, int* out)
{
    if (!s || (s[0] != '-' && s[0] != '+')) return false;
    int sign = (s[0] == '-') ? -1 : 1;
    const char* d = s + 1;
    if (!is_digit(d[0])) return false;
    int v = 0;
    while (is_digit(*d))
    {
        v = v * 10 + (*d - '0');
        d++;
        if (v > 999) return false;
    }
    if (*d != '\0') return false;
    *out = sign * v;
    return true;
}

bool js8_text_rest_is_grid(const char* rest)
{
    if (!rest) return false;

    /* ⚠ "RR73" IS A SYNTACTICALLY VALID MAIDENHEAD SQUARE. RR is a legal field
     * pair (the letters run A..R) and 73 is a legal square, so the shape test
     * below accepts it, and a grid test that ran before the protocol words
     * would treat every sign-off as a locator. The real tokens win by name.
     * ft8_qso.c has the same collision and resolves it by ordering - its
     * strcmp(rest, "RR73") runs before is_roger_token() - but this predicate is
     * called on its own, so it has to be safe by itself.
     * A far-east locator like RE78 is a grid and must still pass. */
    if (strcmp(rest, "RR73") == 0 || strcmp(rest, "RRR") == 0) return false;

    /* Four characters, LL##. */
    if (strlen(rest) != 4) return false;
    if (rest[0] < 'A' || rest[0] > 'R') return false;
    if (rest[1] < 'A' || rest[1] > 'R') return false;
    return is_digit(rest[2]) && is_digit(rest[3]);
}

void js8_fmt_report(int snr_db, char* out, size_t out_len)
{
    /* packNum's range, not FT8's. Clamping here rather than letting
     * js8_pack_num() do it keeps the TEXT and the FRAME saying the same thing:
     * a silent clamp inside the packer would log "+40" and transmit "+31". */
    if (snr_db < -30) snr_db = -30;
    if (snr_db > 31) snr_db = 31;
    snprintf(out, out_len, "%+03d", snr_db);
}

// ---------------------------------------------------------------------------
// Frame -> text
// ---------------------------------------------------------------------------

bool js8_frame_to_text(const uint8_t frame[JS8_FRAME_BYTES], char* out, size_t out_len)
{
    if (!frame || !out || out_len == 0) return false;
    out[0] = '\0';

    switch (js8_frame_type(frame))
    {
    case JS8_FRAME_HEARTBEAT:
    {
        js8_heartbeat_t hb;
        if (!js8_unpack_heartbeat(frame, &hb)) return false;
        if (!hb.call[0]) return false;
        /* "CQ <call> <grid>" is what the decode list, the CQ filter, the robot
         * and ft8_qso.c's `strncmp(text, "CQ ", 3)` all already recognise. The
         * HB beacon is spelled out but is not a CQ: answering a heartbeat as if
         * it were a call for a QSO is how a relay network gets pestered. */
        const char* lead = hb.is_cq ? "CQ" : "HB";
        if (hb.grid[0])
            snprintf(out, out_len, "%s %s %s", lead, hb.call, hb.grid);
        else
            snprintf(out, out_len, "%s %s", lead, hb.call);
        return true;
    }

    case JS8_FRAME_DIRECTED:
    {
        js8_directed_t d;
        if (!js8_unpack_directed(frame, &d)) return false;
        if (!d.from[0] || !d.to[0]) return false;

        char rest[16];
        rest[0] = '\0';

        if (d.cmd == JS8_CMD_SNR && d.num != JS8_NUM_NONE)
        {
            js8_fmt_report(d.num, rest, sizeof(rest));
        }
        else if (d.cmd == JS8_CMD_RR)
        {
            /* RR with a number is FT8's "R-07" - rogered, and here is my
             * report. RR without one is the end of the exchange, which FT8
             * spells RR73 and which ft8_qso.c compares against literally. */
            if (d.num != JS8_NUM_NONE)
            {
                char r[8];
                js8_fmt_report(d.num, r, sizeof(r));
                snprintf(rest, sizeof(rest), "R%s", r);
            }
            else
            {
                snprintf(rest, sizeof(rest), "RR73");
            }
        }
        else if (d.cmd == JS8_CMD_73)
        {
            snprintf(rest, sizeof(rest), "73");
        }
        else
        {
            /* Every other command is spelled, not dropped. An unrecognised
             * `rest` reaches ft8_qso.c as "a report we cannot read", which it
             * already handles by waiting - and the operator sees what was
             * actually sent instead of a blank row. */
            snprintf(rest, sizeof(rest), "%s", js8_cmd_text(d.cmd));
        }

        snprintf(out, out_len, "%s %s %s", d.to, d.from, rest);
        return true;
    }

    case JS8_FRAME_COMPOUND:
    case JS8_FRAME_COMPOUND_DIRECTED:
    default:
        /* Out of scope (docs/js8-feasibility.md). Refusing is right: these
         * carry compound callsigns, and rendering one with the 28-bit codec
         * would put a DIFFERENT station's callsign on the screen. */
        return false;
    }
}

// ---------------------------------------------------------------------------
// Text -> frame
// ---------------------------------------------------------------------------

bool js8_text_to_frame(const char* text, uint8_t frame[JS8_FRAME_BYTES], uint8_t* itype)
{
    if (!text || !frame) return false;
    if (itype) *itype = 0; /* Normal, single frame */

    char a[16], b[16], rest[24];
    if (!split3(text, a, sizeof(a), b, sizeof(b), rest, sizeof(rest))) return false;

    if (strcmp(a, "CQ") == 0 || strcmp(a, "HB") == 0)
    {
        js8_heartbeat_t hb;
        memset(&hb, 0, sizeof(hb));
        snprintf(hb.call, sizeof(hb.call), "%s", b);
        hb.is_cq = (a[0] == 'C');
        hb.bits3 = 0; /* "CQ CQ CQ" / the plain HB wording */
        /* FT8 CQs carry other things in this position - "CQ DX", "CQ POTA" -
         * and JS8's Heartbeat has no field for them. Only a locator is kept;
         * anything else is dropped rather than refused, because the CQ itself
         * is still valid without it. */
        if (js8_text_rest_is_grid(rest))
        {
            /* Exactly 4 characters by the predicate's own test; copied rather
             * than snprintf'd so the 5-byte field is provably not truncated. */
            memcpy(hb.grid, rest, 4);
            hb.grid[4] = '\0';
        }
        return js8_pack_heartbeat(&hb, frame);
    }

    js8_directed_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.to, sizeof(d.to), "%s", a);
    snprintf(d.from, sizeof(d.from), "%s", b);
    d.num = JS8_NUM_NONE;

    int rpt = 0;
    if (strcmp(rest, "RR73") == 0 || strcmp(rest, "RRR") == 0)
    {
        d.cmd = JS8_CMD_RR;
    }
    else if (strcmp(rest, "73") == 0)
    {
        d.cmd = JS8_CMD_73;
    }
    else if (rest[0] == 'R' && parse_report(rest + 1, &rpt))
    {
        d.cmd = JS8_CMD_RR;
        d.num = rpt;
    }
    else if (parse_report(rest, &rpt))
    {
        d.cmd = JS8_CMD_SNR;
        d.num = rpt;
    }
    else
    {
        /* No JS8 Directed form. The FT8 ladder's opening "<them> <me> <grid>"
         * lands here, and so does free text. Refusing is the point: a frame
         * invented for it would key the radio with something no JS8Call station
         * answers, and the caller can send a report instead - which is what
         * JS8's shorter ladder does anyway. */
        return false;
    }

    /* Out of packNum's range means the report is not the one we would log.
     * Refuse rather than let the packer clamp silently. */
    if (d.num != JS8_NUM_NONE && (d.num < -30 || d.num > 31)) return false;

    return js8_pack_directed(&d, frame);
}
