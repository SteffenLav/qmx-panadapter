static int g_was_ok = 1; static const char *g_last = "start";
static void step_check(const char *what) {
    if (!g_was_ok) return;
    if (!integrity_q(0)) { g_was_ok = 0;
        printf("** log broke during: %s (prev op %s)  free_blocks %d alloc %d deleted %d qso.adi %ld diag %ld d0 %ld\n",
               what, g_last, (int)fs.free_blocks, (int)fs.stats_p_allocated, (int)fs.stats_p_deleted,
               fsize("qso.adi"), fsize("diag.log"), fsize("diag.0.log")); }
    g_last = what; }
