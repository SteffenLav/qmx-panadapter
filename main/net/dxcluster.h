#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// DX cluster spot feed - the HUMAN half of the spotting world.
//
// RBN (net/rbn.c) is automated skimmers, so it only ever reports what a machine
// can decode: CW, RTTY, FT8, FT4. There is no such thing as an SSB skimmer, so
// **phone spots are structurally invisible to RBN** - which is exactly what the
// operator noticed. A DX cluster is people typing, so it carries SSB, and it
// carries park/summit references in the comment.
//
// Deliberately a SEPARATE client from rbn.c rather than a mode of it: the two
// speak different line formats (a cluster comment is free text with no dB and
// usually no mode), and keeping them apart means a misbehaving cluster node can
// be switched off without losing RBN, and vice versa.
//
// ⚠ This opens a SECOND long-lived TCP connection alongside RBN, on a board
// whose WiFi link is its most fragile component (see the esp_hosted history in
// CLAUDE.md). Opt-in for that reason. Traffic is a few lines per second at
// most, and everything is filtered to the current band at ingest.

void dxcluster_init(void);

// Seconds since the last accepted spot line, or -1 if none yet.
int  dxcluster_age_s(void);

// Stations currently held (after de-duplication and band filtering).
int  dxcluster_spot_count(void);

// Parse one "DX de ..." cluster line. Returns false for anything that is not a
// spot - login banners, node chatter, a truncated line. Exposed for the boot
// self-test, which runs it against lines captured from a real node.
//   call_out : the spotted station
//   freq_hz  : the spot frequency
//   ref_out  : park/summit reference found in the comment, "" if none
//   mode_out : 0 other, 1 CW, 2 SSB, 3 digital (matches spot_mode_t)
bool dxcluster_parse_line(const char *line,
                          char *call_out, size_t call_cap,
                          uint32_t *freq_hz,
                          char *ref_out, size_t ref_cap,
                          int *mode_out);

// Boot self-test over captured real lines. Logs PASS/FAIL.
void dxcluster_selftest(void);
