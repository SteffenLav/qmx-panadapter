#ifndef _INCLUDE_JS8_REASM_H_
#define _INCLUDE_JS8_REASM_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* size_t, for js8_reasm_describe */

#include "js8_message.h"
#include "js8_jsc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reassembly of JS8 free-text transmissions (J7).
 *
 * ⛔ WHY A DATA FRAME HAS NO SENDER IN IT. The layout is [1][1][70]: one bit
 * saying this is data, one saying which coding, and seventy bits of payload.
 * There is no callsign field and no sequence number. So a run of data frames
 * can only be tied to a station by WHERE and WHEN it was heard - the audio
 * offset it arrived on, and the slots it arrived in. That is what this module
 * does, and it is the only thing it can do.
 *
 * ⚠ IT USED NOT TO PRODUCE TEXT, and the history is worth one line because
 * the shape of the module still reflects it. The coding tables live only
 * inside JS8Call, which is GPL-3, and while this project was MIT they were
 * out of reach - a 262144-word dictionary is data, not an idea, so no clean
 * reimplementation can reach the same indices. The project relicensed to
 * GPL-3 on 2026-10-09 for exactly this, and js8_jsc.c now decodes each frame
 * as it arrives (see JS8_REASM_TEXT_MAX).
 *
 * ⭐ IT STILL EARNS ITS KEEP WITHOUT TEXT. A frame can decode to nothing -
 * a transmission joined late, or a word index above our truncated table - and
 * then the run still says that OZ9JEP is sending six frames at 1200 Hz,
 * which is the difference between a dead-looking band and a visibly busy one.
 */

/* Offsets drift a few Hz between slots - the candidate search itself
 * quantises to about 6.25 Hz - so frames within this much of each other are
 * taken to be the same station. Wider than this and two stations sharing a
 * busy band segment would merge into one message; narrower and one station's
 * own drift would split into several. */
#define JS8_REASM_FREQ_TOL_HZ   15

/* How many slots of silence close a transmission. A sender pauses between
 * frames more often than you would think - a slot lost to QSB, or JS8Call's
 * own gap before a continuation - so closing on the first missed slot
 * shreds real messages. Four slots is one minute at JS8 Normal. */
#define JS8_REASM_TIMEOUT_SLOTS 4

/* Concurrent transmissions tracked. The panadapter's passband holds a handful
 * of JS8 signals at once, and this is a fixed array on purpose: no malloc on
 * a path fed from the decode task. */
#define JS8_REASM_MAX_ACTIVE    6

/* Frames counted per transmission. A 70-bit frame carries roughly 8-14
 * characters, so 24 frames is a few hundred characters - longer than the
 * messages JS8 operators actually send. Beyond this the run is still counted,
 * it just stops storing, which is reported rather than hidden. */
#define JS8_REASM_MAX_FRAMES    24

/* Accumulated message text. ⛔ THE RAW FRAMES ARE NOT KEPT. Each frame is
 * decoded as it arrives and only the text is held, which is both what the
 * screen wants and cheaper: this struct is a static in ft8_test.c, so it
 * lives in internal .bss, and this project has been bitten by its own .bss
 * before (#65). Holding 24 frames AND their text would have doubled the
 * cost for nothing - nothing re-decodes a frame later. */
#define JS8_REASM_TEXT_MAX      240

#define JS8_REASM_CALL_LEN      14

typedef struct {
    bool     used;
    int      freq_hz;          /* the offset this run was first heard on     */
    int64_t  first_slot;       /* UTC second of its first frame              */
    int64_t  last_slot;        /* UTC second of its most recent frame        */
    int      n_frames;         /* frames STORED (see n_seen for the truth)   */
    int      n_seen;           /* frames seen, including any not stored      */
    int      n_dup;            /* repeats of the previous frame, same slot   */
    /* ⛔ THE LAST FRAME, kept ONLY to recognise a repeat of itself.
     *
     * The decoder finds the same transmission as several candidates a few Hz
     * apart, every one of which passes CRC-12 and arrives here as a separate
     * fragment. Measured on air 2026-10-09: HB9BV's free text came out as
     * "MSG ID 178MSG ID 178MSG ID 178MSG ID 178" - one message, four
     * candidates, in a single slot.
     *
     * 9 bytes, and only the LAST frame: duplicates of one signal arrive
     * consecutively because a run is already keyed by audio offset, so a
     * different station's frame goes to a different run and cannot come
     * between them. A repeat separated by a genuinely different fragment is
     * NOT caught, and that is deliberate - it is also the shape of a station
     * legitimately repeating itself across a long message. */
    uint8_t  last_frame[JS8_FRAME_BYTES];
    int64_t  last_frame_slot;
    bool     has_last;
    bool     overflowed;       /* n_seen > JS8_REASM_MAX_FRAMES              */
    bool     compressed;       /* the coding the FIRST frame declared        */
    bool     mixed_coding;     /* ⚠ later frames disagreed with it           */
    char     sender[JS8_REASM_CALL_LEN];   /* "" when never identified       */
    /* The message so far. Frames are decoded on arrival and concatenated;
     * a frame that decodes to nothing contributes nothing but is still
     * counted in n_seen, so "6 frames, no text" is a visible state rather
     * than a silent one. */
    char     text[JS8_REASM_TEXT_MAX];
    int      n_text;
    bool     text_full;        /* text was cut because the buffer filled     */
} js8_reasm_run_t;

typedef struct {
    js8_reasm_run_t runs[JS8_REASM_MAX_ACTIVE];
    /* Last station identified at each offset by a frame that DOES carry a
     * callsign, so a later data frame on the same offset can be attributed.
     * Separate from runs[] deliberately: the identifying frame usually
     * arrives in a slot BEFORE any data frame exists to hang it on. */
    struct {
        bool    used;
        int     freq_hz;
        int64_t slot;
        char    call[JS8_REASM_CALL_LEN];
    } ident[JS8_REASM_MAX_ACTIVE];
    int dropped_no_slot;       /* runs refused because the table was full */
} js8_reasm_t;

void js8_reasm_init(js8_reasm_t* r);

/* Feed EVERY decoded frame, data or not. Non-data frames are not stored; they
 * are read for the callsign that will attribute a later data frame, which is
 * why they must not be filtered out by the caller. `sender` is the callsign
 * the caller already unpacked, or NULL if it has none.
 *
 * Returns true if the frame was taken as part of a data run. */
bool js8_reasm_add(js8_reasm_t* r, int64_t slot_utc, int freq_hz,
                   const uint8_t frame[JS8_FRAME_BYTES],
                   js8_frame_type_t type, const char* sender);

/* Call once per slot, after every frame of that slot has been added. Closes
 * runs that have gone quiet for JS8_REASM_TIMEOUT_SLOTS and forgets stale
 * identifications. Closing is what frees a slot in the table, so a caller
 * that never ticks will eventually refuse new runs - counted in
 * dropped_no_slot rather than failing silently. */
void js8_reasm_tick(js8_reasm_t* r, int64_t slot_utc);

/* Live runs, for the screen. Index is not stable across a tick. */
int  js8_reasm_active(const js8_reasm_t* r);
const js8_reasm_run_t* js8_reasm_at(const js8_reasm_t* r, int i);

/* One line per live run for the decode list, e.g.
 *     "OZ9JEP  [data 6f]"      identified
 *     "1200 Hz [data 3f]"      not identified
 * ⚠ Deliberately NOT shaped like a decoded message: it carries no text and
 * must not read as though the words were received and are being withheld. */
bool js8_reasm_describe(const js8_reasm_run_t* run, char* out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* _INCLUDE_JS8_REASM_H_ */
