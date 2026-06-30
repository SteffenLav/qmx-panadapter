# Version History

All releases are available on [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases).

## Latest Release

**v0.19.3** — 2026-06-30

- QMX IQ-mode handshake now retries automatically (up to 4 attempts) instead of silently giving up after one try — fixes a confusing bug where the spectrum could appear shifted/mirrored and tunable across the whole 48 kHz window for an entire session
- A red banner now appears across the top of the screen in the rare case the handshake still fails after all retries, instead of only a log line
- Full writeup in [Version History Document](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md)

### Installing v0.19.3

1. Use the one-click flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases)
2. Or follow [Build from Source](build/build.md)

### Upgrading from Earlier Versions

Your settings (callsign, grid, WiFi, memory channels) are preserved during a normal flash. If something seems stuck, use a **clean flash** to erase all settings:

1. Run the flasher
2. When prompted, type **E** for clean flash
3. Re-enter your settings on first boot

## Previous Releases

### v0.19.2

- microSD auto-archive: diagnostic log, ADIF log, and a config export are mirrored automatically to a microSD card when one is inserted
- Diagnostic log is now always-on and survives a power loss (a rolling copy persists to internal flash, downloadable even with no SD card)
- USB reconnect fix: power-cycling the QMX after WiFi is up no longer breaks audio/CAT
- Crash fix: a USB disconnect race that could reboot the Tab5 when the QMX dropped off USB
- QMX IQ mode is now verified, not assumed — the panadapter checks the radio actually accepted the I/Q-mode command instead of just checking the USB write succeeded
- FT8 continuation messages (report, RR73, 73) resend less often during an active QSO — decode time on busy slots cut by ~25-30%

### v0.19.1

- New project homepage at [tab5.lav.dk](https://tab5.lav.dk) — the user guide and reference as plain web pages
- Logbook uploads (QRZ / eQSL) and log downloads now work reliably while FT8 is running — no more reboots or dropped WiFi during a transfer

### v0.19.0

- FT4 transmit and receive (7.5-second slots, 105 symbols, 48 ms cadence) — CAT cadence verified on real QMX hardware
- Per-mode sticky frequency/bandwidth/filter recall between FT8 and FT4

### v0.18.8

- ARRL Field Day FT8 exchange mode (class + section in message, special TX sequence, ADIF logging)
- FT8 simulation mode (practice QSOs with phantom stations, no radio keyed)
- FT8 decode yield investigation closed (found and fixed 3 separate CPU contention issues)

### v0.18.7

- Decode-yield gap **closed** (controlled A/B with v0.18.0 confirmed fixes sufficient)
- Auto-answer robot mode un-shelved (live TX, full disclaimer on-screen)
- CQ tone auto-relocation on clash
- SNTP/QMX time-priority fix + FT8-derived auto-sync
- Flasher recovery mode + auto port detection

### v0.18.6

- FT8 sparse-decode investigation: found 3 separate regressions since v0.18.0
- Fixed: `cw_audio_init()` ghost task, missing poll-task CDC tolerance, waterfall style-set loop
- RST_SENT fix in CQ-run QSO
- Distance-in-miles display toggle
- Diag-log dot + firmware version in bottom bar

### v0.18.5

- Band-aid for FT8 decode regression (incomplete; issue fully addressed in v0.18.6)
- Critical bootloader-corruption hotfix (flasher wrote to 0x0 instead of 0x10000)
- FT8 double-spawn crash guard restored

### v0.18.4

- Band-nav strip (CW/Digi/Phone color zones below frequency axis)
- One-finger pan/stroll (spectrum scroll + center-freq readout)
- Snap-to-signal peak detection
- Band-aware worked-before (callsign + band memory)
- Robot mode (complete but **shelved** — greyed out, disabled)

### v0.18.3

- Waterfall live controls (black level, contrast, adaptive floor, FFT window selector)
- Display 180° flip toggle
- Image rejection investigation (reverted — not a firmware issue)

### v0.18.2

- Idle reboot resolved (WiFi SDIO RX streaming → mempool recycled buffer)
- Web UI freeze/reconnect fix
- BW from web (CW passband)

### v0.18.1

- Config backup/restore (settings + memory channels + ADIF log as INI file)
- Flasher clean-flash option
- Memory recall fix (CAT race condition → optimistic display + deferred writes)
- Fast Panadapter↔FT8 toggle crash fix

### v0.18.0

- FT8 decode rework: streaming STFT + dual-core + reply-on-immediate-slot
- dBm scale restored on spectrum
- Multiple field-reported fixes (memory freq, tap-to-tune reversal, band lockout)

### v0.15.x and Earlier

See [Full Version History](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md) for detailed notes on all earlier releases.

## Roadmap

### v1.0.0 (Stable)

Pending:

1. **Multi-hour FT8 TX soak** — confirm no duty-cycle crashes, no over-temp, clean shutdown
2. **LoTW (TQSL) upload** — certificate-based API (harder than QRZ/eQSL)

### Phase 6.3 (FPS Recovery)

- Native portrait LVGL rewrite (720×1280, all widgets transposed, canvas drawing ported)
- Avoids 90° software rotation (~50% FPS gain)
- Large effort; not yet started

### Future Additions

- FT4 mode (slot-timing plumbing; protocol already in ft8_lib)
- CW audio (shelved since v0.18.5 due to CPU contention; needs pipeline redesign)
- Offline maps (grid squares, distance visualization)
- Video tutorials & regional quick-start guides

## Download & Documentation

- **Source code:** [GitHub Repository](https://github.com/SteffenLav/qmx-panadapter)
- **Releases:** [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases)
- **User Guide:** [PDF](QMX-Panadapter-UserGuide-v0.19.3.pdf) or [Web](quick-start.md)
- **Build Guide:** [Build from Source](build/build.md)
- **Technical Details:** [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md)

---

**Have a question?** Check [Quick Start](quick-start.md) or [Troubleshooting](reference/troubleshooting.md).
