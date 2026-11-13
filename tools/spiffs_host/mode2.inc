static long s_frag = -1, frag_events, frag_blocked;
static int trunc_to(long n) { spiffs_file t = SPIFFS_open(&fs, "qso.adi", SPIFFS_O_RDWR, 0);
    if (t < 0) { SPIFFS_clearerr(&fs); return -1; }
    s32_t r = SPIFFS_ftruncate(&fs, t, n); SPIFFS_clearerr(&fs); SPIFFS_close(&fs, t); SPIFFS_clearerr(&fs); return r < 0 ? -1 : 0; }
static int cut_frag(void) { if (s_frag < 0) return 1; if (fsize("qso.adi") > s_frag && trunc_to(s_frag)) return 0; s_frag = -1; return 1; }
static int append2(long before) { int we, fe, ce;
    if (!cut_frag()) { frag_blocked++; return 0; }
    if (append_once(before, &we, &fe, &ce)) return 1;
    if (fsize("qso.adi") != before && trunc_to(before)) { s_frag = before; frag_events++; }
    return 0; }
static void qso2(void) {
    long before = fsize("qso.adi"); if (before < 0) before = 0;
    if (s_frag >= 0) before = s_frag;
    int ok = append2(before);
    if (!ok) { q_valve++; SPIFFS_remove(&fs, "diag.0.log"); SPIFFS_clearerr(&fs); SPIFFS_gc(&fs, 32*1024); SPIFFS_clearerr(&fs);
               ok = append2(before); if (ok) q_valve_saved++; }
    long after = fsize("qso.adi");
    if (ok && after == before + RECORD) { q_ok++; return; }
    if (ok) { printf("!! claimed success %ld -> %ld\n", before, after); exit(2); }
    if (first_fail_size < 0) first_fail_size = before;
    q_fail_reported++; }
static long integ_first_bad_n = -1;
static int integrity_q(int verbose) { long n = fsize("qso.adi"); if (n < 0) return 1;
    spiffs_file f = SPIFFS_open(&fs, "qso.adi", SPIFFS_O_RDONLY, 0);
    if (f < 0) { if (verbose) printf("integrity: open failed %d", (int)SPIFFS_errno(&fs)); SPIFFS_clearerr(&fs); return 0; }
    static u8_t b[4096]; long off = 0; int ok = 1;
    while (off < n) { long want = n - off > 4096 ? 4096 : n - off;
        s32_t g = SPIFFS_read(&fs, f, b, want);
        if (g != want) { if (verbose) printf("integrity: read error %d at offset %ld of %ld", (int)g, off, n); ok = 0; SPIFFS_clearerr(&fs); break; }
        for (long i = 0; i < want; i++) { long p = off + i; if (s_frag >= 0 && p >= s_frag) break; if ((b[i] == 10) != ((p % RECORD) == RECORD - 1)) { if (verbose) printf("integrity: bad byte at %ld", p); ok = 0; break; } }
        if (!ok) break; off += want; }
    SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
    if (ok && (s_frag >= 0 ? s_frag : n) % RECORD) { if (verbose) printf("integrity: %ld B not whole records", n); ok = 0; }
    if (ok && verbose) printf("integrity: %ld whole records, %ld B, OK", n / RECORD, n);
    return ok; }
static int integrity(void) { int r = integrity_q(1); puts(""); return r; }
// MODE 3: checked commit + valve + retry, NO truncate anywhere.
// MODE 4: checked commit only (no valve, no truncate).
static long partial_left;
static void qso3(int valve) {
    long before = fsize("qso.adi"); if (before < 0) before = 0;
    int we, fe, ce;
    int ok = append_once(before, &we, &fe, &ce);
    if (!ok && getenv("STEP")) step_check("failed append");
    if (!ok && valve) { q_valve++; SPIFFS_remove(&fs, "diag.0.log"); SPIFFS_clearerr(&fs); if (getenv("STEP")) step_check("valve remove"); gc_watch(32*1024, "valve"); if (getenv("STEP")) step_check("valve gc");
        long b2 = fsize("qso.adi"); if (b2 != before) partial_left++;
        ok = append_once(b2, &we, &fe, &ce); if (ok) q_valve_saved++; before = b2; }
    long after = fsize("qso.adi");
    if (ok && after == before + RECORD) { q_ok++; return; }
    if (ok) { printf("!! claimed success %ld -> %ld\n", before, after); exit(2); }
    if (after != before) partial_left++;
    if (first_fail_size < 0) first_fail_size = before;
    q_fail_reported++; }
// MODE 5: checked commit + cut-back with remembered good size, NO valve.
static void qso5(void) {
    long before = fsize("qso.adi"); if (before < 0) before = 0;
    if (s_frag >= 0) before = s_frag;
    int ok = append2(before);
    if (getenv("STEP") && !ok) step_check("failed append+cut");
    long after = fsize("qso.adi");
    if (ok && after == before + RECORD) { q_ok++; return; }
    if (ok) { printf("!! claimed success %ld -> %ld\n", before, after); exit(2); }
    if (after != before) partial_left++;
    if (first_fail_size < 0) first_fail_size = before;
    q_fail_reported++; }
// MODE 6: reserve-check BEFORE writing (SPIFFS_gc(len) must return OK),
// then checked commit. No valve, no truncate.
static long pre_refused;
static void qso6(void) {
    long before = fsize("qso.adi"); if (before < 0) before = 0;
    u32_t len = getenv("PRE") ? (u32_t)atoi(getenv("PRE")) : 8192;
    s32_t r = SPIFFS_gc(&fs, len); SPIFFS_clearerr(&fs);
    if (r != SPIFFS_OK) { pre_refused++; q_fail_reported++; if (first_fail_size < 0) first_fail_size = before; return; }
    int we, fe, ce;
    int ok = append_once(before, &we, &fe, &ce);
    long after = fsize("qso.adi");
    if (ok && after == before + RECORD) { q_ok++; return; }
    if (ok) { printf("!! claimed success %ld -> %ld\n", before, after); exit(2); }
    if (after != before) partial_left++;
    printf("  write failed AFTER the reserve check passed (qso.adi %ld)\n", before);
    if (first_fail_size < 0) first_fail_size = before;
    q_fail_reported++; }
// Firmware cut_pending_fragment(): admitted copy of the good bytes + rename.
static int copy_cut(long good) {
    if (SPIFFS_gc(&fs, good + 1 + 8192) != SPIFFS_OK) { SPIFFS_clearerr(&fs); return -1; }
    static u8_t buf[1 << 20];
    spiffs_file in = SPIFFS_open(&fs, "qso.adi", SPIFFS_O_RDONLY, 0);
    s32_t g = SPIFFS_read(&fs, in, buf, good); SPIFFS_close(&fs, in); SPIFFS_clearerr(&fs);
    int t = (g == good) ? 0 : -2;
    spiffs_file o = SPIFFS_open(&fs, "qso.tmp", SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
    for (long off = 0; t == 0 && off < good; off += 2048) { long w = good - off > 2048 ? 2048 : good - off; if (SPIFFS_write(&fs, o, buf + off, w) != w) t = -1; }
    if (SPIFFS_fflush(&fs, o) < 0) t = -1;
    if (SPIFFS_close(&fs, o) < 0) t = -1;
    SPIFFS_clearerr(&fs);
    if (t == 0) { SPIFFS_remove(&fs, "qso.adi"); if (SPIFFS_rename(&fs, "qso.tmp", "qso.adi") < 0) t = -3; }
    else SPIFFS_remove(&fs, "qso.tmp");
    SPIFFS_clearerr(&fs);
    return t; }
