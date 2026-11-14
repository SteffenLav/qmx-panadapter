// Host replay of the Tab5 `storage` SPIFFS partition (IDF v5.4.4 SPIFFS core,
// device config) under the firmware's write pattern: the diag persist task
// (held-open append, ~6 KB per 30 s tick, rotate at 128 KB, rotate-on-ENOSPC),
// a QSO record every few ticks, and periodic reboots (mount, gc 64K, check).
//
// MODE 0 = v1.26.2 QSO append (fsync result ignored).
// MODE 1 = T197 (f6d65527/341b8a82): checked commit, cut back on failure,
//          drop diag.0.log + gc 32K, retry once.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spiffs.h"
#include "spiffs_nucleus.h"

#define PART_SZ   0x100000
#define BLK       4096
#define PG        256
static uint8_t flash[PART_SZ];

void spiffs_api_lock(struct spiffs_t *fs)   { (void)fs; }
void spiffs_api_unlock(struct spiffs_t *fs) { (void)fs; }
static s32_t hal_rd(struct spiffs_t *fs, u32_t a, u32_t n, u8_t *d) { (void)fs; memcpy(d, flash + a, n); return SPIFFS_OK; }
static s32_t hal_wr(struct spiffs_t *fs, u32_t a, u32_t n, u8_t *s) { (void)fs; for (u32_t i = 0; i < n; i++) flash[a + i] &= s[i]; return SPIFFS_OK; }
static long n_erase;
static s32_t hal_er(struct spiffs_t *fs, u32_t a, u32_t n) { (void)fs; n_erase++; memset(flash + a, 0xff, n); return SPIFFS_OK; }

static spiffs fs;
static u8_t work[PG * 2];
static u8_t fds[32 * 8];
static u8_t cache[(sizeof(spiffs_cache) + 4 * (sizeof(spiffs_cache_page) + PG))];

static void mount(void)
{
    spiffs_config c = {0};
    c.hal_read_f = hal_rd; c.hal_write_f = hal_wr; c.hal_erase_f = hal_er;
    c.phys_size = PART_SZ; c.phys_addr = 0; c.phys_erase_block = BLK;
    c.log_block_size = BLK; c.log_page_size = PG;
    s32_t r = SPIFFS_mount(&fs, &c, work, fds, sizeof(fds), cache, sizeof(cache), NULL);
    if (r == SPIFFS_ERR_NOT_A_FS) { SPIFFS_format(&fs); r = SPIFFS_mount(&fs, &c, work, fds, sizeof(fds), cache, sizeof(cache), NULL); }
    if (r) { printf("mount failed %d\n", r); exit(1); }
}

static long fsize(const char *p) { spiffs_stat st; return SPIFFS_stat(&fs, p, &st) == 0 ? (long)st.size : -1; }
static u32_t used_kb(void) { u32_t t, u; SPIFFS_info(&fs, &t, &u); return u / 1024; }

static int MODE;
static int RECORD = 190;
static void rec_from_env(void){ if (getenv("REC")) RECORD = atoi(getenv("REC")); }          // bytes per QSO record
static int DIAG_TICK = 6000;
static void fsstat(const char*w){u32_t t,u;SPIFFS_info(&fs,&t,&u);printf("  [%s] used %u KB free_blocks %d alloc_pg %d deleted_pg %d blocks %d\n",w,(unsigned)(u/1024),(int)fs.free_blocks,(int)fs.stats_p_allocated,(int)fs.stats_p_deleted,(int)fs.block_count);}      // diag bytes per 30 s tick
#define DIAG_MAX (128 * 1024)

// ---- diag persist task model ----
static spiffs_file dfd = -1;
static long dbytes;
static int diag_dead;
static void diag_open(void)
{
    dfd = SPIFFS_open(&fs, "diag.log", SPIFFS_O_APPEND | SPIFFS_O_CREAT | SPIFFS_O_WRONLY, 0);
    if (dfd < 0) { SPIFFS_clearerr(&fs); diag_dead = 1; return; }
    diag_dead = 0; dbytes = fsize("diag.log");
}
static void diag_rotate(void)
{
    SPIFFS_close(&fs, dfd); SPIFFS_clearerr(&fs);
    SPIFFS_remove(&fs, "diag.0.log"); SPIFFS_clearerr(&fs);
    SPIFFS_rename(&fs, "diag.log", "diag.0.log"); SPIFFS_clearerr(&fs);
    SPIFFS_gc(&fs, 32 * 1024); SPIFFS_clearerr(&fs);
    dfd = SPIFFS_open(&fs, "diag.log", SPIFFS_O_TRUNC | SPIFFS_O_CREAT | SPIFFS_O_WRONLY, 0);
    if (dfd < 0) { SPIFFS_clearerr(&fs); diag_dead = 1; }
    dbytes = 0;
}
static long diag_lost_rot;
static long YIELD_AT = 0;
static void diag_tick(void)
{
    if (YIELD_AT && fsize("qso.adi") > YIELD_AT && !diag_dead) { SPIFFS_close(&fs, dfd); SPIFFS_clearerr(&fs); SPIFFS_remove(&fs, "diag.log"); SPIFFS_remove(&fs, "diag.0.log"); SPIFFS_clearerr(&fs); diag_dead = 1; }
    if (diag_dead) return;
    static u8_t buf[2048];
    memset(buf, 'd', sizeof buf);
    int left = DIAG_TICK;
    while (left > 0) {
        int n = left > 2048 ? 2048 : left;
        if (SPIFFS_write(&fs, dfd, buf, n) < 0) {
            SPIFFS_clearerr(&fs);
            diag_lost_rot++;
            diag_rotate();               // ENOSPC path
            return;
        }
        left -= n; dbytes += n;
        if (dbytes >= DIAG_MAX) { diag_rotate(); if (diag_dead) return; }
    }
    SPIFFS_fflush(&fs, dfd); SPIFFS_clearerr(&fs);
}

static int COMPACT; static void compact(void); static int integrity_q(int); static void step_check(const char *); static s32_t gc_watch(u32_t, const char *);
static void boot(void)
{
    if (dfd >= 0) { SPIFFS_close(&fs, dfd); SPIFFS_clearerr(&fs); dfd = -1; }
    SPIFFS_unmount(&fs);
    mount();
    if (!getenv("NOGC")) { gc_watch(getenv("BOOTGC") ? atoi(getenv("BOOTGC")) : 65536, "boot"); }
    if (!getenv("NOCHECK")) { int ok0 = integrity_q(0); SPIFFS_check(&fs); SPIFFS_clearerr(&fs); int ok1 = integrity_q(0); if (ok0 && !ok1) printf("** SPIFFS_check at boot broke the log (qso.adi %ld)\n", fsize("qso.adi")); }
    if (COMPACT) compact();
    if (YIELD_AT && fsize("qso.adi") > YIELD_AT) { SPIFFS_remove(&fs, "diag.log"); SPIFFS_remove(&fs, "diag.0.log"); SPIFFS_clearerr(&fs); diag_dead = 1; return; }
    diag_open();
}

// ---- QSO append models ----
// One attempt: newlib-style 128 B chunks, then fsync, then close.
// Returns: 1 landed, 0 failed (reported), and sets *silent when the old code
// would have claimed success but the file did not grow by RECORD.
static int append_once(long before, int *write_err, int *fsync_err, int *close_err)
{
    static u8_t rec[1024];
    memset(rec, 'q', RECORD); rec[RECORD - 1] = '\n';
    *write_err = *fsync_err = *close_err = 0;
    spiffs_file f = SPIFFS_open(&fs, "qso.adi", SPIFFS_O_APPEND | SPIFFS_O_CREAT | SPIFFS_O_WRONLY, 0);
    if (f < 0) { SPIFFS_clearerr(&fs); *write_err = 1; return 0; }
    for (int off = 0; off < RECORD; off += 128) {
        int n = RECORD - off > 128 ? 128 : RECORD - off;
        if (SPIFFS_write(&fs, f, rec + off, n) < 0) { SPIFFS_clearerr(&fs); *write_err = 1; break; }
    }
    if (SPIFFS_fflush(&fs, f) < 0) { SPIFFS_clearerr(&fs); *fsync_err = 1; }
    if (SPIFFS_close(&fs, f) < 0)  { SPIFFS_clearerr(&fs); *close_err = 1; }
    (void)before;
    return !(*write_err || *fsync_err || *close_err);
}

// Proactive compaction through the public API only (what esp_spiffs_info /
// esp_spiffs_gc give the firmware): ask gc to make nearly all non-allocated
// space free. That lands inside spiffs_gc_check()'s working window, so it
// collects deleted pages while there are still free blocks to move into.
// COMPACT: 0 off, 1 at boot, 2 at boot + after each QSO
static long compact_calls, compact_erases_max;
static void compact(void)
{
    u32_t t, u; SPIFFS_info(&fs, &t, &u);
    long req = (long)t - (long)u - 2 * BLK;
    if (req <= 0) return;
    long e0 = n_erase;
    SPIFFS_gc(&fs, (u32_t)req); SPIFFS_clearerr(&fs);
    long d = n_erase - e0;
    if (d > compact_erases_max) compact_erases_max = d;
    compact_calls++;
}

static long q_ok, q_fail_reported, q_silent_loss, q_valve, q_valve_saved;
static long first_fail_size = -1; static int cleared; static long clear_ok0;

#include "mode2.inc"
#include "gcwrap.inc"
#include "step.inc"
#include "mode7.inc"
static long n_attempt;
static void qso(void)
{
    if (MODE == 2) { qso2(); return; }
    if (MODE == 3) { qso3(1); return; }
    if (MODE == 4) { qso3(0); return; }
    if (MODE == 5) { qso5(); return; }
    if (MODE == 6) { qso6(); return; }
    if (MODE == 7) { qso7(n_attempt); return; }
    long before = fsize("qso.adi"); if (before < 0) before = 0;
    int we, fe, ce;
    int ok = append_once(before, &we, &fe, &ce);
    long after = fsize("qso.adi");
    if (MODE == 0) {
        // v1.26.2: success unless ferror (write) or fclose failed.
        int claimed = !(we || ce);
        if (claimed && after == before + RECORD) q_ok++;
        else if (claimed) { q_silent_loss++; if (first_fail_size < 0) first_fail_size = before; }
        else { q_fail_reported++; if (first_fail_size < 0) first_fail_size = before; }
        return;
    }
    if (ok && after == before + RECORD) { q_ok++; if (COMPACT == 2) compact(); return; }
    if (ok) { printf("!! T197 claimed success but file %ld -> %ld\n", before, after); exit(2); }
    if (first_fail_size < 0) first_fail_size = before;
    // cut back, valve, retry once
    if (fsize("qso.adi") != before) { if (SPIFFS_open(&fs, "x", 0, 0) < 0) SPIFFS_clearerr(&fs); }
    { spiffs_file t = SPIFFS_open(&fs, "qso.adi", SPIFFS_O_RDWR, 0);
      if (t >= 0) { SPIFFS_ftruncate(&fs, t, before); SPIFFS_close(&fs, t); } SPIFFS_clearerr(&fs); }
    q_valve++;
    SPIFFS_remove(&fs, "diag.0.log"); SPIFFS_clearerr(&fs);
    SPIFFS_gc(&fs, 32 * 1024); SPIFFS_clearerr(&fs);
    if (fsize("qso.adi") != before) {
        spiffs_file t = SPIFFS_open(&fs, "qso.adi", SPIFFS_O_RDWR, 0);
        if (t >= 0) { SPIFFS_ftruncate(&fs, t, before); SPIFFS_close(&fs, t); } SPIFFS_clearerr(&fs);
    }
    ok = append_once(before, &we, &fe, &ce);
    after = fsize("qso.adi");
    if (ok && after == before + RECORD) { q_ok++; q_valve_saved++; return; }
    if (ok) { printf("!! T197 retry claimed success but file %ld -> %ld\n", before, after); exit(2); }
    { spiffs_file t = SPIFFS_open(&fs, "qso.adi", SPIFFS_O_RDWR, 0);
      if (t >= 0) { SPIFFS_ftruncate(&fs, t, before); SPIFFS_close(&fs, t); } SPIFFS_clearerr(&fs); }
    long cut = fsize("qso.adi");
    if (cut != before) printf("!! cut back failed: %ld != %ld\n", cut, before);
    q_fail_reported++;
}

int main(int argc, char **argv)
{
    MODE        = argc > 1 ? atoi(argv[1]) : 1;
    int ticks_per_qso = argc > 2 ? atoi(argv[2]) : 5;
    int boot_every    = argc > 3 ? atoi(argv[3]) : 2880;   // ticks (24 h)
    long attempts     = argc > 4 ? atol(argv[4]) : 6000;
    if (argc > 5) DIAG_TICK = atoi(argv[5]);
    if (argc > 6) COMPACT = atoi(argv[6]);
    if (argc > 7) YIELD_AT = atol(argv[7]);
    rec_from_env();
    memset(flash, 0xff, sizeof flash);
    mount();
    { spiffs_file f = SPIFFS_open(&fs, "lotw_cert.b64", SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
      u8_t b[1596]; memset(b, 'c', sizeof b); SPIFFS_write(&fs, f, b, sizeof b); SPIFFS_close(&fs, f);
      f = SPIFFS_open(&fs, "lotw_key.b64", SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
      SPIFFS_write(&fs, f, b, 848); SPIFFS_close(&fs, f); }
    boot();
    long tick = 0, n = 0, last_report = 0;
    while (n < attempts) {
        diag_tick(); if (getenv("STEP")) step_check("diag_tick"); tick++;
        if (tick % ticks_per_qso == 0) {
            n_attempt = n + 1; qso(); if (getenv("STEP")) step_check("qso"); n++;
            if (getenv("CLEAR_AT_WALL") && q_fail_reported == 10 && !cleared) { cleared = 1; long ok0 = q_ok; printf("CLEAR at n=%ld after %ld ok, qso.adi %ld\n", n, ok0, fsize("qso.adi")); SPIFFS_remove(&fs, "qso.adi"); SPIFFS_clearerr(&fs); clear_ok0 = ok0; }
            if (getenv("TRACE") && n % 250 == 0) { printf("n=%5ld qso=%6ld diag=%6ld d0=%6ld ", n, fsize("qso.adi"), fsize("diag.log"), fsize("diag.0.log")); fsstat("t"); }
            long fails = q_fail_reported + q_silent_loss;
            if (fails && fails != last_report && (fails <= 3 || fails % 500 == 0)) {
                last_report = fails;
                printf("attempt %5ld: qso.adi %6ld B (%ld ok)  diag.log %6ld  diag.0 %6ld  used %u KB  %s\n",
                       n, fsize("qso.adi"), q_ok, fsize("diag.log"), fsize("diag.0.log"),
                       (unsigned)used_kb(), MODE ? "REPORTED FAIL" : (q_silent_loss ? "SILENT LOSS" : "reported"));
            }
        }
        if (tick % boot_every == 0) { boot(); if (getenv("STEP")) step_check("boot"); }
        if (integ_first_bad_n < 0 && (tick % 50) == 0 && !integrity_q(0)) { integ_first_bad_n = n; printf("FIRST INTEGRITY FAILURE at attempt %ld, qso.adi %ld, used %u KB, ok %ld fails %ld: ", n, fsize("qso.adi"), (unsigned)used_kb(), q_ok, q_fail_reported + q_silent_loss); integrity(); }
    }
    if (cleared) printf("after clear: %ld QSOs logged, then wall at qso.adi %ld\n", q_ok - clear_ok0, fsize("qso.adi"));
    printf("  partial records left: %ld, refused by reserve check: %ld, fragments injected %ld cut %ld", partial_left, pre_refused, m7_injected, m7_cut_ok); puts("");
    integrity(); printf("  fragments left by failed cut-back: %ld, appends blocked until cut: %ld", frag_events, frag_blocked); puts("");
    fsstat("end"); printf("  compact calls %ld, max erases in one call %ld, total erases %ld\n", compact_calls, compact_erases_max, n_erase);
    { s32_t r = SPIFFS_gc(&fs, 4096); printf("  gc(4K)=%d\n", r); SPIFFS_clearerr(&fs); fsstat("after gc4k"); }
    { s32_t r = SPIFFS_gc_quick(&fs, 0); printf("  gc_quick=%d\n", r); SPIFFS_clearerr(&fs); fsstat("after quick"); }
    printf("MODE %d rec=%d qso/%dticks boot/%d: attempts %ld ok %ld reported_fail %ld silent_loss %ld | valve %ld saved %ld | first fail at qso.adi=%ld B | final qso.adi %ld diag.log %ld diag.0 %ld used %u KB diag_dead=%d\n",
           MODE, RECORD, ticks_per_qso, boot_every, n, q_ok, q_fail_reported, q_silent_loss,
           q_valve, q_valve_saved, first_fail_size, fsize("qso.adi"), fsize("diag.log"),
           fsize("diag.0.log"), (unsigned)used_kb(), diag_dead);
    return 0;
}
