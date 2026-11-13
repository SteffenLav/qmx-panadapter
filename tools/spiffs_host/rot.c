// Host replay of the QSO log rotation (main/adif/adif_rotate.c - the real
// file, compiled in) against the real IDF v5.4.4 SPIFFS code at the Tab5
// `storage` geometry. The SD archive is a RAM file with FAT semantics.
//
// What it proves, per scenario:
//   - a power cut at EVERY flash program/erase and every archive write of a
//     rotation (POWER CUT sweep): after the cut, every record is in the
//     archive or the device log; after boot recovery and one more run, the
//     archive + device log are exactly the original records, in order, once
//   - a full partition at the device-log rewrite: ROT_FULL, nothing lost,
//     and the move finishes once space returns
//   - an upload cursor below the cap: records it has not sent stay
//   - a foreign archive on the card: nothing is touched
//   - zero writes failing after a passed reserve check
//
// Build (from this directory):
//   S=/c/esp/v5.4.4/esp-idf/components/spiffs/spiffs/src
//   gcc -O2 -w -Iinc -I../../main/adif -I/c/esp/v5.4.4/esp-idf/components/spiffs/include -I$S \
//       -o rot.exe rot.c ../../main/adif/adif_rotate.c $S/*.c
// Run: rot.exe [stride]   (stride = cut every Nth op; 1 = every op)
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spiffs.h"
#include "spiffs_nucleus.h"
#include "adif_rotate.h"

#define PART_SZ 0x100000
#define BLK     4096
#define PG      256
static uint8_t flash[PART_SZ];

// ---- power cut -------------------------------------------------------------
static long cut_countdown = -1;      // -1 = off
static long ops;                     // flash programs + erases + archive writes
static jmp_buf cut_jmp;
static int  in_run;
static const char *phase = "-", *cut_phase = "-";
static void maybe_cut(void) { ops++; if (cut_countdown > 0 && --cut_countdown == 0) { cut_countdown = -1; cut_phase = phase; longjmp(cut_jmp, 1); } }

void spiffs_api_lock(struct spiffs_t *fs)   { (void)fs; }
void spiffs_api_unlock(struct spiffs_t *fs) { (void)fs; }
static s32_t hal_rd(struct spiffs_t *fs, u32_t a, u32_t n, u8_t *d) { (void)fs; memcpy(d, flash + a, n); return SPIFFS_OK; }
static s32_t hal_wr(struct spiffs_t *fs, u32_t a, u32_t n, u8_t *s)
{
    (void)fs;
    if (in_run && cut_countdown == 1 && !getenv("NOHALF")) {          // the cut lands mid-program: half the bytes
        for (u32_t i = 0; i < n / 2; i++) flash[a + i] &= s[i];
    }
    if (in_run) maybe_cut();
    for (u32_t i = 0; i < n; i++) flash[a + i] &= s[i];
    return SPIFFS_OK;
}
static s32_t hal_er(struct spiffs_t *fs, u32_t a, u32_t n) { (void)fs; if (in_run) maybe_cut(); memset(flash + a, 0xff, n); return SPIFFS_OK; }

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
    memset(&fs, 0, sizeof fs);
    s32_t r = SPIFFS_mount(&fs, &c, work, fds, sizeof(fds), cache, sizeof(cache), NULL);
    if (r == SPIFFS_ERR_NOT_A_FS) { SPIFFS_format(&fs); r = SPIFFS_mount(&fs, &c, work, fds, sizeof(fds), cache, sizeof(cache), NULL); }
    if (r) { printf("mount failed %d\n", r); exit(1); }
}
static const char *g_log = "qso.adi";
// esp_spiffs stores "/spiffs/x" as object "/x": a real image needs the slash.
static const char *g_pfx = "";
static const char *NM(const char *b) { static char r[4][40]; static int k; k = (k + 1) & 3; snprintf(r[k], sizeof r[k], "%s%s", g_pfx, b); return r[k]; }
static long fsize(const char *p) { spiffs_stat st; long r = SPIFFS_stat(&fs, p, &st) == 0 ? (long)st.size : -1; SPIFFS_clearerr(&fs); return r; }

// ---- the archive: a RAM file, FAT semantics --------------------------------
static char arc_buf[4 << 20];
static long arc_len = -1;            // -1 = no file

// ---- rot_io_t over SPIFFS + the RAM archive --------------------------------
typedef struct { int which; spiffs_file fd; long pos; int wr; } hnd_t;
static hnd_t hpool[8];
static const char *name_of(int w) { return NM(w == ROT_DEV ? "qso.adi" : "qso.tmp"); }
static long after_reserve_fail;      // writes that failed after a passed reserve
static int  reserved_ok;

static void *h_open(void *c, int which, const char *mode)
{
    (void)c;
    hnd_t *h = NULL;
    for (int i = 0; i < 8; i++) if (hpool[i].which == -1) { h = &hpool[i]; break; }
    if (!h) { printf("!! handle pool exhausted\n"); exit(3); }
    h->which = which; h->pos = 0; h->wr = (mode[0] != 'r');
    if (which == ROT_ARC) {
        if (mode[0] == 'r') { if (arc_len < 0) { h->which = -1; return NULL; } }
        else if (mode[0] == 'w') arc_len = 0;
        else { if (arc_len < 0) arc_len = 0; h->pos = arc_len; }
        return h;
    }
    spiffs_flags fl = mode[0] == 'r' ? SPIFFS_O_RDONLY
                    : mode[0] == 'w' ? (SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY)
                                     : (SPIFFS_O_APPEND | SPIFFS_O_CREAT | SPIFFS_O_WRONLY);
    h->fd = SPIFFS_open(&fs, name_of(which), fl, 0);
    if (h->fd < 0) { SPIFFS_clearerr(&fs); h->which = -1; return NULL; }
    return h;
}
static long h_read(void *c, void *hv, void *dst, long n)
{
    (void)c; hnd_t *h = hv;
    if (h->which == ROT_ARC) {
        long left = arc_len - h->pos; if (left < 0) left = 0;
        if (n > left) n = left;
        memcpy(dst, arc_buf + h->pos, (size_t)n); h->pos += n; return n;
    }
    s32_t g = SPIFFS_read(&fs, h->fd, dst, n);
    if (g < 0) { s32_t e = SPIFFS_errno(&fs); SPIFFS_clearerr(&fs); return e == SPIFFS_ERR_END_OF_OBJECT ? 0 : -1; }
    return g;
}
static long h_write(void *c, void *hv, const void *src, long n)
{
    (void)c; hnd_t *h = hv;
    if (h->which == ROT_ARC) {
        // A FAT append cut by a power loss leaves some prefix of the chunk.
        if (in_run && cut_countdown == 1) { long part = n / 3; memcpy(arc_buf + h->pos, src, (size_t)part); arc_len = h->pos + part; }
        if (in_run) maybe_cut();
        memcpy(arc_buf + h->pos, src, (size_t)n); h->pos += n;
        if (h->pos > arc_len) arc_len = h->pos;
        return n;
    }
    s32_t w = SPIFFS_write(&fs, h->fd, (void *)src, n);
    if (w < 0) { SPIFFS_clearerr(&fs); if (reserved_ok) after_reserve_fail++; return -1; }
    return w;
}
static bool h_seek(void *c, void *hv, long off)
{
    (void)c; hnd_t *h = hv;
    if (h->which == ROT_ARC) { h->pos = off; return off <= arc_len; }
    s32_t r = SPIFFS_lseek(&fs, h->fd, off, SPIFFS_SEEK_SET); SPIFFS_clearerr(&fs); return r >= 0;
}
static bool h_close(void *c, void *hv)
{
    (void)c; hnd_t *h = hv; bool ok = true;
    if (h->which != ROT_ARC) {
        if (h->wr && SPIFFS_fflush(&fs, h->fd) < 0) ok = false;
        if (SPIFFS_close(&fs, h->fd) < 0) ok = false;
        SPIFFS_clearerr(&fs);
        if (!ok && h->wr && reserved_ok) after_reserve_fail++;
    }
    h->which = -1;
    return ok;
}
static long h_size(void *c, int which) { (void)c; return which == ROT_ARC ? arc_len : fsize(name_of(which)); }
static bool h_remove(void *c, int which)
{
    (void)c;
    if (which == ROT_ARC) { arc_len = -1; return true; }
    s32_t r = SPIFFS_remove(&fs, name_of(which)); SPIFFS_clearerr(&fs); return r == 0;
}
static bool h_rename(void *c) { (void)c; s32_t r = SPIFFS_rename(&fs, NM("qso.tmp"), NM("qso.adi")); SPIFFS_clearerr(&fs); return r == 0; }
static bool h_trunc(void *c, int which, long len) { (void)c; if (which != ROT_ARC || len > arc_len) return false; arc_len = len; return true; }
static bool h_reserve(void *c, long need)
{
    (void)c;
    s32_t r = SPIFFS_gc(&fs, (u32_t)need + 8192); SPIFFS_clearerr(&fs);
    reserved_ok = (r == SPIFFS_OK);
    return reserved_ok;
}
static int quiet = 1;
static void h_log(void *c, bool warn, const char *m) { (void)c; if (!quiet) printf("    [%s] %s\n", warn ? "W" : "I", m); }

static char iobuf[4096];
static const rot_io_t IO = { NULL, iobuf, sizeof iobuf, h_open, h_read, h_write, h_seek, h_close,
                             h_size, h_remove, h_rename, h_trunc, h_reserve, h_log };

static void reset_handles(void) { for (int i = 0; i < 8; i++) hpool[i].which = -1; }

// What adif_log_init() does with qso.tmp at boot.
static void boot_recover(void)
{
    if (fsize(NM("qso.tmp")) >= 0) {
        if (fsize(NM("qso.adi")) < 0) SPIFFS_rename(&fs, NM("qso.tmp"), NM("qso.adi"));
        else SPIFFS_remove(&fs, NM("qso.tmp"));
        SPIFFS_clearerr(&fs);
    }
}

// After a power cut the RAM state is gone: mount fresh, NEVER unmount - an
// unmount flushes the write cache, i.e. writes flash after the power died.
// (The first version of this harness did that and reported losses that were
// its own writes.)
static void boot_cold(void)
{
    int d = getenv("DBGB") != NULL;
    mount();
    if (d) printf("    mount: adi %ld tmp %ld\n", fsize("qso.adi"), fsize("qso.tmp"));
    s32_t g = SPIFFS_gc(&fs, 65536); SPIFFS_clearerr(&fs);
    if (d) printf("    gc=%d: adi %ld tmp %ld\n", (int)g, fsize("qso.adi"), fsize("qso.tmp"));
    s32_t c = getenv("NOCHECK") ? 0 : SPIFFS_check(&fs); SPIFFS_clearerr(&fs);
    if (d) printf("    check=%d: adi %ld tmp %ld\n", (int)c, fsize("qso.adi"), fsize("qso.tmp"));
    boot_recover();
    if (d) printf("    recover: adi %ld tmp %ld\n", fsize("qso.adi"), fsize("qso.tmp"));
}
static void boot(void) { SPIFFS_unmount(&fs); mount(); SPIFFS_gc(&fs, 65536); SPIFFS_clearerr(&fs); SPIFFS_check(&fs); SPIFFS_clearerr(&fs); boot_recover(); }

// ---- records ---------------------------------------------------------------
static int rec_len(int seq) { return 150 + (int)((seq * 2654435761u) % 451); }   // 150..600 B
static int make_rec(int seq, char *out)
{
    int len = rec_len(seq);
    int n = sprintf(out, "<CALL:6>R%05d <COMMENT:%d>", seq, len - 34);
    while (n < len - 7) out[n++] = 'a' + (char)((seq + n) % 26);
    memcpy(out + n, " <EOR>\n", 7); n += 7;
    return n;
}

static void write_dev_header(void)
{
    spiffs_file f = SPIFFS_open(&fs, g_log, SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
    const char *h = "<ADIF_VER:5>3.1.4 <PROGRAMID:13>QMX-Panadapter <EOH>\n";
    SPIFFS_write(&fs, f, (void *)h, (s32_t)strlen(h)); SPIFFS_fflush(&fs, f); SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
}
// One QSO the way the firmware writes it: admitted, appended in 128 B chunks.
static int append_rec(int seq)
{
    phase = "qso-reserve-gc";
    if (SPIFFS_gc(&fs, 1024 + 8192) != SPIFFS_OK) { SPIFFS_clearerr(&fs); return 0; }
    phase = "qso-append";
    char r[1024]; int n = make_rec(seq, r);
    spiffs_file f = SPIFFS_open(&fs, g_log, SPIFFS_O_APPEND | SPIFFS_O_WRONLY, 0);
    if (f < 0) { SPIFFS_clearerr(&fs); return 0; }
    int ok = 1;
    for (int off = 0; off < n; off += 128) { int k = n - off > 128 ? 128 : n - off; if (SPIFFS_write(&fs, f, r + off, k) < 0) ok = 0; }
    if (SPIFFS_fflush(&fs, f) < 0) ok = 0;
    SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
    return ok;
}
// Diag log churn between QSOs (the persist task: ~6 KB per tick, rotate at 128 KB).
static void diag_churn(int bytes)
{
    static char b[2048]; memset(b, 'd', sizeof b);
    phase = "diag-write";
    spiffs_file f = SPIFFS_open(&fs, "diag.log", SPIFFS_O_APPEND | SPIFFS_O_CREAT | SPIFFS_O_WRONLY, 0);
    for (int left = bytes; left > 0; left -= 2048) SPIFFS_write(&fs, f, b, left > 2048 ? 2048 : left);
    SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
    phase = "diag-rotate";
    if (fsize("diag.log") > 128 * 1024) { SPIFFS_remove(&fs, "diag.0.log"); SPIFFS_rename(&fs, "diag.log", "diag.0.log"); SPIFFS_clearerr(&fs); }
}

// ---- checks ----------------------------------------------------------------
static char *slurp_dev(long *len)
{
    long n = fsize(g_log); if (n < 0) { *len = -1; return NULL; }
    char *b = malloc((size_t)n + 1);
    spiffs_file f = SPIFFS_open(&fs, g_log, SPIFFS_O_RDONLY, 0);
    s32_t g = f >= 0 ? SPIFFS_read(&fs, f, b, n) : -1;
    SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
    if (g != n) { free(b); *len = -2; return NULL; }       // unreadable: the T197 failure
    b[n] = 0; *len = n; return b;
}
// Parse records "<CALL:6>Rnnnnn" from text, checking each is byte-exact.
static int parse_recs(const char *t, long n, int *seqs, int max, const char **err)
{
    int k = 0; long p = 0; char want[1024];
    while (p < n) {
        const char *nl = memchr(t + p, '\n', (size_t)(n - p));
        if (!nl) { *err = "partial last line"; return -1; }
        long len = nl - (t + p) + 1;
        int seq;
        if (sscanf(t + p, "<CALL:6>R%05d", &seq) != 1) { *err = "unparsable record"; return -1; }
        int wl = make_rec(seq, want);
        if (wl != len || memcmp(want, t + p, (size_t)len)) { *err = "record bytes differ"; return -1; }
        if (k >= max) { *err = "too many"; return -1; }
        seqs[k++] = seq; p += len;
    }
    return k;
}
static int seqbuf_a[20000], seqbuf_d[20000];
// mode 0: no loss (every seq 0..total-1 present at least once, nothing else).
// mode 1: exact (archive records then device records == 0..total-1, once each,
//         and the header agrees with the archive).
static const char *check(int total, int mode, rot_hdr_t *hout)
{
    long dl; char *d = slurp_dev(&dl);
    if (dl == -1) return "device log missing";
    if (dl == -2) return "device log UNREADABLE";
    const char *nl = memchr(d, '\n', (size_t)dl);
    if (!nl) { free(d); return "no header"; }
    long hl = nl - d + 1;
    rot_hdr_t h; rot_hdr_parse(d, (size_t)hl, &h); if (hout) *hout = h;
    const char *err = "";
    int nd = parse_recs(d + hl, dl - hl, seqbuf_d, 20000, &err);
    free(d);
    if (nd < 0) return err;
    int na = 0;
    if (arc_len >= 0) {
        long ahl = (long)strlen(ROT_ARC_HEADER);
        if (arc_len < ahl || memcmp(arc_buf, ROT_ARC_HEADER, (size_t)ahl)) { if (mode == 1 || arc_len > ahl) return "bad archive header"; }
        else {
            // A torn tail is allowed after a cut (mode 0): parse whole lines only.
            long end = arc_len;
            if (mode == 0) { while (end > ahl && arc_buf[end - 1] != '\n') end--; }
            na = parse_recs(arc_buf + ahl, end - ahl, seqbuf_a, 20000, &err);
            if (na < 0) return err;
        }
    }
    static unsigned char seen[20000]; memset(seen, 0, sizeof seen);
    for (int i = 0; i < na; i++) seen[seqbuf_a[i]]++;
    for (int i = 0; i < nd; i++) seen[seqbuf_d[i]]++;
    for (int s = 0; s < total; s++) if (!seen[s]) return "A RECORD WAS LOST";
    if (mode == 0) return NULL;
    if (na + nd != total) return "duplicates after the move finished";
    for (int i = 0; i < na; i++) if (seqbuf_a[i] != i) return "archive out of order";
    for (int i = 0; i < nd; i++) if (seqbuf_d[i] != na + i) return "device log out of order";
    if (!(h.arc_bytes == 0 && na == 0) && (long)h.arc_bytes != arc_len) return "header arc_bytes != archive size";
    if ((int)h.archived != na) return "header archived != archive records";
    return NULL;
}

// ---- snapshot --------------------------------------------------------------
static uint8_t snap_flash[PART_SZ];
static char   *snap_arc; static long snap_arc_len;
static void snap_take(void) { memcpy(snap_flash, flash, PART_SZ); if (!snap_arc) snap_arc = malloc(sizeof arc_buf); memcpy(snap_arc, arc_buf, sizeof arc_buf); snap_arc_len = arc_len; }
static void snap_restore(void) { memcpy(flash, snap_flash, PART_SZ); memcpy(arc_buf, snap_arc, sizeof arc_buf); arc_len = snap_arc_len; mount(); }

static long n_scen, n_fail;
static void fail(const char *what, const char *why) { n_fail++; printf("  FAIL %s: %s\n", what, why); }

static rot_status_t run(uint32_t max_rot, rot_result_t *r)
{
    reset_handles(); reserved_ok = 0; in_run = 1;
    rot_run(&IO, max_rot, r);
    in_run = 0; reserved_ok = 0;
    return r->status;
}

// Build a log of `total` records (with diag churn) and optionally a first
// rotation part way, so the swept rotation starts from a card that already
// holds an archive.
static int build(int total, int rotate_at, int diag)
{
    memset(flash, 0xff, sizeof flash); arc_len = -1; mount();
    spiffs_file f = SPIFFS_open(&fs, "lotw_cert.b64", SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
    static u8_t b[2048]; memset(b, 'c', sizeof b); SPIFFS_write(&fs, f, b, 1596); SPIFFS_close(&fs, f);
    write_dev_header();
    for (int s = 0; s < total; s++) {
        if (!append_rec(s)) return s;
        if (diag) diag_churn(diag);
        if (s + 1 == rotate_at) { rot_result_t r; boot(); if (run(UINT32_MAX, &r) != ROT_DONE) { printf("  pre-rotation: %s\n", r.why); } }
    }
    return total;
}

static void sweep(const char *name, int total, int rotate_at, int diag, int stride, uint32_t max_rot)
{
    int made = build(total, rotate_at, diag);
    if (made != total) { printf("%s: partition full while building at %d records\n", name, made); total = made; }
    boot();
    snap_take();
    // Reference run: how many ops, and the outcome with no cut.
    rot_result_t r; ops = 0;
    rot_status_t st = run(max_rot, &r);
    long total_ops = ops;
    rot_hdr_t h; const char *e = check(total, st == ROT_DONE || st == ROT_NOTHING ? 1 : 0, &h);
    n_scen++;
    printf("%-30s %5d recs -> %-8s rot %4u held %4u dev %4u arc %4u, %ld ops\n", name, total,
           st == ROT_DONE ? "DONE" : st == ROT_NOTHING ? "NOTHING" : st == ROT_FULL ? "FULL" : st == ROT_MISMATCH ? "MISMATCH" : "IO",
           r.rotated, r.held, r.dev_records, h.archived, total_ops);
    if (e) fail(name, e);
    if (max_rot == UINT32_MAX && st == ROT_DONE) {
        long dl; char *d = slurp_dev(&dl); free(d);
        if (dl - 120 > ROT_LOW_BYTES) fail(name, "device log still over the low mark");
    }
    if (st != ROT_DONE) return;
    // The sweep: cut power at op k, recover, check no loss; run again, check exact.
    long cuts = 0, finished_by_second = 0;
    for (long k = getenv("START") ? atol(getenv("START")) : 1; k <= total_ops; k += stride) {
        snap_restore(); ops = 0; cut_countdown = k;
        rot_result_t r1;
        if (setjmp(cut_jmp) == 0) { run(max_rot, &r1); cut_countdown = -1; continue; }
        in_run = 0; cut_countdown = -1; cuts++;
        if (getenv("DBG") && atol(getenv("DBG")) == k) {
            mount();
            printf("    DBG cut@%ld at remount: qso.adi %ld qso.tmp %ld arc %ld\n", k, fsize("qso.adi"), fsize("qso.tmp"), arc_len);
            SPIFFS_gc(&fs, 65536); SPIFFS_clearerr(&fs);
            printf("    DBG after gc:    qso.adi %ld qso.tmp %ld\n", fsize("qso.adi"), fsize("qso.tmp"));
            SPIFFS_check(&fs); SPIFFS_clearerr(&fs);
            printf("    DBG after check: qso.adi %ld qso.tmp %ld\n", fsize("qso.adi"), fsize("qso.tmp"));
            mount();
        }
        boot_cold();
        if (getenv("DBG") && atol(getenv("DBG")) == k)
            printf("    DBG after boot recovery: qso.adi %ld qso.tmp %ld\n", fsize("qso.adi"), fsize("qso.tmp"));
        e = check(total, 0, NULL);
        if (e) { char w[96]; snprintf(w, sizeof w, "%s cut@%ld (after cut)", name, k); fail(w, e); continue; }
        rot_result_t r2; run(max_rot, &r2);
        if (r2.status == ROT_DONE && r2.finished) finished_by_second++;
        e = check(total, 1, NULL);
        if (e) { char w[96]; snprintf(w, sizeof w, "%s cut@%ld (second run %d: %s)", name, k, r2.status, r2.why); fail(w, e); }
    }
    printf("    %ld power cuts, %ld finished an interrupted move, writes failing after reserve: %ld\n", cuts, finished_by_second, after_reserve_fail);
}

// BASELINE: the same power-cut sweep over ordinary operation with NO rotation
// - 30 rounds of a diag tick and one QSO append, on a 400-record log. Any loss
// here is SPIFFS's own power-loss behaviour, which the rotation inherits but
// does not cause. Result per cut: OK, LOST (a record committed before the
// sweep started is gone), UNREADABLE, or MISSING (no qso.adi after boot).
static const char *ph_name[8]; static int n_ph;
static long ph_cuts[8], ph_missing[8], ph_unread[8], ph_prefix[8];
static int tally_phase(const char *p)
{
    for (int i = 0; i < n_ph; i++) if (strcmp(ph_name[i], p) == 0) return i;
    ph_name[n_ph] = p; return n_ph++;
}
// Bytes of qso.adi that read back before the first read error.
static long readable_prefix(void)
{
    spiffs_file f = SPIFFS_open(&fs, "qso.adi", SPIFFS_O_RDONLY, 0);
    if (f < 0) { SPIFFS_clearerr(&fs); return 0; }
    static char b[256]; long got = 0;
    for (;;) { s32_t g = SPIFFS_read(&fs, f, b, sizeof b); if (g <= 0) break; got += g; if (g < (s32_t)sizeof b) break; }
    SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
    return got;
}

#include "baseline.inc"

int main(int argc, char **argv)
{
    int stride = argc > 1 ? atoi(argv[1]) : 1;
    reset_handles();
    if (getenv("BASE")) { baseline(stride); return 0; }

    // WALL=1: build the log with diag churn until a QSO append is REFUSED
    // (the N4OPI state), then boot and rotate, 3 boots max. DROPDIAG0=1 as
    // below. REC-size records vary 150-600 B as everywhere here.
    if (getenv("WALL")) {
        int diag = getenv("DIAG") ? atoi(getenv("DIAG")) : 6000;
        int made = build(6000, 0, diag);
        u32_t t, u; SPIFFS_info(&fs, &t, &u);
        printf("wall: append refused after %d records, qso.adi %ld B, used %u KB, diag %ld + %ld\n",
               made, fsize("qso.adi"), (unsigned)(u / 1024), fsize("diag.log"), fsize("diag.0.log"));
        for (int b = 1; b <= 3; b++) {
            boot();
            if (getenv("DROPDIAG0")) { SPIFFS_remove(&fs, "diag.0.log"); SPIFFS_clearerr(&fs); }
            if (getenv("DROPDIAGALL")) { SPIFFS_remove(&fs, "diag.0.log"); SPIFFS_remove(&fs, "diag.log"); SPIFFS_clearerr(&fs); }
            if (getenv("GCLOOP")) {
                int ok = 0;
                for (int i = 0; i < 64; i++) { if (SPIFFS_gc(&fs, 32 * 1024) == SPIFFS_OK) ok++; SPIFFS_clearerr(&fs); }
                s32_t big = SPIFFS_gc(&fs, 110 * 1024); SPIFFS_clearerr(&fs);
                SPIFFS_info(&fs, &t, &u);
                printf("  gc loop: %d/64 small passes ok, then gc(110K)=%d, used %u KB, free_blocks %d deleted_pg %d\n",
                       ok, (int)big, (unsigned)(u / 1024), (int)fs.free_blocks, (int)fs.stats_p_deleted);
            }
            { SPIFFS_info(&fs, &t, &u);
              printf("  before rotation: used %u KB, free_blocks %d, deleted_pg %d, alloc_pg %d\n",
                     (unsigned)(u / 1024), (int)fs.free_blocks, (int)fs.stats_p_deleted, (int)fs.stats_p_allocated); }
            rot_result_t r; quiet = 0;
            rot_status_t st = run(UINT32_MAX, &r);
            printf("  boot %d: rotation %d (%s)\n", b, st, r.why);
            if (st == ROT_DONE) {
                const char *e = check(made, 1, NULL);
                printf("  exact check: %s; writes failing after reserve %ld\n", e ? e : "OK", after_reserve_fail);
                // and can it log again?
                int ok = 0; for (int i = 0; i < 50; i++) ok += append_rec(made + i);
                printf("  after rotation: %d of 50 new QSOs logged\n", ok);
                break;
            }
        }
        return 0;
    }

    // IMG=path: a real `storage` partition read off a Tab5 (esptool
    // read_flash 0x820000 0x100000). Boot it the way the firmware does, then
    // rotate - exactly what that unit did, on the PC.
    // DROPDIAG0=1 removes diag.0.log first (already mirrored to the card).
    if (getenv("IMG")) {
        g_pfx = "/";
        FILE *fi = fopen(getenv("IMG"), "rb");
        if (!fi || fread(flash, 1, PART_SZ, fi) != PART_SZ) { printf("cannot read %s\n", getenv("IMG")); return 2; }
        fclose(fi);
        mount();
        { spiffs_DIR dd; struct spiffs_dirent de, *pe; SPIFFS_opendir(&fs, "/", &dd);
          while ((pe = SPIFFS_readdir(&dd, &de))) printf("  object '%s' %u B\n", (char *)pe->name, (unsigned)pe->size);
          SPIFFS_closedir(&dd); }
        u32_t t, u; SPIFFS_info(&fs, &t, &u);
        printf("image mounted: used %u KB of %u KB, qso.adi %ld, diag.log %ld, diag.0.log %ld, free_blocks %d deleted_pg %d\n",
               (unsigned)(u / 1024), (unsigned)(t / 1024), fsize(NM("qso.adi")), fsize(NM("diag.log")), fsize(NM("diag.0.log")),
               (int)fs.free_blocks, (int)fs.stats_p_deleted);
        s32_t g = SPIFFS_gc(&fs, 65536); SPIFFS_clearerr(&fs);
        SPIFFS_check(&fs); SPIFFS_clearerr(&fs); boot_recover();
        SPIFFS_info(&fs, &t, &u);
        printf("after boot gc(64K)=%d + check: used %u KB, free_blocks %d deleted_pg %d\n", (int)g, (unsigned)(u / 1024),
               (int)fs.free_blocks, (int)fs.stats_p_deleted);
        if (getenv("DROPDIAG0")) {
            SPIFFS_remove(&fs, NM("diag.0.log")); SPIFFS_clearerr(&fs);
            SPIFFS_info(&fs, &t, &u);
            printf("diag.0.log removed: used %u KB, free_blocks %d deleted_pg %d\n", (unsigned)(u / 1024),
                   (int)fs.free_blocks, (int)fs.stats_p_deleted);
        }
        long before = fsize(NM("qso.adi"));
        rot_result_t r; quiet = 0;
        rot_status_t st = run(UINT32_MAX, &r);
        printf("rotation: status %d (%s); qso.adi %ld -> %ld, archive %ld B, writes failing after reserve %ld\n",
               st, r.why, before, fsize(NM("qso.adi")), arc_len, after_reserve_fail);
        return 0;
    }

    // Under the cap: nothing happens, no archive is created.
    sweep("under cap (100 recs)", 100, 0, 0, stride, UINT32_MAX);
    // First rotation at the cap, quiet partition and with diag churn.
    sweep("first rotation (400 recs)", 400, 0, 0, stride, UINT32_MAX);
    sweep("first rotation + diag", 400, 0, 6000, stride, UINT32_MAX);
    // A card that already has an archive.
    sweep("second rotation", 700, 400, 6000, stride, UINT32_MAX);
    // Randy's case: a ~500 KB log rotated the first time.
    sweep("N4OPI-size first rotation", 1300, 0, 0, stride * 4, UINT32_MAX);
    // An upload that has sent only 50 records.
    sweep("upload cursor at 50", 400, 0, 0, stride, 50);
    sweep("upload cursor at 0", 400, 0, 0, stride, 0);

    // Full partition at the rewrite: fill until the reserve for the rewrite
    // fails, rotate, free space, rotate again.
    {
        n_scen++;
        build(500, 0, 0); boot();
        spiffs_file f = SPIFFS_open(&fs, "filler", SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
        static u8_t b[4096]; memset(b, 'f', sizeof b);
        long dev = fsize("qso.adi");
        for (;;) { u32_t t, u; SPIFFS_info(&fs, &t, &u); if ((long)(t - u) < dev / 2 + 8192) break; if (SPIFFS_write(&fs, f, b, sizeof b) < 0) break; }
        SPIFFS_fflush(&fs, f); SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
        rot_result_t r; quiet = 0;
        rot_status_t st = run(UINT32_MAX, &r);
        printf("full partition: first run -> %d (%s)\n", st, r.why);
        if (st != ROT_FULL) fail("full partition", "expected ROT_FULL");
        const char *e = check(500, 0, NULL); if (e) fail("full partition (after FULL)", e);
        SPIFFS_remove(&fs, "filler"); SPIFFS_clearerr(&fs); boot();
        st = run(UINT32_MAX, &r);
        printf("full partition: after space returns -> %d (%s)\n", st, r.why);
        if (st != ROT_DONE || !r.finished) fail("full partition", "move did not finish");
        e = check(500, 1, NULL); if (e) fail("full partition (finished)", e);
        quiet = 1;
    }

    // A foreign archive: another station's card. Nothing may be touched.
    {
        n_scen++;
        build(400, 0, 0); boot();
        const char *hh = ROT_ARC_HEADER; arc_len = (long)strlen(hh); memcpy(arc_buf, hh, (size_t)arc_len);
        char rr[1024]; int n = make_rec(9999, rr); memcpy(arc_buf + arc_len, rr, (size_t)n); arc_len += n;
        long before = fsize("qso.adi"), abefore = arc_len;
        rot_result_t r; quiet = 0;
        rot_status_t st = run(UINT32_MAX, &r); quiet = 1;
        if (st != ROT_MISMATCH || fsize("qso.adi") != before || arc_len != abefore) fail("foreign archive", "was touched");
        else printf("foreign archive: MISMATCH, nothing touched\n");
        // A card with NO archive for a log that says it moved records to one
        // (new card, or archive deleted on a PC): a new archive is started and
        // the link reset. Over the cap: the new archive holds the moved
        // records and the header counts only those.
        n_scen++;
        build(700, 400, 0); boot(); arc_len = -1;
        st = run(UINT32_MAX, &r);
        {
            rot_hdr_t h; const char *e = NULL;
            long dl; char *d = slurp_dev(&dl);
            const char *nl = memchr(d, '\n', (size_t)dl); long hl2 = nl - d + 1;
            rot_hdr_parse(d, (size_t)hl2, &h);
            int nd = parse_recs(d + hl2, dl - hl2, seqbuf_d, 20000, &e);
            free(d);
            long ahl = (long)strlen(ROT_ARC_HEADER);
            int na = parse_recs(arc_buf + ahl, arc_len - ahl, seqbuf_a, 20000, &e);
            int ok = st == ROT_DONE && r.relinked && nd > 0 && na > 0 && (int)h.archived == na && (long)h.arc_bytes == arc_len &&
                     seqbuf_d[nd - 1] == 699 && seqbuf_a[na - 1] + 1 == seqbuf_d[0];
            for (int i = 1; ok && i < na; i++) if (seqbuf_a[i] != seqbuf_a[i - 1] + 1) ok = 0;
            for (int i = 1; ok && i < nd; i++) if (seqbuf_d[i] != seqbuf_d[i - 1] + 1) ok = 0;
            if (!ok) fail("missing archive, over cap", r.why);
            else printf("missing archive, over cap: new archive with %d records, header %lu/%lu, device %d - contiguous\n",
                        na, (unsigned long)h.archived, (unsigned long)h.arc_bytes, nd);
        }
        // The relink under power cuts: build the same state, cut at every
        // stride-th op, recover, check nothing was lost, run again, check the
        // archive + device log are contiguous and end at 699 and the header
        // agrees with the new archive.
        {
            n_scen++;
            build(700, 400, 0); boot(); arc_len = -1;
            long dl0; char *d0x = slurp_dev(&dl0);
            const char *nlx = memchr(d0x, '\n', (size_t)dl0); const char *e0 = "";
            int nd0 = parse_recs(nlx + 1, dl0 - (nlx - d0x + 1), seqbuf_d, 20000, &e0);
            int first = seqbuf_d[0]; free(d0x); (void)nd0;
            snap_take();
            rot_result_t rr; ops = 0; run(UINT32_MAX, &rr); long tot = ops;
            long cuts = 0, bad = 0, spiffs_lost = 0, full_retry = 0, full_stuck = 0;
            for (long k = 1; k <= tot; k += stride) {
                snap_restore(); ops = 0; cut_countdown = k;
                rot_result_t r1;
                if (setjmp(cut_jmp) == 0) { run(UINT32_MAX, &r1); cut_countdown = -1; continue; }
                in_run = 0; cut_countdown = -1; cuts++;
                boot_cold();
                rot_result_t r2; run(UINT32_MAX, &r2);
                int extra = 0;
                while (r2.status == ROT_FULL && extra < 3) { extra++; boot(); run(UINT32_MAX, &r2); }
                if (extra) { full_retry++; if (r2.status == ROT_FULL) full_stuck++; }
                long dl; char *d = slurp_dev(&dl);
                if (dl < 0) { spiffs_lost++; continue; }        // the SPIFFS class, counted apart
                const char *nl = memchr(d, '\n', (size_t)dl); long hl2 = nl - d + 1;
                rot_hdr_t h; rot_hdr_parse(d, (size_t)hl2, &h);
                const char *e = ""; int nd = parse_recs(d + hl2, dl - hl2, seqbuf_d, 20000, &e);
                free(d);
                int na = 0; long ahl = (long)strlen(ROT_ARC_HEADER);
                if (arc_len > 0) na = parse_recs(arc_buf + ahl, arc_len - ahl, seqbuf_a, 20000, &e);
                int ok = nd > 0 && na >= 0 && (int)h.archived == na && (na == 0 ? h.arc_bytes == 0 : (long)h.arc_bytes == arc_len);
                int expect = first;
                for (int i = 0; ok && i < na; i++) if (seqbuf_a[i] != expect++) ok = 0;
                for (int i = 0; ok && i < nd; i++) if (seqbuf_d[i] != expect++) ok = 0;
                if (ok && expect != 700) ok = 0;
                if (!ok) { bad++; if (bad <= 3) printf("  relink cut@%ld: second run %d (%s), dev %d arc %d hdr %lu/%lu\n", k, r2.status, r2.why, nd, na, (unsigned long)h.archived, (unsigned long)h.arc_bytes); }
            }
            if (bad) fail("relink under power cuts", "see above");
            printf("relink under power cuts: %ld cuts, %ld wrong, %ld lost to SPIFFS itself; %ld refused (FULL) at first, %ld still refused after 3 more boots\n",
                   cuts, bad, spiffs_lost, full_retry, full_stuck);
        }
        // Under the cap: only the header is reset; the records are untouched.
        n_scen++;
        build(700, 400, 0); boot();
        { rot_result_t r0; for (int s = 0; s < 1; s++) run(UINT32_MAX, &r0); }
        arc_len = -1;
        long recs_before; char *d0 = slurp_dev(&recs_before);
        const char *nl0 = memchr(d0, '\n', (size_t)recs_before); long body0 = recs_before - (nl0 - d0 + 1);
        char *b0 = malloc((size_t)body0); memcpy(b0, nl0 + 1, (size_t)body0); free(d0);
        st = run(UINT32_MAX, &r);
        {
            long dl; char *d = slurp_dev(&dl);
            const char *nl = memchr(d, '\n', (size_t)dl); long hl2 = nl - d + 1;
            rot_hdr_t h; rot_hdr_parse(d, (size_t)hl2, &h);
            int ok = st == ROT_NOTHING && r.relinked && h.archived == 0 && h.arc_bytes == 0 && arc_len == -1 &&
                     hl2 == (long)strlen(ROT_ARC_HEADER) && memcmp(d, ROT_ARC_HEADER, (size_t)hl2) == 0 &&
                     dl - hl2 == body0 && memcmp(d + hl2, b0, (size_t)body0) == 0;
            free(d);
            if (!ok) fail("missing archive, under cap", r.why);
            else printf("missing archive, under cap: header reset to plain, %ld B of records unchanged, no archive created\n", body0);
        }
        free(b0);
    }

    printf("\n%ld scenarios, %ld failures, writes failing after a passed reserve: %ld\n", n_scen, n_fail, after_reserve_fail);
    return n_fail ? 1 : 0;
}
