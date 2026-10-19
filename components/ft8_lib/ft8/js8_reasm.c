#include "js8_reasm.h"

#include <stdio.h>
#include <string.h>

/* See js8_reasm.h for what this module is. It decodes each data frame as it
 * arrives (js8_jsc.c) and keeps only the text; the raw frames are not stored,
 * because nothing re-decodes them and this struct lives in internal .bss. */

/* Both sides of this copy live inside the same js8_reasm_t, so snprintf is
 * rejected under -Werror=restrict: the compiler cannot prove the source and
 * destination do not overlap. They never do - ident[] and runs[] are
 * different arrays - but a bounded copy is the right answer anyway. */
static void copy_call(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0;
    if (!dst || dst_len == 0) return;
    if (src) for (; i + 1 < dst_len && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int freq_near(int a, int b)
{
    int d = a - b;
    if (d < 0) d = -d;
    return d <= JS8_REASM_FREQ_TOL_HZ;
}

void js8_reasm_init(js8_reasm_t* r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

/* The run on this offset, or NULL. Nearest match rather than first match:
 * with a 15 Hz tolerance two runs can both be "near" a new frame, and giving
 * it to whichever happened to sit lower in the array would steal frames from
 * one sender and hand them to another. */
static js8_reasm_run_t* find_run(js8_reasm_t* r, int freq_hz)
{
    js8_reasm_run_t* best = NULL;
    int best_d = 0;
    for (int i = 0; i < JS8_REASM_MAX_ACTIVE; i++) {
        js8_reasm_run_t* run = &r->runs[i];
        if (!run->used) continue;
        int d = run->freq_hz - freq_hz;
        if (d < 0) d = -d;
        if (d > JS8_REASM_FREQ_TOL_HZ) continue;
        if (!best || d < best_d) { best = run; best_d = d; }
    }
    return best;
}

static void remember_ident(js8_reasm_t* r, int64_t slot, int freq_hz,
                           const char* call)
{
    if (!call || !call[0]) return;
    /* Reuse the entry on this offset when it is the SAME station re-identifying,
     * or when a LATER slot has genuinely replaced an earlier occupant of the
     * frequency - the newest identification wins there, because the station
     * that has just taken the offset is the one whose data frames follow.
     *
     * ⛔ BUT NOT when a DIFFERENT callsign turns up in the SAME slot. Those two
     * stations coexist; neither replaced the other, and overwriting silently
     * handed one station's offset to the other. Measured by the harness
     * 2026-10-10: OZ7TKM on 1496 and OZ2LAV on 1502, 6 Hz apart in one slot,
     * left ONE entry reading OZ2LAV - so a data frame between them was
     * attributed with total confidence to a station picked by arrival order.
     * Letting both stand is what allows lookup_ident() to see the ambiguity
     * and decline to name anyone. */
    for (int i = 0; i < JS8_REASM_MAX_ACTIVE; i++) {
        if (r->ident[i].used && freq_near(r->ident[i].freq_hz, freq_hz)
            && (!strcmp(r->ident[i].call, call) || r->ident[i].slot < slot)) {
            r->ident[i].slot = slot;
            r->ident[i].freq_hz = freq_hz;
            snprintf(r->ident[i].call, sizeof(r->ident[i].call), "%s", call);
            return;
        }
    }
    for (int i = 0; i < JS8_REASM_MAX_ACTIVE; i++) {
        if (!r->ident[i].used) {
            r->ident[i].used = true;
            r->ident[i].slot = slot;
            r->ident[i].freq_hz = freq_hz;
            snprintf(r->ident[i].call, sizeof(r->ident[i].call), "%s", call);
            return;
        }
    }
    /* Table full: drop the oldest, since an identification is only useful
     * while it is recent anyway. */
    int oldest = 0;
    for (int i = 1; i < JS8_REASM_MAX_ACTIVE; i++)
        if (r->ident[i].slot < r->ident[oldest].slot) oldest = i;
    r->ident[oldest].slot = slot;
    r->ident[oldest].freq_hz = freq_hz;
    snprintf(r->ident[oldest].call, sizeof(r->ident[oldest].call), "%s", call);
}

static int ident_near(int a, int b)
{
    int d = a - b;
    if (d < 0) d = -d;
    return d <= JS8_REASM_IDENT_TOL_HZ;
}

/* The callsign to attribute a data run on this offset to, or NULL to leave it
 * unidentified.
 *
 * ⚠ THIS USED TO RETURN THE FIRST MATCH AT THE FULL 15 Hz TOLERANCE, and both
 * halves of that were wrong:
 *
 *  - FIRST, not nearest. find_run() immediately above already carries the
 *    comment explaining why that is wrong ("with a 15 Hz tolerance two runs
 *    can both be near a new frame"), and was fixed. This function was the
 *    same bug left standing in the same file.
 *  - 15 Hz. Measured on air 2026-10-10: OZ7TKM identified on 1494 Hz and our
 *    own anonymous free text on 1506 Hz was displayed as "OZ7TKM: STATUSCHK".
 *    See JS8_REASM_IDENT_TOL_HZ for why attribution is now 8 Hz.
 *
 * AMBIGUITY YIELDS NOTHING. If two DIFFERENT callsigns are both in range, no
 * guess is honest, so the run stays unidentified - the same choice made for
 * the DT heading, which blanks rather than average two distinct senders. Two
 * entries carrying the SAME call are not ambiguous; that is one station whose
 * offset moved. */
static const char* lookup_ident(const js8_reasm_t* r, int freq_hz)
{
    const char* best = NULL;
    int best_d = JS8_REASM_IDENT_TOL_HZ + 1;

    for (int i = 0; i < JS8_REASM_MAX_ACTIVE; i++) {
        if (!r->ident[i].used) continue;
        if (!ident_near(r->ident[i].freq_hz, freq_hz)) continue;

        int d = r->ident[i].freq_hz - freq_hz;
        if (d < 0) d = -d;

        if (best && strcmp(best, r->ident[i].call) != 0) {
            /* A second, DIFFERENT station is just as plausible. Refuse. */
            return NULL;
        }
        if (d < best_d) { best_d = d; best = r->ident[i].call; }
    }
    return best;
}

bool js8_reasm_add(js8_reasm_t* r, int64_t slot_utc, int freq_hz,
                   const uint8_t frame[JS8_FRAME_BYTES],
                   js8_frame_type_t type, const char* sender)
{
    if (!r || !frame) return false;

    /* A frame that carries a callsign is not stored, but it IS the only way a
     * later data frame gets a name, so it must reach here. Callers that
     * filter to data frames only would turn every message anonymous. */
    if (!js8_frame_is_data(type)) {
        remember_ident(r, slot_utc, freq_hz, sender);
        /* It also ends any run on this offset: the station has gone back to
         * structured frames, so whatever free text was in flight is over. */
        js8_reasm_run_t* run = find_run(r, freq_hz);
        if (run) run->last_slot = slot_utc - JS8_REASM_TIMEOUT_SLOTS * 15;
        return false;
    }

    bool compressed = (type == JS8_FRAME_DATA_COMPRESSED);

    js8_reasm_run_t* run = find_run(r, freq_hz);
    if (!run) {
        for (int i = 0; i < JS8_REASM_MAX_ACTIVE; i++) {
            if (!r->runs[i].used) { run = &r->runs[i]; break; }
        }
        if (!run) {
            /* Refusing is better than evicting: evicting would delete a run
             * the screen is already showing in order to start one that may be
             * a single stray frame. Counted so a persistently full table is
             * visible rather than looking like a quiet band. */
            r->dropped_no_slot++;
            return false;
        }
        memset(run, 0, sizeof(*run));
        run->used       = true;
        run->freq_hz    = freq_hz;
        run->first_slot = slot_utc;
        run->compressed = compressed;
        const char* id = lookup_ident(r, freq_hz);
        if (id) copy_call(run->sender, sizeof(run->sender), id);
    }
    else if (compressed != run->compressed) {
        /* ⚠ Both codings in one transmission. JS8Call picks per frame -
         * packDataMessage() takes whichever of Huffman and compressed encodes
         * MORE characters - so this is normal traffic, not corruption, and it
         * must not be treated as a reason to split the run. Flagged only
         * because a renderer would have to switch tables mid-message. */
        run->mixed_coding = true;
    }

    /* ⛔ A REPEAT OF THE PREVIOUS FRAME IN THE SAME SLOT IS ONE SIGNAL, NOT
     * TWO FRAGMENTS. See the last_frame note in js8_reasm.h for the measured
     * case. Counted rather than dropped silently, so a run that is mostly
     * duplicates is visible instead of looking like a long message. */
    if (run->has_last && run->last_frame_slot == slot_utc &&
        memcmp(run->last_frame, frame, JS8_FRAME_BYTES) == 0) {
        run->n_dup++;
        run->last_slot = slot_utc;
        return true;
    }
    memcpy(run->last_frame, frame, JS8_FRAME_BYTES);
    run->last_frame_slot = slot_utc;
    run->has_last = true;

    run->last_slot = slot_utc;
    run->n_seen++;
    if (run->n_frames < JS8_REASM_MAX_FRAMES) {
        run->n_frames++;
    } else {
        run->overflowed = true;
    }

    /* ⭐ DECODE NOW, and keep only the text. Nothing ever re-decodes a frame,
     * so storing the raw 9 bytes as well would cost internal .bss for no
     * reader - and this project has been bitten by its own .bss before (#65).
     *
     * A frame that decodes to nothing is normal, not an error: it can be the
     * tail of a transmission we joined late, or a word coding whose index
     * landed above our truncated table. It still counts in n_seen, so "6
     * frames, no text" is a state the operator can see rather than a silence. */
    if (!run->text_full) {
        char piece[JS8_JSC_TEXT_MAX];
        if (js8_jsc_frame_to_text(frame, piece, sizeof(piece))) {
            size_t pl = strlen(piece);
            size_t room = sizeof(run->text) - 1 - (size_t)run->n_text;
            if (pl > room) { pl = room; run->text_full = true; }
            if (pl) {
                memcpy(run->text + run->n_text, piece, pl);
                run->n_text += (int)pl;
                run->text[run->n_text] = '\0';
            } else {
                run->text_full = true;
            }
        }
    }

    /* A run can be identified part-way through, by an ident that arrived
     * while it was already open. */
    if (!run->sender[0]) {
        const char* id = lookup_ident(r, freq_hz);
        if (id) copy_call(run->sender, sizeof(run->sender), id);
    }
    return true;
}

void js8_reasm_tick(js8_reasm_t* r, int64_t slot_utc)
{
    if (!r) return;
    /* JS8 Normal is 15 s. Expressed in seconds rather than slot counts
     * because the caller's slot numbering is not this module's business and
     * a missed call to tick() must not silently extend every timeout. */
    const int64_t quiet = (int64_t)JS8_REASM_TIMEOUT_SLOTS * 15;

    for (int i = 0; i < JS8_REASM_MAX_ACTIVE; i++) {
        js8_reasm_run_t* run = &r->runs[i];
        if (!run->used) continue;
        if (slot_utc - run->last_slot >= quiet) run->used = false;
    }
    for (int i = 0; i < JS8_REASM_MAX_ACTIVE; i++) {
        if (!r->ident[i].used) continue;
        if (slot_utc - r->ident[i].slot >= quiet) r->ident[i].used = false;
    }
}

int js8_reasm_active(const js8_reasm_t* r)
{
    if (!r) return 0;
    int n = 0;
    for (int i = 0; i < JS8_REASM_MAX_ACTIVE; i++) if (r->runs[i].used) n++;
    return n;
}

const js8_reasm_run_t* js8_reasm_at(const js8_reasm_t* r, int i)
{
    if (!r || i < 0) return NULL;
    int n = 0;
    for (int k = 0; k < JS8_REASM_MAX_ACTIVE; k++) {
        if (!r->runs[k].used) continue;
        if (n == i) return &r->runs[k];
        n++;
    }
    return NULL;
}

bool js8_reasm_describe(const js8_reasm_run_t* run, char* out, size_t out_len)
{
    if (!run || !out || out_len == 0) return false;
    out[0] = '\0';
    if (!run->used) return false;

    /* The frame count is n_seen, not n_frames: what the operator wants to
     * know is how much the station sent, not how much we kept.
     *
     * With text, the text IS the line - a decode list showing "6 frames" next
     * to a message nobody can read would be the worst of both. Without text,
     * the count still says the band is busy, which is the whole reason this
     * module exists. */
    const char* who = run->sender[0] ? run->sender : NULL;
    char tag[16];
    snprintf(tag, sizeof(tag), "%df%s", run->n_seen,
             run->overflowed || run->text_full ? "+" : "");

    if (run->n_text > 0) {
        /* ⛔ THE TRUNCATION MARKER GOES BEFORE THE TEXT, NOT AFTER IT.
         *
         * Measured, because the first version put it after and the harness
         * failed: the caller's buffer is shorter than the message (the decode
         * list passes 64 bytes against 240 of text), so snprintf cut the
         * marker off and a message stopped short read as a complete sentence
         * that merely ended oddly. In front, it cannot be lost - and it
         * carries the frame count, which is the thing worth knowing at
         * exactly the moment the text does not all fit. */
        if (run->overflowed || run->text_full) {
            if (who) snprintf(out, out_len, "%s [%df+]: %s", who,
                              run->n_seen, run->text);
            else     snprintf(out, out_len, "%d Hz [%df+]: %s", run->freq_hz,
                              run->n_seen, run->text);
        } else if (who) {
            snprintf(out, out_len, "%s: %s", who, run->text);
        } else {
            snprintf(out, out_len, "%d Hz: %s", run->freq_hz, run->text);
        }
    } else if (who) {
        snprintf(out, out_len, "%s [data %s]", who, tag);
    } else {
        snprintf(out, out_len, "%d Hz [data %s]", run->freq_hz, tag);
    }
    return true;
}
