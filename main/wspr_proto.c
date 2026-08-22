#include "wspr_proto.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* char36(): '0'-'9' -> 0-9, 'A'-'Z' -> 10-35, else -1.
 * charletter(): 'A'-'Z' -> 0-25, else -1. */
static int char36(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}
static int charletter(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    return -1;
}

/* Power (dBm) -> the legal WSPR power codepoint, rounded to the nearest of
 * the fixed set {0,3,7,10,13,17,20,23,27,30,...}. From encode.py
 * (robertostling/wspr-tools), an independent encoder — see wspr_proto.h. */
static int pack_power_field(int dbm)
{
    static const int corr[10] = { 0, -1, 1, 0, -1, 2, 1, 0, -1, 1 };
    if (dbm < 0) dbm = 0;
    if (dbm > 60) dbm = 60;
    return dbm + corr[dbm % 10] + 64;
}

int wspr_pack_message(const char *callsign, const char *grid, int power_dbm,
                       wspr_msg_bytes_t *out)
{
    if (!callsign || !grid || !out) return 0;

    char c[6] = { 0 };
    size_t len = strlen(callsign);
    if (len < 4 || len > 6) return 0;
    for (size_t k = 0; k < len; k++) {
        char ch = callsign[k];
        c[k] = (char)((ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch);
    }

    /* Find the digit that anchors the mixed-radix packing (the callsign's
     * numeral) — WsprryPi's wspr.cpp scans for it rather than assuming a
     * fixed position, which is what lets both "K1ABC" (digit at 1) and
     * "OZ1LAV" (digit at 2) pack with the same formula.
     *
     * A prefix-digit callsign (e.g. "4X1XX") has more than one digit, so
     * "first digit found" is ambiguous — it would anchor on the '4' and
     * leave "1XX" as a 3-char suffix that isn't all letters. The real
     * constraint (matching the standard-callsign shape the type-1 message
     * requires) is: at most 2 prefix chars, at most 3 suffix chars, and the
     * suffix must be letters only — that disambiguates "4X1XX" to the '1'. */
    int i = -1;
    for (int k = 0; k < (int)len && k <= 2; k++) {
        if (c[k] < '0' || c[k] > '9') continue;
        int suffix_len = (int)len - k - 1;
        if (suffix_len > 3) continue;
        int suffix_ok = 1;
        for (int m = k + 1; m < (int)len; m++) {
            if (c[m] < 'A' || c[m] > 'Z') { suffix_ok = 0; break; }
        }
        if (!suffix_ok) continue;
        i = k;
        break;
    }
    if (i < 0) return 0;
    int n = (int)len - i - 1; /* suffix chars after the digit */

    int32_t n1 = (i < 2) ? 36 : char36(c[i - 2]);
    if (n1 < 0) return 0;
    int p1 = (i < 1) ? 36 : char36(c[i - 1]);
    if (p1 < 0) return 0;
    n1 = 36 * n1 + p1;
    n1 = 10 * n1 + (c[i] - '0');
    int s1 = (n < 1) ? 26 : charletter(c[i + 1]);
    int s2 = (n < 2) ? 26 : charletter(c[i + 2]);
    int s3 = (n < 3) ? 26 : charletter(c[i + 3]);
    if (s1 < 0 || s2 < 0 || s3 < 0) return 0;
    n1 = 27 * n1 + s1;
    n1 = 27 * n1 + s2;
    n1 = 27 * n1 + s3;

    if (strlen(grid) != 4) return 0;
    char g[4];
    for (int k = 0; k < 4; k++) {
        char ch = grid[k];
        g[k] = (char)((ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch);
    }
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') return 0;
    if (g[2] < '0' || g[2] > '9' || g[3] < '0' || g[3] > '9') return 0;
    int lon_field = g[0] - 'A';
    int lat_field = g[1] - 'A';
    int lon_square = g[2] - '0';
    int lat_square = g[3] - '0';
    /* Grid-locator packing formula from encode.py (robertostling/
     * wspr-tools) - an independently-sourced encoder, cross-checked in the
     * harness by round-tripping through wspr_unpack_message()'s
     * WSJT-X-ported unpackgrid(). */
    int32_t n_locator = (179 - 10 * lon_field - lon_square) * 180
                         + 10 * lat_field + lat_square;

    int32_t n2 = (n_locator << 7) | pack_power_field(power_dbm);

    /* Byte-pack n1 (28 bits) + n2 (22 bits) = 50 bits into 7 bytes, the
     * exact inverse of WSJT-X's unpack50(). */
    out->dat[0] = (uint8_t)((n1 >> 20) & 0xFF);
    out->dat[1] = (uint8_t)((n1 >> 12) & 0xFF);
    out->dat[2] = (uint8_t)((n1 >> 4) & 0xFF);
    out->dat[3] = (uint8_t)(((n1 & 0xF) << 4) | ((n2 >> 18) & 0xF));
    out->dat[4] = (uint8_t)((n2 >> 10) & 0xFF);
    out->dat[5] = (uint8_t)((n2 >> 2) & 0xFF);
    out->dat[6] = (uint8_t)((n2 & 0x3) << 6);
    return 1;
}

/* Everything below is ported byte-identical from WSJT-X's own
 * lib/wsprd/wsprd_utils.c (unpack50/unpackcall/unpackgrid) - the
 * authoritative decoder. Do not "clean up" the logic to look more like the
 * pack side above; the point is that this half is independently verifiable
 * against the real source, not derived from it. */
static const char WSPR_C36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ ";

static void wspr_unpack50(const uint8_t *dat, int32_t *n1, int32_t *n2)
{
    int32_t i4;

    i4 = dat[0] & 255;
    *n1 = i4 << 20;
    i4 = dat[1] & 255;
    *n1 = *n1 + (i4 << 12);
    i4 = dat[2] & 255;
    *n1 = *n1 + (i4 << 4);
    i4 = dat[3] & 255;
    *n1 = *n1 + ((i4 >> 4) & 15);
    *n2 = (i4 & 15) << 18;
    i4 = dat[4] & 255;
    *n2 = *n2 + (i4 << 10);
    i4 = dat[5] & 255;
    *n2 = *n2 + (i4 << 2);
    i4 = dat[6] & 255;
    *n2 = *n2 + ((i4 >> 6) & 3);
}

static int wspr_unpackcall(int32_t ncall, char *call)
{
    int32_t n;
    int i;
    char tmp[7];

    n = ncall;
    strcpy(call, "......");
    if (n >= 262177560L) return 0;

    i = n % 27 + 10; tmp[5] = WSPR_C36[i]; n = n / 27;
    i = n % 27 + 10; tmp[4] = WSPR_C36[i]; n = n / 27;
    i = n % 27 + 10; tmp[3] = WSPR_C36[i]; n = n / 27;
    i = n % 10;       tmp[2] = WSPR_C36[i]; n = n / 10;
    i = n % 36;       tmp[1] = WSPR_C36[i]; n = n / 36;
    i = n;            tmp[0] = WSPR_C36[i];
    tmp[6] = '\0';

    for (i = 0; i < 5; i++) {
        if (tmp[i] != ' ') break;
    }
    snprintf(call, 7, "%-6s", &tmp[i]);
    for (i = 0; i < 6; i++) {
        if (call[i] == ' ') call[i] = '\0';
    }
    return 1;
}

static int wspr_unpackgrid(int32_t ngrid, char *grid, int *power_dbm)
{
    int dlat, dlong;

    *power_dbm = (ngrid & 0x7F) - 64;
    ngrid = ngrid >> 7;
    if (ngrid >= 32400) {
        strcpy(grid, "XXXX");
        return 0;
    }
    dlat = (ngrid % 180) - 90;
    dlong = (ngrid / 180) * 2 - 180 + 2;
    if (dlong < -180) dlong += 360;
    if (dlong > 180) dlong += 360;

    int nlong = (int)(60.0 * (180.0 - dlong) / 5.0);
    int n1 = nlong / 240;
    int n2 = (nlong - 240 * n1) / 24;
    grid[0] = WSPR_C36[10 + n1];
    grid[2] = WSPR_C36[n2];

    int nlat = (int)(60.0 * (dlat + 90) / 2.5);
    n1 = nlat / 240;
    n2 = (nlat - 240 * n1) / 24;
    grid[1] = WSPR_C36[10 + n1];
    grid[3] = WSPR_C36[n2];
    return 1;
}

int wspr_unpack_message(const wspr_msg_bytes_t *in, char callsign_out[7],
                         char grid_out[5], int *power_dbm_out)
{
    if (!in || !callsign_out || !grid_out || !power_dbm_out) return 0;
    int32_t n1, n2;
    wspr_unpack50(in->dat, &n1, &n2);
    if (!wspr_unpackcall(n1, callsign_out)) return 0;
    if (!wspr_unpackgrid(n2, grid_out, power_dbm_out)) return 0;
    grid_out[4] = '\0';
    return 1;
}
