#pragma once

/* The JS8 conversation stream - what the JS8 page shows.
 *
 * WHY THIS EXISTS AND js8_reasm DOES NOT ANSWER IT. js8_reasm tracks
 * transmissions that are IN FLIGHT and forgets a run JS8_REASM_TIMEOUT_SLOTS
 * (60 s) after its last frame, because its job is to join fragments. A page an
 * operator opens a minute later must still show what was said, so the text has
 * to be copied somewhere that outlives the run. This is that somewhere.
 *
 * ⭐ IT ALSO CARRIES THE STRUCTURED TRAFFIC, not only free text. Free-text
 * frames are rare - one in 82 slots on the bench on 2026-10-09 - so a page
 * holding only those would be empty almost every time it is opened, and an
 * empty page teaches the operator to stop opening it. Heartbeats and directed
 * messages are the same conversation; the kind field says which is which.
 *
 * ⛔ IT LIVES IN PSRAM. Internal heap on this board runs at about 30 KB free
 * (heap_i in the slot line) and this project has been bitten by its own .bss
 * before (#65). js8_chat_init() allocates; every accessor is safe to call
 * before it and simply reports an empty store, so a failed allocation costs
 * the page and nothing else.
 *
 * ⚠ NO LOCK, deliberately and with the same reasoning js8_reasm records: the
 * writer is the decode task after its worker has joined, the readers are the
 * LVGL thread and the HTTP status handler, and every field is written before
 * the slot that publishes it. A torn read costs one repaint of one row.
 */

#include <stdbool.h>
#include <stdint.h>

/* Messages retained. 32 at one line each fills a 1280x720 page twice over,
 * and the whole store is about 9 KB of PSRAM. */
#define JS8_CHAT_MAX_MSGS   32
/* Matches JS8_REASM_TEXT_MAX - a run's text is copied in whole or not at all,
 * because a message cut in two places (here and there) cannot be read back. */
#define JS8_CHAT_TEXT_MAX   240
#define JS8_CHAT_CALL_LEN   14

typedef enum {
    JS8_CHAT_FREETEXT = 0,  /* joined from data frames by js8_reasm   */
    JS8_CHAT_DIRECTED,      /* a structured message with a callsign   */
} js8_chat_kind_t;

typedef struct {
    js8_chat_kind_t kind;
    int64_t first_utc;      /* slot of the first frame - half the identity  */
    int64_t last_utc;       /* slot of the most recent frame                */
    int     freq_hz;        /* audio offset - the other half of the identity */
    int     frames;         /* frames seen (1 for a structured message)     */
    int     snr_db;         /* structured only; INT16_MIN when not known    */
    bool    active;         /* still in flight - the page marks it          */
    bool    truncated;      /* text was cut: the buffer or the codebook     */
    bool    compressed;     /* the coding the first frame declared          */
    char    sender[JS8_CHAT_CALL_LEN];   /* "" when never identified        */
    char    text[JS8_CHAT_TEXT_MAX];
} js8_chat_msg_t;

#define JS8_CHAT_NO_SNR  (-32768)

/* Allocate the store. Safe to call more than once; returns false only when
 * PSRAM could not provide it, after which every other call is a no-op. */
bool js8_chat_init(void);

/* Add or update a message, keyed on (first_utc, freq_hz).
 *
 * ⛔ UPSERT, NOT APPEND. A free-text run grows over several slots and is fed
 * again every slot it takes a frame in; appending would print the same
 * sentence once per slot, which is the defect already fixed once in the slot
 * log (9f386b88). The key is the run's FIRST slot, which does not change as
 * it grows. */
void js8_chat_add(const js8_chat_msg_t *m);

/* Clear `active` on anything that has not been fed since `now_utc - quiet_s`.
 * Called once per slot, so the page stops marking a finished run as live. */
void js8_chat_tick(int64_t now_utc, int quiet_s);

/* Newest first: index 0 is the most recent message. */
int  js8_chat_count(void);
bool js8_chat_at(int i, js8_chat_msg_t *out);

void js8_chat_clear(void);
