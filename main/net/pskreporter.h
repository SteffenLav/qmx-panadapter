#pragma once
#include <stdint.h>

// PSK Reporter spotting (https://pskreporter.info/pskdev.html): every REAL
// FT8/FT4 decode is queued and reported in batched UDP datagrams (IPFIX) to
// report.pskreporter.info:4739 at most once per ~5 minutes, so the Tab5
// appears as a monitoring station on the map and contributes reception
// reports the way WSJT-X does.
//
// Rules honoured from the spec: one datagram per >=5 min with randomized
// timing (based on start-up, never the wall clock), a callsign reported at
// most once per batch, the same UDP source port for the whole session, the
// record format descriptors sent in the first three datagrams and hourly
// thereafter, sequence number = cumulative report count, and a constant
// per-session random identifier.
//
// Gating: the "Report decodes to PSK Reporter" setting (pskreporter_en,
// default off), callsign+grid configured, WiFi up - and NEVER in simulation
// mode (the spot hook lives in the real-decode path only, and the feeder
// additionally refuses spots while sim_mode_en is set: phantoms must not
// reach the real world).

// Start the background sender task (idempotent). Called once from app init.
void pskreporter_init(void);

// Queue one decoded station. call/mode required; grid may be "" (unknown);
// freq_hz is the RF frequency (dial + audio offset); utc_sec is the decode
// slot time. Cheap and non-blocking - safe from the decode task.
void pskreporter_spot(const char *call, const char *grid,
                      uint32_t freq_hz, int snr_db,
                      const char *mode, int64_t utc_sec);
