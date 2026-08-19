# Overnight soak — 2026-08-19, FT8 mode

Capture: `scratchpad/soak_0819_night.txt` (no expiry, no `-Reset`).
Started 16:39 local, device uptime was ~45 min at that point.

## Why this run exists

Two things shipped in v1.8.7 that a panadapter-mode soak cannot test, and one
that has never been seen working at all.

1. **#191 (SPIFFS ENOSPC)** — the GC-budget fix was only ever tested in
   **panadapter mode**, where the diagnostic log grows ~16 KB/min. **FT8 is
   ~47 KB/min**, about three times the pressure, and the 256 KB rolling file
   therefore rotates roughly every 11 minutes instead of every 30-odd. The
   failure being guarded against is `ENOSPC` with several hundred KB still
   free, so only the faster rotation rate exercises it. **This run is the
   definitive test.**
2. **Standing patch #8** — `_buffer_parse_error`'s bare `abort()`. It has
   **never been observed firing**. The v1.8.6 crash it fixes appeared at
   **7 h 06 m** of a healthy FT8 session, so nothing shorter than a night can
   say anything about it. Its counter is the evidence, not the log.
3. **#199** — the FT8 monitor pool double-build. Only the *reboot* is fixed;
   the double build itself is still open, and the new
   `monitor pool rebuilt with no teardown` warning is the detector.

## Starting state

| | |
|---|---|
| Firmware | **v1.8.7** (the released binary, flashed from the release ZIP) |
| Screen | FT8 |
| Radio | QMX 1_04_004, DiGi, 20 m, 14.074 MHz, `tune_ok` |
| Sub-mode | FT8 (15 s slots) |
| Battery | 81 %, 7977 mV, **not charging** — see the warning below |
| BLE | **off** (`en:false, conn:false`) |
| Sim mode | off |
| Auto-answer robot | off |
| Auto-work pileup | off |
| Pick callers myself | **on** (operator ticked it; harmless with no CQ running) |
| USB patch counters | `chan_err_no_halt: 0`, `unexpected_pipe_event: 0` — **the baseline** |
| ADIF | 37 QSOs |

⚠ **Battery reads 81 % and NOT charging.** If that is the battery-care charge
limit doing its job the unit is on mains and fine. If it is genuinely running
on the pack, the soak will end when it flattens and that must NOT be read as a
crash. Check the battery trend first thing before interpreting an early stop.

## What to look for in the morning

Check the capture is alive AND its `LastWriteTime` is within seconds before
trusting anything — an expired capture reads exactly like a quiet healthy
device.

```
grep -c "Loaded app from partition"          # reboots. Expect 0.
grep -c "assert failed\|Guru Meditation"     # crashes. Expect 0.
grep -c "ENOSPC\|No space left"              # #191. Expect 0 - THE headline result.
grep -c "rebuilt with no teardown"           # #199 detector. Expect 0.
grep -c "draining to recover"                # SDIO recoveries - expect some, they self-heal.
grep "usb_patch\|patch counter"              # #8 / #7 counters - 0 means the fault did not occur.
grep -c "mutex timeout"                      # ft8_screen contention, see below.
```

⚠ **A zero USB-patch counter is NOT proof the patch works.** It says the fault
did not occur. That distinction is the entire reason #189 added the counters.

## Two things already observed, recorded so they are not "discovered" twice

- **`ft8_screen: record_decode: mutex timeout, dropping '<msg>'`** — 12 of them
  across the 42 minutes before this run started, spread evenly rather than
  clustered, so roughly one per 3.5 min. Each one **drops a decode**. Not
  investigated. Worth counting over a full night to see whether the rate is
  steady or grows.
- **The web server stopped answering while the device stayed perfectly healthy**
  (audio streaming, FT8 decoding, `wifi: online` heartbeats continuing, ping and
  TCP-connect both fine). All 149 httpd errors were **errno 11 (EAGAIN)**, i.e.
  send-buffer congestion, not socket exhaustion. ⚠ **It coincided exactly with a
  burst of rapid API polling from the dev machine, so it is most likely
  self-inflicted and must not be recorded as a v1.8.7 regression without a
  repeat under normal use.** If a user reports the web UI dropping out, this note
  is the starting point - and #193's mechanism (1), an HTTPS feed fetch starving
  the single-threaded httpd, is the first thing to test.
