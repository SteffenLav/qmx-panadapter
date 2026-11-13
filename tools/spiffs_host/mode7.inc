// MODE 7 = the firmware rule exactly as built (adif_log.c after the fix):
//   reserve(ADIF_RECORD_MAX 1024 + margin 8192) under the guard; if refused,
//   report and stop. If admitted: cut a pending fragment (truncate), append,
//   checked commit. On a failed commit: remember s_frag, no truncate now.
// FRAG_AT=n injects an 89 B partial record (like N4OPI's v1.26.2 file) after
// attempt n, as the boot repair would find it, and sets s_frag to the good size.
static long m7_cut_ok, m7_injected;
static void inject_fragment(void) {
    long good = fsize("qso.adi");
    spiffs_file f = SPIFFS_open(&fs, "qso.adi", SPIFFS_O_APPEND | SPIFFS_O_WRONLY, 0);
    if (f < 0) { SPIFFS_clearerr(&fs); return; }
    u8_t junk[89]; memset(junk, 'z', sizeof junk);          // no newline: a cut record
    SPIFFS_write(&fs, f, junk, sizeof junk); SPIFFS_fflush(&fs, f); SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
    if (fsize("qso.adi") == good + 89) { s_frag = good; m7_injected++; }
}
static void qso7(long n) {
    static long frag_at = -2;
    if (frag_at == -2) frag_at = getenv("FRAG_AT") ? atol(getenv("FRAG_AT")) : -1;
    if (n == frag_at) inject_fragment();
    long before = (s_frag >= 0) ? s_frag : fsize("qso.adi"); if (before < 0) before = 0;
    s32_t r = SPIFFS_gc(&fs, 1024 + 8192); SPIFFS_clearerr(&fs);
    if (r != SPIFFS_OK) { pre_refused++; q_fail_reported++; if (first_fail_size < 0) first_fail_size = before; return; }
    if (s_frag >= 0) {
        if (fsize("qso.adi") > s_frag && copy_cut(s_frag)) { q_fail_reported++; return; }
        s_frag = -1; m7_cut_ok++;
    }
    int we, fe, ce;
    int ok = append_once(before, &we, &fe, &ce);
    long after = fsize("qso.adi");
    if (ok && after == before + RECORD) { q_ok++; return; }
    if (ok) { printf("!! claimed success %ld -> %ld\n", before, after); exit(2); }
    printf("  write failed AFTER the reserve check passed (qso.adi %ld)\n", before);
    if (after != before) { partial_left++; s_frag = before; }
    if (first_fail_size < 0) first_fail_size = before;
    q_fail_reported++; }
