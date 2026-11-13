// BASELINE: a power-cut sweep over ordinary operation with NO rotation -
// 30 rounds of a diag tick and one QSO append. Any loss here is SPIFFS's own
// power-loss behaviour.
//
// Env: RECS (log size, default 400), DIAG (diag bytes per round, default
// 6000; 0 = diag log not on this partition), DUAL=1 (every append also goes
// to qso.bak; a cut "survives" if EITHER copy holds every record).
//
// Result per cut, for the log as a whole:
//   OK, LOST (a record committed before the sweep is gone), UNREADABLE,
//   MISSING (no file after boot).

// 0 ok, 1 lost, 2 unreadable, 3 missing. *prefix = readable bytes if 2.
static int log_state(const char *name, int recs, long *prefix)
{
    const char *keep = g_log; g_log = name;
    long dl; char *d = slurp_dev(&dl);
    int st = 0;
    if (dl == -1) st = 3;
    else if (dl == -2) { st = 2; if (prefix) { const char *k2 = g_log; (void)k2; *prefix = 0; } }
    else {
        const char *nl = memchr(d, '\n', (size_t)dl);
        long hl = nl ? nl - d + 1 : dl, end = dl;
        while (end > hl && d[end - 1] != '\n') end--;    // a torn last append is boot repair's job
        const char *err = ""; int n = parse_recs(d + hl, end - hl, seqbuf_d, 20000, &err);
        int ok = n >= recs;
        for (int i = 0; ok && i < recs; i++) if (seqbuf_d[i] != i) ok = 0;
        st = ok ? 0 : 1;
    }
    free(d);
    g_log = keep;
    return st;
}

static void copy_spiffs(const char *from, const char *to)
{
    long n = fsize(from);
    char *b = malloc((size_t)n);
    spiffs_file f = SPIFFS_open(&fs, from, SPIFFS_O_RDONLY, 0); SPIFFS_read(&fs, f, b, n); SPIFFS_close(&fs, f);
    f = SPIFFS_open(&fs, to, SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
    SPIFFS_write(&fs, f, b, n); SPIFFS_fflush(&fs, f); SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
    free(b);
}

static int  B_DUAL, B_DIAG, B_RECS;
static void b_round(int i)
{
    if (B_DIAG) diag_churn(B_DIAG);
    g_log = "qso.adi"; append_rec(B_RECS + i);
    if (B_DUAL) { g_log = "qso.bak"; append_rec(B_RECS + i); g_log = "qso.adi"; }
}

static void baseline(int stride)
{
    B_DUAL = getenv("DUAL") != NULL;
    B_DIAG = getenv("DIAG") ? atoi(getenv("DIAG")) : 6000;
    B_RECS = getenv("RECS") ? atoi(getenv("RECS")) : 400;
    build(B_RECS, 0, B_DIAG); boot();
    if (B_DUAL) { copy_spiffs("qso.adi", "qso.bak"); boot(); }
    snap_take();
    ops = 0; in_run = 1;
    for (int i = 0; i < 30; i++) b_round(i);
    in_run = 0;
    long total_ops = ops, cuts = 0, res[4] = {0};
    for (long k = 1; k <= total_ops; k += stride) {
        snap_restore(); ops = 0; cut_countdown = k;
        if (setjmp(cut_jmp) == 0) {
            in_run = 1;
            for (int i = 0; i < 30; i++) b_round(i);
            in_run = 0; cut_countdown = -1; continue;
        }
        in_run = 0; cut_countdown = -1; cuts++;
        g_log = "qso.adi";
        int pi = tally_phase(cut_phase);
        ph_cuts[pi]++;
        boot_cold();
        int st = log_state("qso.adi", B_RECS, NULL);
        if (B_DUAL && st) { int s2 = log_state("qso.bak", B_RECS, NULL); if (s2 < st) st = s2; }
        res[st]++;
        if (st == 3) ph_missing[pi]++;
        if (st == 2) ph_unread[pi]++;
    }
    printf("BASELINE %s%s, %d recs, diag %d B/round: %ld ops, %ld cuts: OK %ld, lost %ld, unreadable %ld, missing %ld\n",
           getenv("NOHALF") ? "clean cuts" : "torn page", B_DUAL ? ", DUAL copy" : "", B_RECS, B_DIAG,
           total_ops, cuts, res[0], res[1], res[2], res[3]);
    for (int i = 0; i < n_ph; i++)
        printf("  %-16s cuts %5ld  missing %4ld  unreadable %4ld\n", ph_name[i], ph_cuts[i], ph_missing[i], ph_unread[i]);
}
