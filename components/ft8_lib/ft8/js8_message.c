#include "js8_message.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The callsign and grid alphabet, verbatim from varicode.cpp. 39 characters:
 * 0-9, A-Z, space, '/', '@'. The 28-bit codec uses the first 37 and the 50-bit
 * one the first 38; '@' exists for the group tokens in `basecalls`. */
static const char kAlnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";
#define ALNUM_N 39

/* 37 * 36 * 10 * 27 * 27 * 27. Everything at or above this is a group token
 * rather than a callsign. */
#define NBASECALL 262177560u

/* 180 * 180. packGrid never produces a value above this; the span up to
 * 0x7FFF is where the reference hides commands and "no grid". */
#define NBASEGRID 32400u
#define NMAXGRID  0x7FFFu

/* ⚠ Only the two group tokens a plain QSO can meet. The reference has
 * nbasecall+1..+20 or so, including the @DX/xx and @JS8NET groups, which are
 * out of scope here (docs/js8-feasibility.md). An unknown group unpacks as
 * "<....>" rather than a wrong name. */
static const struct { const char* name; uint32_t value; } kBasecalls[] = {
    { "<....>",   NBASECALL + 1 },
    { "@ALLCALL", NBASECALL + 2 },
};
#define NBASECALLS ((int)(sizeof(kBasecalls) / sizeof(kBasecalls[0])))

/* directed_cmds, by index. The gaps are indices the reference leaves unused
 * or deprecated; they spell as "?" rather than being silently shown as
 * something they are not. Index 31 is free text, which this does not send. */
static const char* const kCmdText[32] = {
    "SNR?",   "DIT DIT", "NACK",   "HEARING?", "GRID?",  ">",      "STATUS?", "STATUS",
    "HEARING", "MSG",    "MSG TO:", "QUERY",   "QUERY MSGS", "QUERY CALL", "ACK", "GRID",
    "INFO?",  "INFO",    "FB",     "HW CPY?",  "SK",     "RR",     "QSL?",   "QSL",
    "CMD",    "SNR",     "NO",     "YES",      "73",     "HEARTBEAT SNR", "AGN?", "",
};

static const char* const kCqText[8] = {
    "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD", "CQ FD", "CQ CQ", "CQ",
};

const char* js8_cmd_text(uint8_t cmd)
{
    if (cmd > 31) return "?";
    const char* t = kCmdText[cmd];
    return (t && t[0]) ? t : "?";
}

const char* js8_cq_text(bool is_cq, uint8_t bits3)
{
    /* All eight HB variants spell "HB"; the distinctions (AUTO, RELAY, SPOT)
     * belong to the heartbeat network, which is out of scope. */
    if (!is_cq) return "HB";
    return kCqText[bits3 & 7];
}

/* ---- bit plumbing -------------------------------------------------------- */

static void put_bits(uint8_t* buf, int* pos, uint64_t value, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--)
    {
        int bit = (int)((value >> i) & 1u);
        if (bit)
            buf[*pos / 8] |= (uint8_t)(0x80u >> (*pos % 8));
        (*pos)++;
    }
}

static uint64_t get_bits(const uint8_t* buf, int* pos, int nbits)
{
    uint64_t v = 0;
    for (int i = 0; i < nbits; i++)
    {
        v = (v << 1) | (uint64_t)((buf[*pos / 8] >> (7 - (*pos % 8))) & 1u);
        (*pos)++;
    }
    return v;
}

static int alnum_index(char c)
{
    for (int i = 0; i < ALNUM_N; i++)
    {
        if (kAlnum[i] == c) return i;
    }
    return -1;
}

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* ---- 28-bit callsign ----------------------------------------------------- */

/* The reference matches a regex over a set of space-padded permutations:
 *   ([0-9A-Z ])([0-9A-Z])([0-9])([A-Z ])([A-Z ])([A-Z ])
 * Every permutation it builds is exactly six characters, so the match reduces
 * to a per-position character-class test. */
static bool call_pattern_ok(const char* p)
{
    char c0 = p[0], c1 = p[1], c2 = p[2];
    bool ok0 = (c0 >= '0' && c0 <= '9') || (c0 >= 'A' && c0 <= 'Z') || c0 == ' ';
    bool ok1 = (c1 >= '0' && c1 <= '9') || (c1 >= 'A' && c1 <= 'Z');
    bool ok2 = (c2 >= '0' && c2 <= '9');
    if (!ok0 || !ok1 || !ok2) return false;
    for (int i = 3; i < 6; i++)
    {
        char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || c == ' ')) return false;
    }
    return true;
}

uint32_t js8_pack_callsign(const char* call, bool* portable)
{
    if (portable) *portable = false;
    if (!call || !call[0]) return 0;

    char s[16];
    size_t n = strlen(call);
    if (n >= sizeof(s)) return 0;
    for (size_t i = 0; i < n; i++) s[i] = up(call[i]);
    s[n] = '\0';
    /* trimmed() */
    while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';

    for (int i = 0; i < NBASECALLS; i++)
    {
        if (strcmp(s, kBasecalls[i].name) == 0) return kBasecalls[i].value;
    }

    /* "/P" is not part of the callsign - it becomes one bit in the frame. */
    if (n > 2 && s[n - 2] == '/' && s[n - 1] == 'P')
    {
        n -= 2;
        s[n] = '\0';
        if (portable) *portable = true;
    }

    /* Swaziland and Guinea: prefixes the six-character grammar cannot hold,
     * rewritten into ones it can and rewritten back on unpack. Carried over
     * verbatim from the reference - a station using one of these is otherwise
     * unaddressable. */
    char w[16];
    if (strncmp(s, "3DA0", 4) == 0 && n >= 4)
    {
        snprintf(w, sizeof(w), "3D0%s", s + 4);
        snprintf(s, sizeof(s), "%s", w);
        n = strlen(s);
    }
    else if (n >= 3 && s[0] == '3' && s[1] == 'X' && s[2] >= 'A' && s[2] <= 'Z')
    {
        snprintf(w, sizeof(w), "Q%s", s + 2);
        snprintf(s, sizeof(s), "%s", w);
        n = strlen(s);
    }

    if (n < 2 || n > 6) return 0;

    /* The permutations, in the reference's own order - and it keeps the LAST
     * one that matches, not the first, so the order is not cosmetic. */
    char perms[3][8];
    int  nperm = 0;
    snprintf(perms[nperm++], 8, "%s", s);
    switch (n)
    {
    case 2: snprintf(perms[nperm++], 8, " %s   ", s); break;
    case 3: snprintf(perms[nperm++], 8, " %s  ", s);
            snprintf(perms[nperm++], 8, "%s   ", s); break;
    case 4: snprintf(perms[nperm++], 8, " %s ", s);
            snprintf(perms[nperm++], 8, "%s  ", s); break;
    case 5: snprintf(perms[nperm++], 8, " %s", s);
            snprintf(perms[nperm++], 8, "%s ", s); break;
    default: break;
    }

    const char* matched = NULL;
    for (int i = 0; i < nperm; i++)
    {
        if (strlen(perms[i]) == 6 && call_pattern_ok(perms[i])) matched = perms[i];
    }
    if (!matched) return 0;

    /* Mixed radix 37/36/10/27/27/27. The -10 on the last three maps '0'..'9'
     * out of the alphabet's leading digits so that A..Z and space fit 27. */
    int i0 = alnum_index(matched[0]);
    int i1 = alnum_index(matched[1]);
    int i2 = alnum_index(matched[2]);
    int i3 = alnum_index(matched[3]);
    int i4 = alnum_index(matched[4]);
    int i5 = alnum_index(matched[5]);
    if (i0 < 0 || i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0 || i5 < 0) return 0;

    uint32_t packed = (uint32_t)i0;
    packed = 36 * packed + (uint32_t)i1;
    packed = 10 * packed + (uint32_t)i2;
    packed = 27 * packed + (uint32_t)(i3 - 10);
    packed = 27 * packed + (uint32_t)(i4 - 10);
    packed = 27 * packed + (uint32_t)(i5 - 10);
    return packed;
}

void js8_unpack_callsign(uint32_t value, bool portable, char out[16])
{
    out[0] = '\0';
    for (int i = 0; i < NBASECALLS; i++)
    {
        if (kBasecalls[i].value == value)
        {
            snprintf(out, 16, "%s", kBasecalls[i].name);
            return;
        }
    }
    if (value >= NBASECALL)
    {
        /* A group token we do not carry. "<....>" is the reference's own
         * placeholder for a callsign it cannot name - better than inventing. */
        snprintf(out, 16, "<....>");
        return;
    }

    char w[7];
    uint32_t v = value;
    w[5] = kAlnum[v % 27 + 10]; v /= 27;
    w[4] = kAlnum[v % 27 + 10]; v /= 27;
    w[3] = kAlnum[v % 27 + 10]; v /= 27;
    w[2] = kAlnum[v % 10];      v /= 10;
    w[1] = kAlnum[v % 36];      v /= 36;
    w[0] = kAlnum[v % 37];
    w[6] = '\0';

    /* Undo the two prefix rewrites, then the padding. */
    char tmp[16];
    if (strncmp(w, "3D0", 3) == 0)
        snprintf(tmp, sizeof(tmp), "3DA0%s", w + 3);
    else if (w[0] == 'Q' && w[1] >= 'A' && w[1] <= 'Z')
        snprintf(tmp, sizeof(tmp), "3X%s", w + 1);
    else
        snprintf(tmp, sizeof(tmp), "%s", w);

    int a = 0;
    while (tmp[a] == ' ') a++;
    int b = (int)strlen(tmp);
    while (b > a && tmp[b - 1] == ' ') b--;

    int k = 0;
    for (int i = a; i < b && k < 13; i++) out[k++] = tmp[i];
    out[k] = '\0';
    if (portable) snprintf(out + k, 16 - (size_t)k, "/P");
}

/* ---- 50-bit callsign ----------------------------------------------------- */

uint64_t js8_pack_alnum50(const char* call)
{
    if (!call) return 0;

    /* Keep only the alphabet's own characters, then force the '/' positions:
     * an 11-character field with a binary "is a slash" digit at 3 and 7. */
    char w[16];
    int  n = 0;
    for (const char* p = call; *p && n < 11; p++)
    {
        char c = up(*p);
        if (alnum_index(c) >= 0 && c != '@') w[n++] = c;
    }
    w[n] = '\0';

    if (n > 3 && w[3] != '/')
    {
        memmove(w + 4, w + 3, (size_t)(n - 3) + 1);
        w[3] = ' ';
        n++;
    }
    if (n > 7 && w[7] != '/')
    {
        memmove(w + 8, w + 7, (size_t)(n - 7) + 1);
        w[7] = ' ';
        n++;
    }
    while (n < 11) w[n++] = ' ';
    w[11] = '\0';

    const uint64_t R = 38;
    uint64_t packed = 0;
    int idx[11];
    for (int i = 0; i < 11; i++)
    {
        int v = alnum_index(w[i]);
        if (v < 0 || v > 37) return 0;
        idx[i] = v;
    }

    packed += (uint64_t)idx[0] * R * R * R * 2 * R * R * R * 2 * R * R;
    packed += (uint64_t)idx[1] * R * R * R * 2 * R * R * R * 2 * R;
    packed += (uint64_t)idx[2] * R * R * R * 2 * R * R * R * 2;
    packed += (uint64_t)(w[3] == '/') * R * R * R * 2 * R * R * R;
    packed += (uint64_t)idx[4] * R * R * R * 2 * R * R;
    packed += (uint64_t)idx[5] * R * R * R * 2 * R;
    packed += (uint64_t)idx[6] * R * R * R * 2;
    packed += (uint64_t)(w[7] == '/') * R * R * R;
    packed += (uint64_t)idx[8] * R * R;
    packed += (uint64_t)idx[9] * R;
    packed += (uint64_t)idx[10];
    return packed;
}

void js8_unpack_alnum50(uint64_t packed, char out[16])
{
    char w[12];
    const uint64_t R = 38;

    w[10] = kAlnum[packed % R]; packed /= R;
    w[9]  = kAlnum[packed % R]; packed /= R;
    w[8]  = kAlnum[packed % R]; packed /= R;
    w[7]  = (packed % 2) ? '/' : ' '; packed /= 2;
    w[6]  = kAlnum[packed % R]; packed /= R;
    w[5]  = kAlnum[packed % R]; packed /= R;
    w[4]  = kAlnum[packed % R]; packed /= R;
    w[3]  = (packed % 2) ? '/' : ' '; packed /= 2;
    w[2]  = kAlnum[packed % R]; packed /= R;
    w[1]  = kAlnum[packed % R]; packed /= R;
    w[0]  = kAlnum[packed % R];
    w[11] = '\0';

    /* Drop the padding spaces the packer inserted, keeping any '/'. */
    int k = 0;
    for (int i = 0; i < 11 && k < 15; i++)
    {
        if (w[i] == ' ') continue;
        out[k++] = w[i];
    }
    out[k] = '\0';
}

/* ---- grid ---------------------------------------------------------------- */

/* Maidenhead to degrees, the reference's own convention - and it is NOT the
 * usual one: longitude comes out POSITIVE WEST (nlong = 180 - 20*field). A
 * sign error here is invisible, because the value still round-trips through
 * unpackGrid; it only shows when a real station decodes the frame. */
static void grid2deg(const char* grid4, float* dlong, float* dlat)
{
    char g[7];
    g[0] = up(grid4[0]); g[1] = up(grid4[1]);
    g[2] = grid4[2];     g[3] = grid4[3];
    g[4] = 'm';          g[5] = 'm';          /* centre of the square */
    g[6] = '\0';

    int   nlong    = 180 - 20 * (g[0] - 'A');
    int   n20d     = 2 * (g[2] - '0');
    float xminlong = 5.0f * ((g[4] - 'a') + 0.5f);
    *dlong = (float)(nlong - n20d) - xminlong / 60.0f;

    int   nlat    = -90 + 10 * (g[1] - 'A') + (g[3] - '0');
    float xminlat = 2.5f * ((g[5] - 'a') + 0.5f);
    *dlat = (float)nlat + xminlat / 60.0f;
}

static void deg2grid(float dlong, float dlat, char out[5])
{
    if (dlong < -180.0f) dlong += 360.0f;
    if (dlong > 180.0f)  dlong -= 360.0f;

    int nlong = (int)(60.0f * (180.0f - dlong) / 5.0f);
    int n1 = nlong / 240;
    int n2 = (nlong - 240 * n1) / 24;
    out[0] = (char)('A' + n1);
    out[2] = (char)('0' + n2);

    int nlat = (int)(60.0f * (dlat + 90.0f) / 2.5f);
    n1 = nlat / 240;
    n2 = (nlat - 240 * n1) / 24;
    out[1] = (char)('A' + n1);
    out[3] = (char)('0' + n2);
    out[4] = '\0';
}

uint16_t js8_pack_grid(const char* grid)
{
    if (!grid) return NMAXGRID;
    int n = 0;
    while (grid[n] && grid[n] != ' ') n++;
    if (n < 4) return NMAXGRID;          /* no grid, as the reference sends */

    float dlong, dlat;
    grid2deg(grid, &dlong, &dlat);

    /* Truncation toward zero, not floor - the reference assigns a float to an
     * int, and dlong is negative over most of the eastern hemisphere. */
    int ilong = (int)dlong;
    int ilat  = (int)dlat + 90;
    return (uint16_t)(((ilong + 180) / 2) * 180 + ilat);
}

void js8_unpack_grid(uint16_t value, char out[5])
{
    if (value > NBASEGRID)
    {
        out[0] = '\0';
        return;
    }
    float dlat  = (float)(value % 180) - 90.0f;
    float dlong = (float)(value / 180) * 2.0f - 180.0f + 2.0f;
    deg2grid(dlong, dlat, out);
}

/* ---- number -------------------------------------------------------------- */

uint8_t js8_pack_num(int num)
{
    if (num == JS8_NUM_NONE) return 0;
    if (num < -30) num = -30;
    if (num > 31)  num = 31;
    return (uint8_t)(num + 31);
}

int js8_unpack_num(uint8_t packed)
{
    if (packed == 0 || packed > 62) return JS8_NUM_NONE;
    return (int)packed - 31;
}

/* ---- frames -------------------------------------------------------------- */

js8_frame_type_t js8_frame_type(const uint8_t frame[JS8_FRAME_BYTES])
{
    uint8_t t3 = (uint8_t)((frame[0] >> 5) & 7u);
    /* ⛔ The data frames flag themselves in TWO bits, not three - the third is
     * payload (see the enum). So 100/101 are one type and 110/111 are one
     * type, and the odd codes must be folded onto the even ones rather than
     * reported as types 5 and 7, which do not exist.
     *
     * This used to return all three bits. The effect was invisible while
     * nothing handled data frames at all - js8_text.c dropped 4..7 alike
     * through its default - and would have become a split-sender bug the
     * moment reassembly started bucketing by type. */
    if (t3 >= 4u) t3 &= 6u;
    return (js8_frame_type_t)t3;
}

bool js8_pack_directed(const js8_directed_t* msg, uint8_t frame[JS8_FRAME_BYTES])
{
    if (!msg || !frame) return false;

    bool pf = false, pt = false;
    uint32_t from = js8_pack_callsign(msg->from, &pf);
    uint32_t to   = js8_pack_callsign(msg->to, &pt);
    if (from == 0 || to == 0) return false;     /* the reference's own test */

    /* A "/P" in the callsign and the flag in the struct are the same fact, so
     * either one sets the bit. */
    pf = pf || msg->portable_from;
    pt = pt || msg->portable_to;

    uint8_t extra = (uint8_t)((pf ? 0x80u : 0u) | (pt ? 0x40u : 0u) |
                              (js8_pack_num(msg->num) & 0x3Fu));

    memset(frame, 0, JS8_FRAME_BYTES);
    int pos = 0;
    put_bits(frame, &pos, JS8_FRAME_DIRECTED, 3);
    put_bits(frame, &pos, from, 28);
    put_bits(frame, &pos, to, 28);
    put_bits(frame, &pos, msg->cmd % 32u, 5);
    put_bits(frame, &pos, extra, 8);
    return pos == 72;
}

bool js8_unpack_directed(const uint8_t frame[JS8_FRAME_BYTES], js8_directed_t* out)
{
    if (!frame || !out) return false;
    memset(out, 0, sizeof(*out));

    int pos = 0;
    if (get_bits(frame, &pos, 3) != JS8_FRAME_DIRECTED) return false;
    uint32_t from = (uint32_t)get_bits(frame, &pos, 28);
    uint32_t to   = (uint32_t)get_bits(frame, &pos, 28);
    out->cmd      = (uint8_t)get_bits(frame, &pos, 5);
    uint8_t extra = (uint8_t)get_bits(frame, &pos, 8);

    out->portable_from = (extra & 0x80u) != 0;
    out->portable_to   = (extra & 0x40u) != 0;
    out->num           = js8_unpack_num(extra & 0x3Fu);

    js8_unpack_callsign(from, out->portable_from, out->from);
    js8_unpack_callsign(to, out->portable_to, out->to);
    return true;
}

bool js8_pack_heartbeat(const js8_heartbeat_t* msg, uint8_t frame[JS8_FRAME_BYTES])
{
    if (!msg || !frame) return false;

    uint64_t call = js8_pack_alnum50(msg->call);
    if (call == 0) return false;

    uint16_t extra = js8_pack_grid(msg->grid);
    if (msg->is_cq) extra |= 0x8000u;

    /* The 16-bit extra is split 11/5 across the frame, with the low 5 bits
     * sharing a byte with the 3-bit wording index. */
    uint16_t hi11 = (uint16_t)((extra >> 5) & 0x7FFu);
    uint8_t  lo5  = (uint8_t)(extra & 0x1Fu);
    uint8_t  last = (uint8_t)((lo5 << 3) | (msg->bits3 & 7u));

    memset(frame, 0, JS8_FRAME_BYTES);
    int pos = 0;
    put_bits(frame, &pos, JS8_FRAME_HEARTBEAT, 3);
    put_bits(frame, &pos, call, 50);
    put_bits(frame, &pos, hi11, 11);
    put_bits(frame, &pos, last, 8);
    return pos == 72;
}

bool js8_unpack_heartbeat(const uint8_t frame[JS8_FRAME_BYTES], js8_heartbeat_t* out)
{
    if (!frame || !out) return false;
    memset(out, 0, sizeof(*out));

    int pos = 0;
    if (get_bits(frame, &pos, 3) != JS8_FRAME_HEARTBEAT) return false;
    uint64_t call = get_bits(frame, &pos, 50);
    uint16_t hi11 = (uint16_t)get_bits(frame, &pos, 11);
    uint8_t  last = (uint8_t)get_bits(frame, &pos, 8);

    uint16_t extra = (uint16_t)((hi11 << 5) | ((last >> 3) & 0x1Fu));
    out->bits3 = (uint8_t)(last & 7u);
    out->is_cq = (extra & 0x8000u) != 0;

    js8_unpack_alnum50(call, out->call);
    js8_unpack_grid((uint16_t)(extra & 0x7FFFu), out->grid);
    return true;
}
