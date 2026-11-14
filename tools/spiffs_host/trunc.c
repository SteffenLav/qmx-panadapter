// Minimal isolation of SPIFFS_ftruncate on IDF v5.4.4 SPIFFS, device config.
// Build a file by 190 B appends (open/write/fflush/close each, as the
// firmware does), append an 89 B fragment, then truncate back to the good
// size exactly as vfs_spiffs_truncate() does (open WRONLY, ftruncate, close),
// and read the whole file back. Repeated for many file sizes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spiffs.h"
#include "spiffs_nucleus.h"
#define PART_SZ 0x100000
#define BLK 4096
#define PG 256
static uint8_t flash[PART_SZ];
void spiffs_api_lock(struct spiffs_t *fs) { (void)fs; }
void spiffs_api_unlock(struct spiffs_t *fs) { (void)fs; }
static s32_t rd(struct spiffs_t *fs, u32_t a, u32_t n, u8_t *d) { (void)fs; memcpy(d, flash + a, n); return 0; }
static s32_t wr(struct spiffs_t *fs, u32_t a, u32_t n, u8_t *s) { (void)fs; for (u32_t i = 0; i < n; i++) flash[a + i] &= s[i]; return 0; }
static s32_t er(struct spiffs_t *fs, u32_t a, u32_t n) { (void)fs; memset(flash + a, 0xff, n); return 0; }
static spiffs fs; static u8_t work[PG * 2], fds[32 * 8];
static u8_t cache[sizeof(spiffs_cache) + 4 * (sizeof(spiffs_cache_page) + PG)];
static void mount(void) {
    spiffs_config c = {0}; c.hal_read_f = rd; c.hal_write_f = wr; c.hal_erase_f = er;
    c.phys_size = PART_SZ; c.phys_erase_block = BLK; c.log_block_size = BLK; c.log_page_size = PG;
    if (SPIFFS_mount(&fs, &c, work, fds, sizeof fds, cache, sizeof cache, NULL) == SPIFFS_ERR_NOT_A_FS) {
        SPIFFS_format(&fs); SPIFFS_mount(&fs, &c, work, fds, sizeof fds, cache, sizeof cache, NULL); } }
static long fsz(void) { spiffs_stat st; return SPIFFS_stat(&fs, "q", &st) == 0 ? (long)st.size : -1; }
static void app(const u8_t *b, int n) { spiffs_file f = SPIFFS_open(&fs, "q", SPIFFS_O_APPEND | SPIFFS_O_CREAT | SPIFFS_O_WRONLY, 0);
    SPIFFS_write(&fs, f, (void *)b, n); SPIFFS_fflush(&fs, f); SPIFFS_close(&fs, f); }
static int readall(long n, long *bad) { spiffs_file f = SPIFFS_open(&fs, "q", SPIFFS_O_RDONLY, 0); static u8_t b[1 << 20];
    long off = 0; int ok = 1; while (off < n) { long w = n - off > 4096 ? 4096 : n - off; s32_t g = SPIFFS_read(&fs, f, b + off, w);
        if (g != w) { *bad = off; ok = 0; SPIFFS_clearerr(&fs); break; } off += w; }
    SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs); return ok; }
int main(int argc, char **argv) {
    int remount = argc > 1 ? atoi(argv[1]) : 0;   // 1 = unmount/mount between fragment and truncate (= boot repair)
    int rec = getenv("REC") ? atoi(getenv("REC")) : 190, fails = 0, tried = 0, refused = 0;
    u8_t r[1024]; memset(r, 'q', sizeof r);
    for (int nrec = 1; nrec <= 2600; nrec += (nrec < 40 ? 1 : 37)) {
        memset(flash, 0xff, sizeof flash); mount();
        for (int i = 0; i < nrec; i++) app(r, rec);
        long good = fsz(); app(r, 89);
        if (remount) { SPIFFS_unmount(&fs); mount(); }
        s32_t t = 0;
        if (getenv("COPYCUT") && SPIFFS_gc(&fs, good + 8192) != SPIFFS_OK) {
            SPIFFS_clearerr(&fs); refused++; SPIFFS_unmount(&fs); continue;   // firmware: refuse, touch nothing
        }
        if (getenv("COPYCUT")) {
            static u8_t buf[1 << 20];
            spiffs_file in = SPIFFS_open(&fs, "q", SPIFFS_O_RDONLY, 0);
            s32_t g = SPIFFS_read(&fs, in, buf, good); SPIFFS_close(&fs, in);
            spiffs_file o = SPIFFS_open(&fs, "q.tmp", SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
            for (long off = 0; off < good; off += 2048) { long w = good - off > 2048 ? 2048 : good - off; if (SPIFFS_write(&fs, o, buf + off, w) != w) t = -1; }
            if (SPIFFS_fflush(&fs, o) < 0) t = -1; SPIFFS_close(&fs, o);
            if (g != good) t = -2;
            if (t == 0) { SPIFFS_remove(&fs, "q"); if (SPIFFS_rename(&fs, "q.tmp", "q") < 0) t = -3; }
            else { SPIFFS_remove(&fs, "q.tmp"); }
            SPIFFS_clearerr(&fs);
        } else {
            spiffs_file f = SPIFFS_open(&fs, "q", SPIFFS_WRONLY, 0);
            t = SPIFFS_ftruncate(&fs, f, good); SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
        }
        long after = fsz(), bad = -1; int ok = readall(after, &bad);
        // and can it still be appended to and read?
        app(r, rec); long after2 = fsz(); long bad2 = -1; int ok2 = readall(after2, &bad2);
        tried++;
        if (t || after != good || !ok || !ok2 || after2 != good + rec) {
            fails++;
            if (fails <= 12) printf("nrec %4d good %7ld: ftruncate=%d size %ld read %s(bad@%ld) | append size %ld read %s(bad@%ld)  [good%%251=%ld, good%%256=%ld]\n",
                nrec, good, (int)t, after, ok ? "ok" : "FAIL", bad, after2, ok2 ? "ok" : "FAIL", bad2, good % 251, good % 256);
        }
        SPIFFS_unmount(&fs);
    }
    printf("%s rec=%d remount=%d: %d of %d cuts damaged the file; %d refused by the reserve check (file left as it was)\n",
           getenv("COPYCUT") ? "COPY-CUT" : "TRUNCATE", rec, remount, fails, tried, refused);
    return 0;
}
