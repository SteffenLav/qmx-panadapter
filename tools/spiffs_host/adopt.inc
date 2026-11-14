// ---------------------------------------------------------------------------
// SEEDED ARCHIVE (Randy N4OPI, 2026-11-14): a log this device never made,
// placed on the card by hand as qso-archive.adi, taken over by the device.
//
// This is the only route to "all my contacts on the card AND marked worked
// before" - the records stay on the card, never enter SPIFFS, and the
// worked-before index is built from them. adopt_archive() in adif_log.c does
// one thing: it rewrites the device log's HEADER so the log owns that
// archive. No record moves. So the header write is all this scenario has to
// model, and everything afterwards is the real rotation code.
//
// Three things are proved, in the order a user meets them:
//   A  before adopting, rotation REFUSES and touches nothing. This matters:
//      the seeded archive sits on the card through a boot before anyone can
//      answer a dialog, and a rotation that "tidied" it would eat the file.
//   B  after adopting, rotation accepts the archive and still touches nothing
//      (the device log is empty and under the cap).
//   C  new contacts then rotate ONTO the seeded archive: they append after it,
//      the seeded bytes are unchanged byte for byte, and the header counts
//      seeded + moved.
// ---------------------------------------------------------------------------

// Write the device log's header with an explicit (archived, arc_bytes) and
// keep the records after it. This is exactly what adopt_archive() does.
static void seed_header(uint32_t archived, uint32_t arc_bytes)
{
    long dl; char *d = slurp_dev(&dl);
    const char *nl = memchr(d, '\n', (size_t)dl);
    long hl = nl ? nl - d + 1 : dl;
    rot_hdr_t h = { archived, arc_bytes };
    char hdr[256]; int hn = rot_hdr_format(hdr, sizeof hdr, &h);
    spiffs_file f = SPIFFS_open(&fs, g_log, SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
    SPIFFS_write(&fs, f, hdr, hn);
    if (dl > hl) SPIFFS_write(&fs, f, d + hl, (s32_t)(dl - hl));
    SPIFFS_fflush(&fs, f); SPIFFS_close(&fs, f); SPIFFS_clearerr(&fs);
    free(d);
}

// A hand-made archive of `n` records on the card. Returns its record count.
static int seed_archive(int n)
{
    const char *hh = ROT_ARC_HEADER;
    arc_len = (long)strlen(hh);
    memcpy(arc_buf, hh, (size_t)arc_len);
    for (int s = 0; s < n; s++) {
        char r[1024]; int k = make_rec(s, r);
        if (arc_len + k > (long)sizeof arc_buf) { printf("  seed_archive: arc_buf too small at %d\n", s); return s; }
        memcpy(arc_buf + arc_len, r, (size_t)k); arc_len += k;
    }
    return n;
}

static void adopt_scenarios(void)
{
    const int SEED = 2700;                 // Randy's log
    static char seed_copy[4 << 20];
    long seed_len;
    rot_result_t r;
    rot_hdr_t h;

    // ---- A: seeded archive, log has NOT adopted it -------------------------
    n_scen++;
    build(0, 0, 0);                        // empty log, plain header
    int made = seed_archive(SEED);
    seed_len = arc_len; memcpy(seed_copy, arc_buf, (size_t)arc_len);
    long dev_before = fsize("qso.adi");
    boot();
    quiet = 0;
    rot_status_t st = run(UINT32_MAX, &r);
    quiet = 1;
    if (st != ROT_MISMATCH)
        fail("seeded archive, not adopted", "expected ROT_MISMATCH");
    else if (arc_len != seed_len || memcmp(arc_buf, seed_copy, (size_t)seed_len) != 0)
        fail("seeded archive, not adopted", "the card's archive was changed");
    else if (fsize("qso.adi") != dev_before)
        fail("seeded archive, not adopted", "the device log was changed");
    else
        printf("seeded archive (%d recs, %ld B), not adopted: MISMATCH, nothing touched\n",
               made, seed_len);

    // ---- B: adopt, then rotate --------------------------------------------
    n_scen++;
    seed_header((uint32_t)made, (uint32_t)seed_len);
    boot();
    st = run(UINT32_MAX, &r);
    {
        long dl; char *d = slurp_dev(&dl);
        const char *nl = memchr(d, '\n', (size_t)dl); long hl = nl - d + 1;
        rot_hdr_parse(d, (size_t)hl, &h); free(d);
        int ok = (st == ROT_NOTHING) &&
                 h.archived == (uint32_t)made && h.arc_bytes == (uint32_t)seed_len &&
                 arc_len == seed_len && memcmp(arc_buf, seed_copy, (size_t)seed_len) == 0;
        if (!ok) fail("seeded archive, adopted", r.why);
        else printf("seeded archive, adopted: %s, header %lu recs / %lu B, archive byte-identical\n",
                    st == ROT_NOTHING ? "NOTHING to move" : "?",
                    (unsigned long)h.archived, (unsigned long)h.arc_bytes);
    }

    // ---- C: new contacts rotate onto the seeded archive ---------------------
    // Append past the cap, with sequence numbers that continue the seeded set
    // so the archive stays contiguous and the check is exact.
    n_scen++;
    int added = 0;
    for (int s = SEED; s < SEED + 900; s++) { if (!append_rec(s)) break; added++; }
    boot();
    st = run(UINT32_MAX, &r);
    {
        long dl; char *d = slurp_dev(&dl);
        const char *nl = memchr(d, '\n', (size_t)dl); long hl = nl - d + 1;
        rot_hdr_parse(d, (size_t)hl, &h);
        const char *e = NULL;
        int nd = parse_recs(d + hl, dl - hl, seqbuf_d, 20000, &e);
        free(d);
        long ahl = (long)strlen(ROT_ARC_HEADER);
        int na = parse_recs(arc_buf + ahl, arc_len - ahl, seqbuf_a, 20000, &e);
        int ok = st == ROT_DONE && !e && nd > 0 && na > made &&
                 (int)h.archived == na && (long)h.arc_bytes == arc_len &&
                 // the seeded bytes are still there, byte for byte, at the front
                 arc_len > seed_len && memcmp(arc_buf, seed_copy, (size_t)seed_len) == 0 &&
                 // archive then device log = 0..SEED+added-1, in order, once
                 seqbuf_a[0] == 0 && seqbuf_d[nd - 1] == SEED + added - 1 &&
                 seqbuf_a[na - 1] + 1 == seqbuf_d[0];
        for (int i = 1; ok && i < na; i++) if (seqbuf_a[i] != seqbuf_a[i - 1] + 1) ok = 0;
        for (int i = 1; ok && i < nd; i++) if (seqbuf_d[i] != seqbuf_d[i - 1] + 1) ok = 0;
        if (!ok) fail("seeded archive, new contacts rotate onto it", e ? e : r.why);
        else printf("seeded archive + %d new: moved %u, archive %d recs (%ld B, seeded %ld B intact), device %d - contiguous 0..%d\n",
                    added, r.rotated, na, arc_len, seed_len, nd, SEED + added - 1);
    }
}
