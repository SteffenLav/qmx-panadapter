#include "ft8_screen.h"

#include <string.h>
#include <strings.h>  // strcasecmp - ft8_screen_find_call
#include <stdio.h>
#include <stdlib.h>   // qsort - shared decode-list ordering
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ft8_tx.h"    // ft8_tx_get_parity_lock() - pause aging on our TX parity
#include "ft8_test.h"  // ft8_op_mode_slot_ms() - protocol slot grid for row parity
#include "storage/settings.h"  // ft8_filters_t.max_age_sec - operator-tunable row aging

static const char *TAG = "ft8_screen";

// The decode list is a live picture of who is transmitting *now*, not a log.
// A station that hasn't been re-decoded within this many seconds is expired
// (you can't work a signal that's already gone). ~90 s keeps stations that are
// actively calling/working (they transmit every 15-30 s) through fades and
// their own QSO exchanges, while still dropping stations that have left.
// History: 60 s -> 120 s (2026-07-15) because the short window churned on
// marginal signals AND rows died mid-CQ-run; the CQ-run churn later got its
// real fix (parity-aware aging pause, #46). 120 -> 90 (2026-07-19) once the
// #51 ISO-pipeline fix restored ~16 unique decodes/slot: 120 s now keeps far
// too large a crowd on screen, so 90 s tightens "Active" back to who is
// genuinely here now. Tunable - and now literally tunable, by the operator,
// from the Filter modal's "Max age in list" dropdown (30/45/60/75/90 s);
// this is the fallback used when that setting has never been written (a
// pre-existing NVS blob reads back 0, which must NOT be read as "expire
// instantly" - see row_stale_sec() below).
#define FT8_ROW_STALE_SEC   90

// Reads the operator's chosen max-age, or FT8_ROW_STALE_SEC if it has never
// been set (0 = an NVS blob from before this setting existed). Computed once
// per caller, before the mutex, same discipline as tx_even/tx_lock/per_ms
// just above each call site - settings_load_all() is a cheap RAM-cached
// read (no flash I/O), so this is safe to call every tick.
static int row_stale_sec(void)
{
    qmx_settings_t qs;
    settings_load_all(&qs);
    return qs.ft8_filters.max_age_sec ? (int)qs.ft8_filters.max_age_sec : FT8_ROW_STALE_SEC;
}

// Hard ceiling on the parity-aware aging PAUSE above. The pause itself is right
// - while we transmit over a station's slot its silence tells us nothing - but it
// had no upper bound, and during a CQ run we are parity-locked essentially all
// the time. So every row on our TX parity was kept FOREVER: ten minutes, an
// hour, whatever the session lasted.
//
// That is not cosmetic. build_tone_occupancy() (ft8_tx.c) reads this same table
// and filters it to OUR parity - precisely the rows being retained - so the
// occupancy strip filled up and stayed full, the automatic picker ran out of
// "free" tones, and only a restart cleared it. Field-reported by Roy KI0ER
// (2026-08-05): "the offset strip turns totally red ... all slots taken", cured
// by restarting, which is exactly what an unbounded retain looks like.
//
// 10 minutes: long enough to ride out any realistic CQ run or exchange without
// the list churning, short enough that a station who left is not still claiming
// a frequency. A stale "occupied" is worse than a missing row - it steers us
// away from a tone that is actually free.
#define FT8_ROW_PAUSED_MAX_SEC 600

static ft8_call_t s_table[FT8_CALL_TABLE_SIZE];
static SemaphoreHandle_t s_mutex = NULL;

// Extract the transmitter callsign from an FT8 message.
//
// FT8 message forms:
//   "CQ K1ABC"             -> K1ABC  (plain CQ, 2nd token)
//   "CQ DX K1ABC"          -> K1ABC  (directional CQ, 3rd token)
//   "CQ NA K1ABC"          -> K1ABC  (continent-directional CQ)
//   "CQ POTA K1ABC"        -> K1ABC  (arbitrary directional)
//   "K9XYZ K1ABC FN42"     -> K1ABC  (QSO reply, source = 2nd token)
//   "K9XYZ K1ABC -10"      -> K1ABC  (signal report)
//
// Rule (matches WSJT-X heuristic in packjt77.f90): if the 1st token
// is exactly "CQ" and the 2nd token contains no digit, treat it as
// a directional qualifier and take the 3rd token. Otherwise take
// the 2nd token. Real callsigns nearly always contain a digit;
// directional tokens (DX, NA, EU, AS, AF, SA, OC, POTA, etc.) do not.
static bool token_has_digit(const char *t, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (t[i] >= '0' && t[i] <= '9') return true;
    }
    return false;
}

bool ft8_screen_extract_call(const char *msg, char *out_call, size_t cap)
{
    // Locate token starts/lengths for up to 3 tokens.
    const char *t[3] = {NULL, NULL, NULL};
    size_t      len[3] = {0, 0, 0};
    int n = 0;
    while (*msg == ' ') msg++;
    while (*msg && n < 3) {
        t[n] = msg;
        size_t l = 0;
        while (msg[l] && msg[l] != ' ') l++;
        len[n] = l;
        n++;
        msg += l;
        while (*msg == ' ') msg++;
    }
    if (n < 2) return false;

    // Pick source token: 3rd if 1st is "CQ" and 2nd has no digit.
    int pick = 1;  // default 2nd token
    bool first_is_cq = (len[0] == 2 && t[0][0] == 'C' && t[0][1] == 'Q');
    if (first_is_cq && n >= 3 && !token_has_digit(t[1], len[1])) {
        pick = 2;
    }

    size_t out_len = len[pick];
    if (out_len >= cap) out_len = cap - 1;
    memcpy(out_call, t[pick], out_len);
    out_call[out_len] = '\0';

    // A hash-resolved callsign arrives in <angle brackets> ("<PJ4/K9XYZ>",
    // see ft8_hash.c) - strip them so the station table keys on the bare
    // call and matches the QSO machine's target / worked-before / grid
    // lookups. An unresolved "<...>" becomes "...", which matches nothing.
    if (out_len >= 2 && out_call[0] == '<' && out_call[out_len - 1] == '>') {
        memmove(out_call, out_call + 1, out_len - 2);
        out_call[out_len - 2] = '\0';
        out_len -= 2;
    }
    return out_len > 0;
}

// Caller must hold s_mutex.
static ft8_call_t *find_or_evict(const char *call)
{
    int free_idx   = -1;
    int oldest_idx = 0;
    int64_t oldest_utc = s_table[0].last_utc;
    for (int i = 0; i < FT8_CALL_TABLE_SIZE; i++) {
        if (s_table[i].occupied) {
            if (strncmp(s_table[i].call, call, FT8_CALL_MAX_LEN) == 0) {
                return &s_table[i];
            }
            if (s_table[i].last_utc < oldest_utc) {
                oldest_utc = s_table[i].last_utc;
                oldest_idx = i;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx >= 0) return &s_table[free_idx];
    memset(&s_table[oldest_idx], 0, sizeof(ft8_call_t));
    return &s_table[oldest_idx];
}

void ft8_screen_init(void)
{
    memset(s_table, 0, sizeof(s_table));
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "FT8 screen data layer ready (%d slots, ~%u B, mutex=%p)",
             FT8_CALL_TABLE_SIZE, (unsigned)sizeof(s_table), s_mutex);
}

// Parse a Maidenhead grid out of an FT8 message. Grids appear as the
// last whitespace-separated token of a CQ message ("CQ K1ABC FN42")
// or the 3rd token of an exchange ("K1ABC K9XYZ FN42"). They are 4 or
// 6 chars: 2 letters + 2 digits + (optional) 2 letters. Anything that
// doesn't match (e.g. signal reports "-10", "R+05", "73", "RR73") is
// ignored. Writes "" to out_grid when no grid is found.
static bool token_looks_like_grid(const char *t, size_t len)
{
    if (len != 4 && len != 6) return false;
    // "RR73" is a reserved FT8 roger/73 token, not a grid square, even
    // though it syntactically matches AA00..RR99 (R is a valid field letter).
    if (len == 4 && t[0] == 'R' && t[1] == 'R' && t[2] == '7' && t[3] == '3') return false;
    if (t[0] < 'A' || t[0] > 'R') return false;
    if (t[1] < 'A' || t[1] > 'R') return false;
    if (t[2] < '0' || t[2] > '9') return false;
    if (t[3] < '0' || t[3] > '9') return false;
    if (len == 6) {
        // Subsquare letters can be upper or lower a..x; we'll see them upper
        // from the wire, but be tolerant.
        char c4 = t[4]; if (c4 >= 'a' && c4 <= 'z') c4 = (char)(c4 - 32);
        char c5 = t[5]; if (c5 >= 'a' && c5 <= 'z') c5 = (char)(c5 - 32);
        if (c4 < 'A' || c4 > 'X') return false;
        if (c5 < 'A' || c5 > 'X') return false;
    }
    return true;
}

void ft8_screen_extract_grid(const char *msg, char *out_grid, size_t cap)
{
    out_grid[0] = '\0';
    // Scan to last token.
    const char *p = msg;
    const char *last_tok = NULL;
    size_t last_len = 0;
    while (*p == ' ') p++;
    while (*p) {
        const char *tok = p;
        size_t l = 0;
        while (p[l] && p[l] != ' ') l++;
        last_tok = tok;
        last_len = l;
        p += l;
        while (*p == ' ') p++;
    }
    if (last_tok && token_looks_like_grid(last_tok, last_len)) {
        size_t n = last_len;
        if (n >= cap) n = cap - 1;
        memcpy(out_grid, last_tok, n);
        out_grid[n] = '\0';
    }
}

void ft8_screen_record_decode(const char *text,
                              int score, int snr_db, int freq_off,
                              int64_t utc_sec, int dt_ms)
{
    char call[FT8_CALL_MAX_LEN];
    if (!ft8_screen_extract_call(text, call, sizeof(call))) {
        return;
    }
    // ⛔ THE WRITER MUST WAIT. A reader that gives up simply redraws a tick
    // later; a decode dropped here is gone for good - the station never enters
    // the table, so it is missing from the list, from the occupancy map the
    // auto-answer gate reads, and from scan_for_reply_to_me(). A dropped
    // message addressed to US is a stalled QSO step, and nothing marks it as
    // anything other than the partner having gone quiet.
    //
    // The old 50 ms was far too short for what is at stake. Measured over a
    // 54,142-decode soak: 99 drops (0.18%), in runs of two or three, each
    // exactly 56 ms apart - i.e. the timeout firing back to back rather than
    // one long stall. Cause, from an out-of-order log timestamp in the same
    // burst: the holder's own "UPD" line landed ~190 ms after it took the
    // mutex. BOTH decode tasks run at priority 1, the lowest on the board, so
    // the holder was simply not scheduled while the capture task, fft_task (4),
    // the feeds (2) and httpd (5) all ran above it. Priority inheritance cannot
    // help - there is no inversion to correct when holder and waiter share a
    // priority; the holder needs CPU, not a boost.
    //
    // So this is CPU starvation, not lock contention, and the answer is to
    // outwait it: every observed stall was ~190 ms, and 2 s leaves an order of
    // magnitude of margin while still bounded, so a holder that genuinely died
    // cannot wedge the decode task forever. Raising the decode priority instead
    // is the #51 trap - it would starve fft_task, which is far worse.
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "record_decode: mutex timeout after 2s, dropping '%s'", text);
        return;
    }
    ft8_call_t *e = find_or_evict(call);
    bool first_time = !e->occupied;
    if (first_time) {
        strncpy(e->call, call, FT8_CALL_MAX_LEN - 1);
        e->call[FT8_CALL_MAX_LEN - 1] = '\0';
        e->heard_count = 0;
        e->occupied = true;
    }
    size_t n = strlen(text);
    if (n >= FT8_TEXT_MAX_LEN) n = FT8_TEXT_MAX_LEN - 1;
    memcpy(e->last_text, text, n);
    e->last_text[n] = '\0';
    e->last_utc   = utc_sec;
    e->last_score = (int16_t)score;
    e->last_snr_db = (int16_t)snr_db;
    e->last_freq  = (int16_t)freq_off;
    e->last_dt_ms = (int16_t)dt_ms;
    // Update grid ONLY if this message contains one - never clobber a stored
    // grid with empty when later messages omit it. A leftover unguarded
    // extract straight into e->last_grid used to do exactly that: every
    // post-answer message (report/RR73/73 carries no grid) erased the grid
    // their initial CQ/answer had provided, so by QSO completion the ADIF
    // lookup found it empty - GRIDSQUARE missing from ~90% of CQ-run logs
    // (John W5JSS field report, v1.0.1 through v1.3.1).
    char new_grid[FT8_GRID_MAX_LEN];
    ft8_screen_extract_grid(text, new_grid, FT8_GRID_MAX_LEN);
    if (new_grid[0]) {
        memcpy(e->last_grid, new_grid, FT8_GRID_MAX_LEN);
    }
    e->heard_count++;
    ESP_LOGI(TAG, "%s call=%-10s heard=%u score=%d freq=%d text='%s'",
             first_time ? "NEW " : "UPD ",
             e->call, e->heard_count, e->last_score, e->last_freq,
             e->last_text);
    if (s_mutex) xSemaphoreGive(s_mutex);
}

/* Look ONE station up by callsign. Exists so a caller that needs what a station
 * last SENT does not have to snapshot the whole table to find out: that snapshot
 * is ~11 KB of ft8_call_t, which CLAUDE.md is emphatic must never sit on a stack
 * on this board, and a 64-entry scan under the same mutex is cheaper anyway.
 *
 * Deliberately does NOT expire stale rows the way get_all() does. This answers
 * "what did they last send", and a caller drained from the pileup queue may
 * legitimately have been heard several minutes ago - which is exactly the case
 * that needs it (#172). Freshness is the caller's business; the pileup queue has
 * its own ageing. */
bool ft8_screen_find_call(const char *call, ft8_call_t *out)
{
    if (!call || !call[0] || !out) return false;
    bool found = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "find_call: mutex timeout");
        return false;
    }
    for (int i = 0; i < FT8_CALL_TABLE_SIZE; i++) {
        if (!s_table[i].occupied) continue;
        if (strcasecmp(s_table[i].call, call) != 0) continue;
        *out = s_table[i];
        found = true;
        break;
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    return found;
}

void ft8_screen_get_all(ft8_call_t *out, int max, int *count_out)
{
    int n = 0;
    if (count_out) *count_out = 0;
    if (!out || max <= 0) return;
    // While our own TX is parity-locked (CQ run / QSO exchange), rows whose
    // last decode landed on OUR TX parity cannot be re-heard - we transmit
    // over every slot we'd decode them in - so their silence carries no
    // information and their aging clock pauses. Without this, every such
    // station crosses the stale threshold together ~60 s into a run and the
    // list visibly empties in one refresh tick. Queried BEFORE s_mutex so we
    // never hold two locks at once (ft8_tx has its own).
    bool tx_even  = false;
    bool tx_lock  = ft8_tx_get_parity_lock(&tx_even);
    int  per_ms   = ft8_op_mode_slot_ms();
    int  stale_sec = row_stale_sec();
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "get_all: mutex timeout");
        return;
    }
    // Expire stale stations as we snapshot: anything not re-decoded within
    // stale_sec is freed here, so it vanishes from the view, the active
    // count, and the CQ clear-frequency scan in one place. last_utc and now are
    // both UTC seconds (slot start vs wall clock); valid because FT8 mode gates
    // on SNTP sync. Scan the whole table (not limited by max) so purging is
    // complete even if the caller's buffer fills.
    int64_t now = (int64_t)time(NULL);
    for (int i = 0; i < FT8_CALL_TABLE_SIZE; i++) {
        if (!s_table[i].occupied) continue;
        if (now - s_table[i].last_utc > stale_sec) {
            if (tx_lock && (now - s_table[i].last_utc) <= FT8_ROW_PAUSED_MAX_SEC) {
                // Row parity on the active protocol's grid (same nearest-slot
                // rounding as ft8_screen_view's E/O indicator).
                int64_t sidx = ((int64_t)s_table[i].last_utc * 1000 + per_ms / 2) / per_ms;
                bool row_even = (sidx % 2) == 0;
                if (row_even == tx_even) {
                    // Our TX parity: aging paused, keep the row visible.
                    if (n < max) out[n++] = s_table[i];
                    continue;
                }
            }
            s_table[i].occupied = false;   // station went quiet — drop it
            continue;
        }
        if (n < max) out[n++] = s_table[i];
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    if (count_out) *count_out = n;
}

int ft8_screen_active_count(void)
{
    // Count-only companion to ft8_screen_get_all(), for callers that want the
    // number and nothing else - notably the context-help triage, which runs on
    // taskLVGL where an ft8_call_t table (~11 KB) on the stack is a crash, not a
    // slow path. Same "count-only accessor" pattern as settings_wifi_known_count().
    //
    // Deliberately READ-ONLY: it applies the same visibility rule as get_all() but
    // never purges. Asking "how many stations are live" from a help panel must not
    // mutate the decode list.
    bool tx_even = false;
    bool tx_lock = ft8_tx_get_parity_lock(&tx_even);
    int  per_ms  = ft8_op_mode_slot_ms();
    int  stale_sec = row_stale_sec();
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "active_count: mutex timeout");
        return 0;
    }
    int64_t now = (int64_t)time(NULL);
    int n = 0;
    for (int i = 0; i < FT8_CALL_TABLE_SIZE; i++) {
        if (!s_table[i].occupied) continue;
        if (now - s_table[i].last_utc > stale_sec) {
            if (!(tx_lock && (now - s_table[i].last_utc) <= FT8_ROW_PAUSED_MAX_SEC)) continue;
            int64_t sidx = ((int64_t)s_table[i].last_utc * 1000 + per_ms / 2) / per_ms;
            if (((sidx % 2) == 0) != tx_even) continue;   // not our parity: genuinely stale
        }
        n++;
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    return n;
}

void ft8_screen_clear(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "clear: mutex timeout");
        return;
    }
    memset(s_table, 0, sizeof(s_table));
    ESP_LOGI(TAG, "decode list cleared");
    if (s_mutex) xSemaphoreGive(s_mutex);
}

// ---------------------------------------------------------------------------
// Ordering: one implementation, callable from any task
// ---------------------------------------------------------------------------
//
// The decode list's order is a POLICY - "what is the operator looking for" -
// and it now has two consumers: the Tab5's own list (ft8_screen_view.c) and the
// browser's (net/webserver.c /api/decodes). Two copies would drift, and the
// first symptom would be the two screens disagreeing about which station is at
// the top during a QSO, which is exactly what the pin was added to fix.
//
// The tiers, most wanted first:
//   1. the station we are working  - their reply, their CQ, even a message they
//      send to a third station, which is when you most want to see them
//   2. anything addressed to us
//   3. CQ calls
//   4. everything else, strongest signal first
//
// qsort() takes no context pointer, so the two keys live in file statics - which
// is why this function owns a mutex rather than exposing a bare comparator: the
// LVGL thread and the HTTP task must not be mid-sort in each other's keys.
static SemaphoreHandle_t s_sort_mutex = NULL;
static char s_sort_pin[FT8_CALL_MAX_LEN];
static char s_sort_me[FT8_CALL_MAX_LEN];

static int cmp_ordered(const void *a, const void *b)
{
    const ft8_call_t *ca = (const ft8_call_t *)a;
    const ft8_call_t *cb = (const ft8_call_t *)b;
    bool a_pin = s_sort_pin[0] && strcmp(ca->call, s_sort_pin) == 0;
    bool b_pin = s_sort_pin[0] && strcmp(cb->call, s_sort_pin) == 0;
    if (a_pin != b_pin) return b_pin ? 1 : -1;
    bool a_me = s_sort_me[0] && strstr(ca->last_text, s_sort_me);
    bool b_me = s_sort_me[0] && strstr(cb->last_text, s_sort_me);
    if (a_me != b_me) return b_me ? 1 : -1;
    bool a_cq = (strncmp(ca->last_text, "CQ ", 3) == 0);
    bool b_cq = (strncmp(cb->last_text, "CQ ", 3) == 0);
    if (a_cq != b_cq) return b_cq ? 1 : -1;
    if (cb->last_snr_db > ca->last_snr_db) return  1;
    if (cb->last_snr_db < ca->last_snr_db) return -1;
    return 0;
}

void ft8_screen_sort_rows(ft8_call_t *rows, int n,
                          const char *my_call, const char *pin_call)
{
    if (!rows || n <= 1) return;
    if (!s_sort_mutex) {
        s_sort_mutex = xSemaphoreCreateMutex();
        if (!s_sort_mutex) return;          // sort unsorted rather than race
    }
    if (xSemaphoreTake(s_sort_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "sort_rows: mutex timeout - leaving order untouched");
        return;
    }
    snprintf(s_sort_pin, sizeof(s_sort_pin), "%s", pin_call ? pin_call : "");
    snprintf(s_sort_me,  sizeof(s_sort_me),  "%s", my_call  ? my_call  : "");
    qsort(rows, n, sizeof(ft8_call_t), cmp_ordered);
    xSemaphoreGive(s_sort_mutex);
}
