// psk_harness.c — host-side verification of the PSK Reporter datagram
// (main/net/pskreporter.c).
//
// Build (from test/):
//   gcc -O2 -Wall -o psk_harness.exe psk_harness.c
//
// Run (from test/):
//   ./psk_harness.exe                 # self-tests only
//   ./psk_harness.exe capture.bin     # also validate + dump a real datagram
//
// WHY THIS EXISTS
//   PSK Reporter is fed over UDP with no acknowledgement of any kind. A
//   malformed datagram is silently discarded by the collector, so "no error in
//   the device log" is not evidence that anything worked. The only on-air
//   proof is an entry appearing in pskreporter.info's "Software in use" table,
//   which needs a real antenna and a real operator. This harness closes the
//   gap that CAN be closed on a bench: proving the bytes we emit are
//   well-formed IPFIX (RFC 7011) carrying the fields the spec asks for.
//
//   It does NOT prove the collector accepts our particular field combination.
//   Only the "Software in use" table proves that. Keep that distinction.
//
// WHAT IT PROVES
//   1. The parser below is a STRICT, INDEPENDENT implementation of IPFIX
//      message parsing — written from RFC 7011 + pskreporter.info/pskdev.html,
//      NOT by reading pskreporter.c's builder. It can therefore actually
//      disagree with the builder.
//   2. The parser ACCEPTS a hand-built reference datagram (built here from the
//      spec, byte by byte).
//   3. The parser REJECTS nine targeted corruptions of that same datagram.
//      This is the load-bearing part: a validator that accepts everything
//      proves nothing, so each corruption must be caught.
//   4. Given a real capture off the wire, it validates it and dumps every
//      decoded field, so the spots can be eyeballed against the device log.
//
// FOUND WITH THIS (2026-07-26): the receiver Data Set padded to 4 bytes
// unconditionally, but that set's shortest possible record is only 3 octets, so
// a 3-byte pad was byte-identical to a record of three empty strings - an
// RFC 7011 s3.3.1 violation that a collector may resolve either by ingesting a
// phantom empty receiver record or by dropping the message. It depended purely
// on callsign+grid+version string lengths, so it hit some stations and not
// others. Test [3] is the permanent regression vector.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define PEN_PSK 30351

// ---------------------------------------------------------------------------
// Independent strict IPFIX parser
// ---------------------------------------------------------------------------

#define MAX_TEMPLATES 8
#define MAX_FIELDS    16

typedef struct {
    uint16_t id;          // field id with the enterprise bit MASKED OFF
    bool     enterprise;
    uint32_t pen;
    uint16_t len;         // 0xFFFF = variable length
} field_t;

typedef struct {
    uint16_t id;
    int      n_fields;
    int      scope_count;  // >0 only for options templates
    field_t  f[MAX_FIELDS];
} template_t;

typedef struct {
    template_t t[MAX_TEMPLATES];
    int        n;
    char       err[256];
    int        n_data_records;
    int        n_templates_seen;
} parse_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

#define FAIL(P, ...) do { snprintf((P)->err, sizeof((P)->err), __VA_ARGS__); return false; } while (0)

static const char *psk_field_name(uint32_t pen, uint16_t id, bool ent)
{
    if (!ent && id == 150) return "flowStartSeconds";
    if (ent && pen == PEN_PSK) {
        switch (id) {
        case 1:  return "senderCallsign";
        case 2:  return "receiverCallsign";
        case 3:  return "senderLocator";
        case 4:  return "receiverLocator";
        case 5:  return "frequency";
        case 6:  return "sNR";
        case 7:  return "IMD";
        case 8:  return "decoderSoftware";
        case 9:  return "antennaInformation";
        case 10: return "mode";
        case 11: return "informationSource";
        case 12: return "persistentIdentifier";
        default: return "?enterprise";
        }
    }
    return "?unknown";
}

static template_t *find_template(parse_t *P, uint16_t id)
{
    for (int i = 0; i < P->n; i++) if (P->t[i].id == id) return &P->t[i];
    return NULL;
}

// Parse a template or options-template set body.
static bool parse_template_set(parse_t *P, const uint8_t *b, int len, bool options)
{
    int off = 0;
    while (off < len) {
        // A set may be zero-padded to a 4-byte boundary; a run of trailing
        // zeros shorter than a minimal record is padding, not a record.
        if (len - off < 4) {
            for (int i = off; i < len; i++)
                if (b[i]) FAIL(P, "non-zero trailing byte in template set at +%d", i);
            return true;
        }
        uint16_t tid = rd16(b + off);
        uint16_t cnt = rd16(b + off + 2);
        if (tid == 0 && cnt == 0) {   // pure padding
            for (int i = off; i < len; i++)
                if (b[i]) FAIL(P, "non-zero byte after template padding at +%d", i);
            return true;
        }
        off += 4;
        if (tid < 256) FAIL(P, "template id %u must be >= 256", tid);
        if (cnt == 0 || cnt > MAX_FIELDS) FAIL(P, "template %u field count %u out of range", tid, cnt);

        int scope = 0;
        if (options) {
            if (len - off < 2) FAIL(P, "options template %u truncated before scope count", tid);
            scope = rd16(b + off);
            off += 2;
            if (scope == 0 || scope > cnt)
                FAIL(P, "options template %u scope count %d invalid (fields=%u)", tid, scope, cnt);
        }

        if (P->n >= MAX_TEMPLATES) FAIL(P, "too many templates");
        template_t *t = &P->t[P->n++];
        memset(t, 0, sizeof(*t));
        t->id = tid;
        t->n_fields = cnt;
        t->scope_count = scope;

        for (int i = 0; i < cnt; i++) {
            if (len - off < 4) FAIL(P, "template %u field %d truncated", tid, i);
            uint16_t raw = rd16(b + off);
            uint16_t flen = rd16(b + off + 2);
            off += 4;
            field_t *f = &t->f[i];
            f->enterprise = (raw & 0x8000) != 0;
            f->id = raw & 0x7FFF;
            f->len = flen;
            if (f->enterprise) {
                if (len - off < 4) FAIL(P, "template %u field %d missing PEN", tid, i);
                f->pen = rd32(b + off);
                off += 4;
                if (f->pen != PEN_PSK)
                    FAIL(P, "template %u field %d has PEN %u, expected %u", tid, i, f->pen, PEN_PSK);
            }
            if (flen == 0) FAIL(P, "template %u field %d has zero length", tid, i);
        }
        P->n_templates_seen++;
    }
    return true;
}

// Decode one data record against a template. Advances *off. Dumps if verbose.
static bool decode_record(parse_t *P, template_t *t, const uint8_t *b, int len,
                          int *off, bool verbose)
{
    if (verbose) printf("    record (template 0x%04X):\n", t->id);
    for (int i = 0; i < t->n_fields; i++) {
        field_t *f = &t->f[i];
        const char *nm = psk_field_name(f->pen, f->id, f->enterprise);
        if (f->len == 0xFFFF) {
            if (*off >= len) FAIL(P, "var-length field '%s' has no length octet", nm);
            int n = b[*off];
            (*off)++;
            if (n == 255) {
                if (*off + 2 > len) FAIL(P, "var-length field '%s' truncated 3-byte length", nm);
                n = rd16(b + *off);
                *off += 2;
            }
            if (*off + n > len)
                FAIL(P, "var-length field '%s' claims %d bytes, only %d left", nm, n, len - *off);
            if (verbose) printf("      %-18s = \"%.*s\" (%d)\n", nm, n, (const char *)(b + *off), n);
            *off += n;
        } else {
            if (*off + f->len > len)
                FAIL(P, "fixed field '%s' (%u bytes) overruns set", nm, f->len);
            if (verbose) {
                if (f->len == 4) {
                    printf("      %-18s = %u\n", nm, rd32(b + *off));
                } else if (f->len == 1) {
                    printf("      %-18s = %d\n", nm, (int)(int8_t)b[*off]);
                } else {
                    printf("      %-18s = <%u bytes>\n", nm, f->len);
                }
            }
            *off += f->len;
        }
    }
    P->n_data_records++;
    return true;
}

static bool parse_ipfix(parse_t *P, const uint8_t *buf, int len, bool verbose)
{
    memset(P, 0, sizeof(*P));
    if (len < 16) FAIL(P, "message shorter than a 16-byte header (%d)", len);

    uint16_t ver = rd16(buf);
    uint16_t mlen = rd16(buf + 2);
    if (ver != 0x000A) FAIL(P, "version 0x%04X, expected 0x000A", ver);
    if (mlen != len) FAIL(P, "header length %u != actual %d", mlen, len);

    if (verbose) {
        printf("  header: version=0x%04X length=%u exportTime=%u seq=%u domainId=0x%08X\n",
               ver, mlen, rd32(buf + 4), rd32(buf + 8), rd32(buf + 12));
    }

    int off = 16;
    while (off < len) {
        if (len - off < 4) FAIL(P, "trailing %d bytes too short for a set header", len - off);
        uint16_t sid = rd16(buf + off);
        uint16_t slen = rd16(buf + off + 2);
        if (slen < 4) FAIL(P, "set 0x%04X length %u < 4", sid, slen);
        if (off + slen > len) FAIL(P, "set 0x%04X length %u overruns message", sid, slen);

        const uint8_t *body = buf + off + 4;
        int blen = slen - 4;

        if (sid == 2 || sid == 3) {
            if (verbose) printf("  %s set (len %u)\n",
                               sid == 2 ? "template" : "options-template", slen);
            if (!parse_template_set(P, body, blen, sid == 3)) return false;
        } else if (sid >= 256) {
            template_t *t = find_template(P, sid);
            if (!t) FAIL(P, "data set 0x%04X has no preceding template", sid);
            if (verbose) printf("  data set 0x%04X (len %u)\n", sid, slen);
            // Smallest record this template can produce: every fixed field at
            // its stated width, every variable field at one length octet with
            // an empty value. Fewer bytes than this remaining means what's left
            // cannot be a record, so it must be zero padding.
            int min_rec = 0;
            for (int i = 0; i < t->n_fields; i++)
                min_rec += (t->f[i].len == 0xFFFF) ? 1 : t->f[i].len;

            int o = 0;
            while (blen - o >= min_rec) {
                // RFC 7011 s3.3.1: if padding is present its length MUST be
                // shorter than the shortest record the template can produce -
                // otherwise padding is indistinguishable from a record of
                // empty values, and a collector may either ingest a phantom
                // record or reject the message. Catch that ambiguity here
                // rather than silently decoding zeros as a record.
                bool rest_zero = true;
                for (int i = o; i < blen; i++) if (body[i]) { rest_zero = false; break; }
                if (rest_zero)
                    FAIL(P, "data set 0x%04X: %d trailing zero bytes but min record is %d"
                            " - padding must be SHORTER than a record (RFC 7011 3.3.1);"
                            " ambiguous with an all-empty record",
                         sid, blen - o, min_rec);
                int before = o;
                if (!decode_record(P, t, body, blen, &o, verbose)) return false;
                if (o == before) FAIL(P, "data set 0x%04X made no progress", sid);
            }
            // Whatever remains is padding and MUST be zero (RFC 7011 §3.3.1).
            if (blen - o >= 4)
                FAIL(P, "data set 0x%04X has %d unparsed bytes (min record %d)", sid, blen - o, min_rec);
            for (int i = o; i < blen; i++)
                if (body[i]) FAIL(P, "data set 0x%04X padding byte at +%d is 0x%02X, not zero", sid, i, body[i]);
        } else {
            FAIL(P, "reserved set id %u", sid);
        }
        off += slen;
    }
    if (off != len) FAIL(P, "sets end at %d, message length %d", off, len);
    return true;
}

// ---------------------------------------------------------------------------
// Hand-built reference datagram (from the spec, independent of the builder)
// ---------------------------------------------------------------------------

static int put_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
    return (int)n + 1;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

// Offsets recorded so the corruption tests can target exact bytes.
typedef struct {
    int rx_set_len;    // offset of the receiver data set's length field
    int tx_set_len;
    int first_varlen;  // offset of the first var-length length octet in tx data
    int pad_byte;      // offset of a padding byte (or -1)
} ref_off_t;

static int build_reference(uint8_t *b, ref_off_t *ro, const char *sw)
{
    memset(ro, 0, sizeof(*ro));
    ro->pad_byte = -1;
    int off = 16;

    // --- options template set (receiver info), set id 3 ---
    int s = off;
    wr16(b + off, 3); off += 2;
    int lenpos = off; off += 2;
    wr16(b + off, 0x9992); off += 2;   // template id
    wr16(b + off, 3);      off += 2;   // field count
    wr16(b + off, 1);      off += 2;   // scope count
    const uint16_t rxf[3] = { 2, 4, 8 };  // receiverCallsign, receiverLocator, decoderSoftware
    for (int i = 0; i < 3; i++) {
        wr16(b + off, (uint16_t)(0x8000 | rxf[i])); off += 2;
        wr16(b + off, 0xFFFF);                      off += 2;
        wr32(b + off, PEN_PSK);                     off += 4;
    }
    while ((off - s) % 4) b[off++] = 0;
    wr16(b + lenpos, (uint16_t)(off - s));

    // --- template set (sender records), set id 2 ---
    s = off;
    wr16(b + off, 2); off += 2;
    lenpos = off; off += 2;
    wr16(b + off, 0x9993); off += 2;
    wr16(b + off, 7);      off += 2;
    struct { uint16_t id; uint16_t len; bool ent; } txf[7] = {
        { 1,   0xFFFF, true  },   // senderCallsign
        { 5,   4,      true  },   // frequency
        { 6,   1,      true  },   // sNR
        { 10,  0xFFFF, true  },   // mode
        { 11,  1,      true  },   // informationSource
        { 3,   0xFFFF, true  },   // senderLocator
        { 150, 4,      false },   // flowStartSeconds
    };
    for (int i = 0; i < 7; i++) {
        wr16(b + off, (uint16_t)((txf[i].ent ? 0x8000 : 0) | txf[i].id)); off += 2;
        wr16(b + off, txf[i].len);                                        off += 2;
        if (txf[i].ent) { wr32(b + off, PEN_PSK); off += 4; }
    }
    while ((off - s) % 4) b[off++] = 0;
    wr16(b + lenpos, (uint16_t)(off - s));

    // --- receiver data set (0x9992) ---
    s = off;
    wr16(b + off, 0x9992); off += 2;
    ro->rx_set_len = off; off += 2;
    off += put_str(b + off, "OZ1LAV");
    off += put_str(b + off, "JO65");
    off += put_str(b + off, sw);
    while ((off - s) % 4) { if (ro->pad_byte < 0) ro->pad_byte = off; b[off++] = 0; }
    wr16(b + ro->rx_set_len, (uint16_t)(off - s));

    // --- sender data set (0x9993), two spots ---
    s = off;
    wr16(b + off, 0x9993); off += 2;
    ro->tx_set_len = off; off += 2;
    struct { const char *call; uint32_t f; int8_t snr; const char *mode; const char *grid; uint32_t t; } sp[2] = {
        { "K1ABC",     14075500, -12, "FT8", "FN42", 1770000000u },
        { "PJ4/K9XYZ", 14074900,  -3, "FT4", "",     1770000015u },
    };
    for (int i = 0; i < 2; i++) {
        if (i == 0) ro->first_varlen = off;
        off += put_str(b + off, sp[i].call);
        wr32(b + off, sp[i].f); off += 4;
        b[off++] = (uint8_t)sp[i].snr;
        off += put_str(b + off, sp[i].mode);
        b[off++] = 1;
        off += put_str(b + off, sp[i].grid);
        wr32(b + off, sp[i].t); off += 4;
    }
    while ((off - s) % 4) b[off++] = 0;
    wr16(b + ro->tx_set_len, (uint16_t)(off - s));

    wr16(b, 0x000A);
    wr16(b + 2, (uint16_t)off);
    wr32(b + 4, 1770000020u);
    wr32(b + 8, 0);
    wr32(b + 12, 0xDEADBEEF);
    return off;
}

// ---------------------------------------------------------------------------
// Self-tests
// ---------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;

static void expect_ok(const uint8_t *b, int len, const char *what)
{
    parse_t P;
    if (parse_ipfix(&P, b, len, false)) {
        printf("  PASS  %-52s (%d templates, %d records)\n", what, P.n_templates_seen, P.n_data_records);
        g_pass++;
    } else {
        printf("  FAIL  %-52s -> unexpectedly rejected: %s\n", what, P.err);
        g_fail++;
    }
}

static void expect_reject(uint8_t *b, int len, const char *what)
{
    parse_t P;
    if (!parse_ipfix(&P, b, len, false)) {
        printf("  PASS  %-52s -> rejected: %s\n", what, P.err);
        g_pass++;
    } else {
        printf("  FAIL  %-52s -> WRONGLY ACCEPTED\n", what);
        g_fail++;
    }
}

int main(int argc, char **argv)
{
    static uint8_t ref[2048], tmp[2048];
    ref_off_t ro;
    int rlen = build_reference(ref, &ro, "QMX Panadapter v1.3.1");

    printf("PSK Reporter datagram harness\n");
    printf("reference datagram: %d bytes\n\n", rlen);

    printf("[1] parser accepts a spec-conformant datagram\n");
    expect_ok(ref, rlen, "hand-built reference (2 templates, 3 records)");

    printf("\n[2] parser rejects targeted corruptions\n");
    #define MUT(desc, code) do { memcpy(tmp, ref, rlen); int L = rlen; code; expect_reject(tmp, L, desc); } while (0)

    MUT("wrong version (0x0009)",              wr16(tmp, 0x0009));
    MUT("header length disagrees with buffer", wr16(tmp + 2, (uint16_t)(rlen - 4)));
    MUT("truncated message (last 8 bytes cut)", L = rlen - 8);
    MUT("receiver set length too large",       wr16(tmp + ro.rx_set_len, 0x0FFF));
    MUT("sender set length too small (2)",     wr16(tmp + ro.tx_set_len, 2));
    MUT("var-length string overruns its set",  tmp[ro.first_varlen] = 0x7F);
    // Offset 30 is field[0]'s PEN in the options-template set: 16 set-header(4)
    // + template id(2) + field count(2) + scope count(2) + field id(2) + len(2).
    MUT("bad PEN on a template field",         wr32(tmp + 30, 12345));
    MUT("non-zero padding in a data set",
        do { if (ro.pad_byte >= 0) tmp[ro.pad_byte] = 0xAB; } while (0));

    // Regression vector for the bug this harness found on real hardware
    // (2026-07-26): the receiver Data Set's shortest record is 3 octets, so a
    // 3-byte alignment pad is byte-identical to a record of three empty
    // strings. Whether the natural pad reaches 3 depends only on
    // callsign+grid+software lengths, so it silently corrupted the datagram
    // for some stations and not others. build_reference() still pads
    // unconditionally, so feeding it a software string that forces a 3-byte
    // pad reproduces the original bad datagram - and must be rejected.
    printf("\n[3] regression: 3-byte pad on a 3-octet-minimum record\n");
    {
        static uint8_t bad[2048];
        ref_off_t bro;
        int blen = build_reference(bad, &bro, "QMX Panadapter v1.3.1-9-gc62eca5");
        expect_reject(bad, blen, "pad == min record length (RFC 7011 3.3.1)");
    }

    printf("\n[4] reference decode dump (sanity-check field values)\n");
    { parse_t P; parse_ipfix(&P, ref, rlen, true); }

    if (argc > 1) {
        printf("\n[4] validating real capture: %s\n", argv[1]);
        FILE *f = fopen(argv[1], "rb");
        if (!f) { printf("  cannot open %s\n", argv[1]); g_fail++; }
        else {
            static uint8_t cap[65536];
            int n = (int)fread(cap, 1, sizeof(cap), f);
            fclose(f);
            printf("  %d bytes read\n", n);
            parse_t P;
            if (parse_ipfix(&P, cap, n, true)) {
                printf("  PASS  real device datagram is well-formed IPFIX"
                       " (%d templates, %d records)\n", P.n_templates_seen, P.n_data_records);
                g_pass++;
            } else {
                printf("  FAIL  real device datagram REJECTED: %s\n", P.err);
                g_fail++;
            }
        }
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
