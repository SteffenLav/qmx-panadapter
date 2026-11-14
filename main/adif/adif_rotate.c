// QSO log rotation core. See adif_rotate.h and docs/qso-log-rotation-design.md.
//
// ⛔ Portable on purpose: tools/spiffs_host/rot.c compiles this file against
// the real SPIFFS code and cuts power at every flash write. No ESP-IDF calls
// here - all I/O goes through rot_io_t.
//
// Order of one run, and what a power cut at each point leaves:
//   1. tidy the archive's tail (FAT truncate)  - cut: next run tidies again
//   2. append the records to the archive       - cut: torn tail; step 1 cuts it
//   3. rewrite the device log to qso.tmp       - cut: qso.tmp is deleted at boot
//   4. remove qso.adi, rename qso.tmp          - cut: boot renames qso.tmp
// Between 2 and 4 the records are in BOTH files. The next run sees them past
// the header's arc_bytes, proves they are the device log's head byte for byte,
// and finishes the move. They are never in neither file.

#include "adif_rotate.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

const char ROT_ARC_HEADER[] =
    "<ADIF_VER:5>3.1.4 <PROGRAMID:13>QMX-Panadapter <EOH>\n";

#define TAG_ARCHIVED  "<APP_QMXPAN_ARCHIVED:"
#define TAG_ARC_BYTES "<APP_QMXPAN_ARCHIVE_BYTES:"
#define HDR_MAX 512

static uint32_t field_u32(const char *line, size_t len, const char *tag)
{
    size_t tl = strlen(tag);
    for (size_t i = 0; i + tl <= len; i++) {
        if (memcmp(line + i, tag, tl) != 0) continue;
        size_t j = i + tl;
        unsigned flen = 0;
        while (j < len && line[j] >= '0' && line[j] <= '9') flen = flen * 10 + (unsigned)(line[j++] - '0');
        if (j >= len || line[j] != '>' || flen == 0 || flen > 10) return 0;
        j++;
        uint32_t v = 0;
        for (unsigned k = 0; k < flen; k++, j++) {
            if (j >= len || line[j] < '0' || line[j] > '9') return 0;
            v = v * 10 + (uint32_t)(line[j] - '0');
        }
        return v;
    }
    return 0;
}

void rot_hdr_parse(const char *line, size_t len, rot_hdr_t *h)
{
    h->archived  = field_u32(line, len, TAG_ARCHIVED);
    h->arc_bytes = field_u32(line, len, TAG_ARC_BYTES);
}

int rot_hdr_format(char *out, size_t sz, const rot_hdr_t *h)
{
    // A log that never rotated keeps the header every earlier release wrote.
    if (h->archived == 0 && h->arc_bytes == 0)
        return snprintf(out, sz, "%s", ROT_ARC_HEADER);
    char a[12], b[12];
    int al = snprintf(a, sizeof a, "%lu", (unsigned long)h->archived);
    int bl = snprintf(b, sizeof b, "%lu", (unsigned long)h->arc_bytes);
    return snprintf(out, sz,
        "<ADIF_VER:5>3.1.4 <PROGRAMID:13>QMX-Panadapter "
        TAG_ARCHIVED "%d>%s " TAG_ARC_BYTES "%d>%s <EOH>\n", al, a, bl, b);
}

// ---- helpers ---------------------------------------------------------------

static void say(const rot_io_t *io, rot_result_t *r, rot_status_t st, bool warn, const char *msg)
{
    r->status = st;
    snprintf(r->why, sizeof r->why, "%s", msg);
    if (io->log) io->log(io->ctx, warn, msg);
}

// Walk the device log's records from the start of record 0 (file offset
// `start`). Stops at the first of: `max_recs` records, or the next record
// would end past `max_bytes`, or (when at_least >= 0) the bytes walked reach
// at_least. Counts whole records only. False on a read error.
static bool walk(const rot_io_t *io, long start, uint32_t max_recs, long max_bytes,
                 long at_least, uint32_t *recs, long *bytes)
{
    *recs = 0; *bytes = 0;
    if (max_recs == 0 || at_least == 0) return true;
    void *f = io->open(io->ctx, ROT_DEV, "rb");
    if (!f) return false;
    bool ok = io->seek(io->ctx, f, start);
    long pos = 0;              // bytes read past start
    while (ok) {
        long n = io->read(io->ctx, f, io->buf, (long)io->buf_sz);
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        for (long i = 0; i < n; i++) {
            if (io->buf[i] != '\n') continue;
            long end = pos + i + 1;
            if (end > max_bytes) goto done;
            *recs += 1; *bytes = end;
            if (*recs >= max_recs) goto done;
            if (at_least >= 0 && end >= at_least) goto done;
        }
        pos += n;
    }
done:
    io->close(io->ctx, f);
    return ok;
}

// Do archive bytes [arc_off, arc_off+n) equal device bytes [dev_off, dev_off+n)?
// -1 on a read error.
static int same_bytes(const rot_io_t *io, long arc_off, long dev_off, long n)
{
    void *a = io->open(io->ctx, ROT_ARC, "rb");
    void *d = a ? io->open(io->ctx, ROT_DEV, "rb") : NULL;
    int res = (a && d && io->seek(io->ctx, a, arc_off) && io->seek(io->ctx, d, dev_off)) ? 1 : -1;
    long half = (long)io->buf_sz / 2;
    char *ba = io->buf, *bd = io->buf + half;
    while (res == 1 && n > 0) {
        long want = n < half ? n : half;
        long ga = io->read(io->ctx, a, ba, want);
        long gd = io->read(io->ctx, d, bd, want);
        if (ga < 0 || gd < 0) { res = -1; break; }
        if (ga != want || gd != want) { res = 0; break; }   // one file ended early
        if (memcmp(ba, bd, (size_t)want) != 0) { res = 0; break; }
        n -= want;
    }
    if (d) io->close(io->ctx, d);
    if (a) io->close(io->ctx, a);
    return res;
}

// Copy n bytes from an open read handle to an open write handle.
static bool copy_n(const rot_io_t *io, void *in, void *out, long n)
{
    while (n != 0) {
        long want = (n < 0 || n > (long)io->buf_sz) ? (long)io->buf_sz : n;
        long g = io->read(io->ctx, in, io->buf, want);
        if (g < 0) return false;
        if (g == 0) return n < 0;            // EOF: fine only for "to the end"
        if (io->write(io->ctx, out, io->buf, g) != g) return false;
        if (n > 0) n -= g;
    }
    return true;
}

// Rewrite the device log to qso.tmp: header h2, then everything after the
// first `skip` bytes of records. Then swap it in. The commit point of every
// change this file makes to the device log. False with r->status set.
static bool commit_rewrite(const rot_io_t *io, rot_result_t *r, long dev_sz, long hdr_len,
                           long skip, const rot_hdr_t *h2)
{
    char hdr[160];
    int nh = rot_hdr_format(hdr, sizeof hdr, h2);
    if (!io->reserve(io->ctx, dev_sz - hdr_len - skip + nh)) {
        say(io, r, ROT_FULL, true, "storage cannot take the rewritten log - nothing removed from it, retried next boot");
        return false;
    }
    void *in = io->open(io->ctx, ROT_DEV, "rb");
    void *out = in ? io->open(io->ctx, ROT_TMP, "wb") : NULL;
    bool ok = in && out && io->seek(io->ctx, in, hdr_len + skip) &&
              io->write(io->ctx, out, hdr, nh) == nh && copy_n(io, in, out, -1);
    if (out && !io->close(io->ctx, out)) ok = false;
    if (in) io->close(io->ctx, in);
    if (!ok) {
        io->remove(io->ctx, ROT_TMP);
        say(io, r, ROT_IO, true, "device log rewrite failed - log unchanged, retried next boot");
        return false;
    }
    io->remove(io->ctx, ROT_DEV);
    if (!io->rename_tmp(io->ctx)) {
        say(io, r, ROT_IO, true, "rename qso.tmp -> qso.adi failed - boot recovers it");
        return false;
    }
    return true;
}

// ---- the run ---------------------------------------------------------------

void rot_run(const rot_io_t *io, uint32_t max_rotate, rot_result_t *r)
{
    char msg[128];
    memset(r, 0, sizeof *r);
    r->arc_size = io->size(io->ctx, ROT_ARC);

    long dev_sz = io->size(io->ctx, ROT_DEV);
    if (dev_sz <= 0) { say(io, r, ROT_NOTHING, false, "no device log"); return; }

    // 1. The device log's header line.
    long hdr_len = 0;
    rot_hdr_t h = {0};
    {
        void *f = io->open(io->ctx, ROT_DEV, "rb");
        if (!f) { say(io, r, ROT_IO, true, "cannot open the device log"); return; }
        long want = dev_sz < HDR_MAX ? dev_sz : HDR_MAX;
        long g = io->read(io->ctx, f, io->buf, want);
        io->close(io->ctx, f);
        if (g != want) { say(io, r, ROT_IO, true, "cannot read the device log header"); return; }
        const char *nl = memchr(io->buf, '\n', (size_t)g);
        if (!nl) { say(io, r, ROT_IO, true, "device log has no header line"); return; }
        hdr_len = (long)(nl - io->buf) + 1;
        rot_hdr_parse(io->buf, (size_t)hdr_len, &h);
    }
    r->hdr = h;

    uint32_t nrec = 0; long whole = 0;
    if (!walk(io, hdr_len, UINT32_MAX, LONG_MAX, -1, &nrec, &whole)) {
        say(io, r, ROT_IO, true, "cannot read the device log"); return;
    }
    r->dev_records = nrec;

    // 2. Where this log's part of the archive ends (base), or -1 if there is
    //    no archive yet. Any doubt about whose archive it is: do nothing.
    const long hl = (long)strlen(ROT_ARC_HEADER);
    long arc = r->arc_size;
    long base;
    // The card has no archive at all, but the log says it moved records to
    // one: a new card, or the archive was deleted on a PC. Those records are
    // gone from THIS card whatever happens here, and refusing would block
    // rotation for good (found 2026-11-13 planning the bench test). An
    // archive that EXISTS but is not this log's is still never touched
    // (MISMATCH below).
    //
    // ⛔ The relink is its OWN commit, before any new archive exists: the
    // header goes back to plain with the records untouched, io->relinked()
    // resets the cursor base, and only then does a normal first rotation
    // run. Folding both into one commit was wrong twice: the cursor shift
    // would be (new count - old count) instead of the records removed, and a
    // cut after the new archive was created but before the commit left a
    // header pointing at an archive of a different size - MISMATCH for ever.
    if (h.arc_bytes != 0 && r->arc_size < 0) {
        snprintf(msg, sizeof msg, "no archive on this card, but the log moved %lu records to one - starting again; those records are not on this card",
                 (unsigned long)h.archived);
        if (io->log) io->log(io->ctx, true, msg);
        rot_hdr_t plain = {0, 0};
        if (!commit_rewrite(io, r, dev_sz, hdr_len, 0, &plain)) return;
        if (io->relinked) io->relinked(io->ctx);
        uint32_t was = h.archived;
        rot_run(io, max_rotate, r);          // header is plain now: no second relink
        r->relinked = true;
        r->relinked_from = was;
        return;
    }
    if (h.arc_bytes == 0) {
        if (h.archived != 0) { say(io, r, ROT_MISMATCH, true, "log header names archived records but no archive size"); return; }
        if (arc < 0) base = -1;
        else if (arc < hl) {
            // The archive's creation was cut before its header landed.
            if (!io->remove(io->ctx, ROT_ARC)) { say(io, r, ROT_IO, true, "cannot remove a torn archive header"); return; }
            arc = -1; base = -1;
        } else {
            void *f = io->open(io->ctx, ROT_ARC, "rb");
            long g = f ? io->read(io->ctx, f, io->buf, hl) : -1;
            if (f) io->close(io->ctx, f);
            if (g != hl) { say(io, r, ROT_IO, true, "cannot read the archive header"); return; }
            if (memcmp(io->buf, ROT_ARC_HEADER, (size_t)hl) != 0) {
                say(io, r, ROT_MISMATCH, true, "the card's archive is not one this firmware wrote"); return;
            }
            base = hl;
        }
    } else {
        if (arc < 0) {
            snprintf(msg, sizeof msg, "log says %lu records are on the card, but there is no archive - wrong card?",
                     (unsigned long)h.archived);
            say(io, r, ROT_MISMATCH, true, msg); return;
        }
        if (arc < (long)h.arc_bytes) {
            snprintf(msg, sizeof msg, "archive is %ld B, log says %lu B - wrong card?", arc, (unsigned long)h.arc_bytes);
            say(io, r, ROT_MISMATCH, true, msg); return;
        }
        base = (long)h.arc_bytes;
    }

    // 3. Bytes past base: an interrupted move. They must be the head of the
    //    device log, byte for byte - a torn last record included.
    long excess = (base >= 0) ? arc - base : 0;
    uint32_t K = 0; long kbytes = 0;
    if (excess > 0) {
        int same = (excess <= dev_sz - hdr_len) ? same_bytes(io, base, hdr_len, excess) : 0;
        if (same < 0) { say(io, r, ROT_IO, true, "cannot compare the archive with the log"); return; }
        if (same == 0) {
            snprintf(msg, sizeof msg, "archive has %ld B past what this log moved, and they are not this log's records - wrong card?", excess);
            say(io, r, ROT_MISMATCH, true, msg); return;
        }
        if (!walk(io, hdr_len, UINT32_MAX, excess, -1, &K, &kbytes)) { say(io, r, ROT_IO, true, "cannot read the device log"); return; }
    }

    // 4. How many records leave: enough to get under ROT_LOW_BYTES once over
    //    ROT_CAP_BYTES, at least the ones already on the card, and never one
    //    an upload has not sent.
    uint32_t want = 0; long wbytes = 0;
    if (whole > ROT_CAP_BYTES &&
        !walk(io, hdr_len, UINT32_MAX, LONG_MAX, whole - ROT_LOW_BYTES, &want, &wbytes)) {
        say(io, r, ROT_IO, true, "cannot read the device log"); return;
    }
    uint32_t R = want > K ? want : K;
    if (R > max_rotate) R = max_rotate;
    r->held = want > R ? want - R : 0;
    long rbytes = 0;
    if (!walk(io, hdr_len, R, LONG_MAX, -1, &R, &rbytes)) { say(io, r, ROT_IO, true, "cannot read the device log"); return; }
    uint32_t keep_k = R < K ? R : K;            // excess records that stay on the card
    long keep_kbytes = (keep_k == K) ? kbytes : rbytes;

    // 5. Tidy the archive's tail: a torn record, or moved records that may
    //    not leave after all (an upload fell behind). FAT truncate is safe;
    //    it is SPIFFS truncate that is not.
    if (base >= 0 && arc != base + keep_kbytes) {
        if (!io->truncate(io->ctx, ROT_ARC, base + keep_kbytes)) {
            say(io, r, ROT_IO, true, "cannot cut the archive's torn tail"); return;
        }
        snprintf(msg, sizeof msg, "archive tail cut %ld -> %ld B (interrupted move)", arc, base + keep_kbytes);
        if (io->log) io->log(io->ctx, true, msg);
        arc = base + keep_kbytes;
    }
    r->arc_size = arc;
    if (R == 0) {
        if (r->held) {
            snprintf(msg, sizeof msg, "log is over the cap, but %lu records wait for an upload - kept", (unsigned long)r->held);
            say(io, r, ROT_NOTHING, true, msg);
        } else say(io, r, ROT_NOTHING, false, "under the cap");
        return;
    }

    // 6. Append the rest to the archive, creating it if this is the first move.
    if (base < 0) {
        void *f = io->open(io->ctx, ROT_ARC, "wb");
        bool ok = f && io->write(io->ctx, f, ROT_ARC_HEADER, hl) == hl;
        if (f && !io->close(io->ctx, f)) ok = false;
        if (!ok || io->size(io->ctx, ROT_ARC) != hl) { say(io, r, ROT_IO, true, "cannot create the archive"); return; }
        base = hl; arc = hl;
    }
    if (rbytes > keep_kbytes) {
        void *in = io->open(io->ctx, ROT_DEV, "rb");
        void *out = in ? io->open(io->ctx, ROT_ARC, "ab") : NULL;
        bool ok = in && out && io->seek(io->ctx, in, hdr_len + keep_kbytes) &&
                  copy_n(io, in, out, rbytes - keep_kbytes);
        if (out && !io->close(io->ctx, out)) ok = false;
        if (in) io->close(io->ctx, in);
        long now = io->size(io->ctx, ROT_ARC);
        r->arc_size = now;
        if (!ok || now != base + rbytes) {
            snprintf(msg, sizeof msg, "archive append failed (%ld of %ld B) - nothing removed from the log",
                     now - arc, rbytes - keep_kbytes);
            say(io, r, ROT_IO, true, msg); return;
        }
    }

    // 7. Rewrite the device log without the moved records. The commit point.
    rot_hdr_t h2 = { h.archived + R, (uint32_t)(base + rbytes) };
    if (!commit_rewrite(io, r, dev_sz, hdr_len, rbytes, &h2)) {
        if (r->status == ROT_FULL || r->status == ROT_IO)
            if (io->log) io->log(io->ctx, true, "the moved records stay in both files; the move finishes next boot");
        return;
    }
    r->hdr = h2;
    r->rotated = R;
    r->finished = keep_k;
    r->dev_records = nrec - R;
    snprintf(msg, sizeof msg, "moved %lu records to the card (%lu finished from an interrupted move); %lu on the device, %lu on the card",
             (unsigned long)R, (unsigned long)keep_k, (unsigned long)r->dev_records, (unsigned long)h2.archived);
    say(io, r, ROT_DONE, false, msg);
}
