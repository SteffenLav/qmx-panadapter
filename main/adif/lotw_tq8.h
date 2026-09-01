// lotw_tq8.h — portable TQ8 (LoTW digitally-signed log) generator.
//
// Builds the uncompressed TQ8 text; the caller gzips it and POSTs to
// https://lotw.arrl.org/lotw/upload (multipart field "upfile").
//
// Format reverse-verified against tqsllib source (location.cpp
// tqsl_getGABBItCONTACTData / tqsl_getGABBItSTATION / tqsl_getGABBItCERT,
// openssl_cert.cpp tqsl_signDataBlock/tqsl_encodeBase64, adif.cpp
// tqsl_adifMakeField) and the ARRL developer spec
// (lotw.arrl.org/lotw-help/developer-tq8). Signature spec = SIGN_LOTW_V2.0.
//
// Deliberately has NO ESP-IDF dependencies: compiled by the firmware
// (mbedtls sign callback) and by the host harness test/lotw_harness.c
// (openssl CLI sign callback) so the byte-level construction can be
// verified on a PC before ever touching the device.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Max RSA signature size we support (4096-bit key = 512 bytes).
#define LOTW_SIG_MAX 512

typedef struct {
    const char *callsign;    // station callsign, e.g. "OZ1LAV" (required)
    const char *dxcc;        // DXCC entity number as string, e.g. "221" (required)
    const char *gridsquare;  // e.g. "JO65HN" (NULL/"" = omit)
    const char *cqz;         // CQ zone, e.g. "14" (NULL/"" = omit)
    const char *ituz;        // ITU zone, e.g. "18" (NULL/"" = omit)
    // US subdivision (NULL/"" = omit). Field names in the tSTATION record are
    // TrustedQSL's INTERNAL names US_STATE / US_COUNTY, NOT the ADIF STATE /
    // CNTY: emit ADIF names and LoTW reports "ADIF field data length overflow",
    // rejects the whole tSTATION, and orphans every tCONTACT in the file.
    const char *us_state;    // 2-letter, e.g. "VA"
    const char *us_county;   // county NAME ALONE, e.g. "Arlington" - the combined
                             // ADIF "VA,Arlington" form is rejected as
                             // "US_COUNTY: Invalid value in field".
    const char *cert_pem;    // callsign certificate, full PEM incl. BEGIN/END lines (required)
} lotw_station_t;

// ⛔ The LoTW mode for an ADIF MODE/SUBMODE pair - NOT the ADIF mode itself.
//
// LoTW keeps its own list of modes for award credit and MFSK is not one of
// them: TQSL's configuration data maps each (ADIF mode, ADIF submode) pair
// onto a LoTW mode, and MFSK only ever appears on the ADIF side of that map.
// We sign our own TQ8s rather than handing an ADIF to TQSL, so this mapping is
// ours to do - and getting it wrong is silent, because a rejected upload only
// says so in a web page nobody reads twice.
//
// Since v1.9.6 the log writes FT4 the ADIF-correct way, MODE=MFSK with
// SUBMODE=FT4, so the pair must be collapsed back to "FT4" here. The result is
// byte-for-byte what this firmware already sent (and LoTW already accepted)
// when the log said MODE=FT4 - that identity is the point: an ADIF fix for
// POTA/ADIFMaster must not change one byte of the LoTW path.
//
// `submode` may be NULL or empty. Returns `mode` unchanged for everything else,
// including plain FT8, so no other mode's behaviour depends on this function.
const char *lotw_mode_from_adif(const char *mode, const char *submode);

typedef struct {
    const char *call;      // worked station (required)
    const char *band;      // LoTW band string, uppercase, e.g. "20M" (required)
    const char *mode;      // LoTW mode, e.g. "FT8" / "FT4" (required)
    const char *freq;      // MHz decimal string, e.g. "14.074" (NULL/"" = omit)
    const char *qso_date;  // "YYYY-MM-DD" — dashes, NOT ADIF YYYYMMDD (required)
    const char *qso_time;  // "HH:MM:SSZ" — colons + trailing Z (required)
} lotw_qso_t;

// Sign `len` bytes of `data` with RSA PKCS#1 v1.5 over SHA-1 using the
// station callsign certificate's private key. On entry *sig_len holds the
// capacity of sig (>= LOTW_SIG_MAX); on success set it to the actual
// signature length. Return 0 on success, nonzero on failure.
typedef int (*lotw_sign_fn)(void *ctx, const uint8_t *data, size_t len,
                            uint8_t *sig, size_t *sig_len);

// Build the complete uncompressed TQ8 text for n_qsos QSOs.
// app_ident: application name/version for the TQSL_IDENT header,
//            e.g. "QMX-Panadapter 0.21.0".
// Returns a NUL-terminated malloc'd buffer (caller frees with free()),
// or NULL on error with a message in errbuf.
char *lotw_tq8_build(const lotw_station_t *st,
                     const lotw_qso_t *qsos, int n_qsos,
                     const char *app_ident,
                     lotw_sign_fn sign, void *sign_ctx,
                     char *errbuf, size_t errlen);

// Exposed for the host harness's self-checks.
// Builds the exact string that gets signed for one QSO (station part +
// QSO fields, each value trimmed, whole string uppercased). Returns a
// malloc'd NUL-terminated string, or NULL on error.
char *lotw_tq8_signdata(const lotw_station_t *st, const lotw_qso_t *qso);

#ifdef __cplusplus
}
#endif
