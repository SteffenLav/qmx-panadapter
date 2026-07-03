# Version History

All releases are available on [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases).

## Latest Release

**v0.19.5** — 2026-07-04

Interaction polish across the panadapter, plus two real bug fixes — including a crash on leaving FT8 mode.

- **FT8 mode-exit crash fixed**: leaving FT8 could occasionally reboot the Tab5. The dual-core decoder was tearing down its shared workspace before its second-CPU helper had finished; it now waits for the helper to fully stop first
- **WiFi on/off works live, and stops forgetting your password**: the on/off icon (now inside the WiFi setup window) applies immediately instead of only on the next reboot; the window also pre-fills your stored password so pressing Save no longer wipes it (which had left the radio unable to rejoin a secured network)
- **FT8/FT4 remembers its frequency** across reboots and panadapter↔FT8 switches (default 14.074 MHz) instead of inheriting whatever the panadapter was last on
- **Band picker shows all bands at once**: two side-by-side columns on radios with many bands (QMX+); a spurious "11m" entry some radios report but don't have is filtered out
- **Band-plan strip**: the position indicator is now a see-through framed window (drag it to tune, centre snaps to a whole kHz); the 6 m band now has band-plan segments; a tiny finger movement on lift no longer nudges the frequency
- **Point-to-tune steadier**: commits exactly where the cyan cursor settled, ignoring the small finger-jump on lift-off; the cyan guide line now runs cleanly through the waterfall (no trail) and stays glued to the spectrum line
- **Settings drawer**: sliders respond only when you grab the knob (a swipe over one scrolls the drawer instead), the whole knob is visible at the ends, the **Settings** title is always visible on open, and **WiFi setup** closes the drawer when opened
- **Memory recall in FT8/FT4**: memories not in the DiGi (data) mode are greyed out and can't be recalled (would knock the radio out of data mode); still editable
- **Antenna Tune** is now its own window (from the settings drawer) with a bigger title and a live SWR/power readout
- Full writeup in [Version History Document](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md)

### Installing v0.19.5

1. Use the one-click flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases)
2. Or follow [Build from Source](build/build.md)

### Upgrading from Earlier Versions

Your settings (callsign, grid, WiFi, memory channels) are preserved during a normal flash. If something seems stuck, use a **clean flash** to erase all settings:

1. Run the flasher
2. When prompted, type **E** for clean flash
3. Re-enter your settings on first boot

## Previous Releases

### v0.19.4

- FT4 usability — four stacked faults fixed: a capture buffer's FFT workspace in slow memory (every other slot decoded nothing), a mis-firing stuck-decoder watchdog wiping the list mid-QSO, an uncapped per-slot clock nudge, and slot-parity computed on FT8's grid in FT4
- QMX IQ-mode confirmation hardened (readback no longer fooled by the command echo)
- See-through frequency keypad; settings-drawer declutter (removed Snap-to-signal + FT8 sync lines, moved Band-plan region)

### v0.19.3

- QMX IQ-mode handshake retried automatically (up to 4 attempts), with a red on-screen banner if all attempts fail — a silent failure could leave a whole session without I/Q data (spectrum shifted, tunable across the full 48 kHz window)
- Band-plan strip: tracks zoom/pan live, shows the filter passband at band scale, drag-to-tune and tap-to-jump directly on the strip
- Memory channel drag-to-move; tap an empty slot to create; out-of-band frequencies rejected immediately
- Frequency entry popup: draggable, resizable (pinch/swipe), position remembered across reboots
- ADIF log viewer rebuilt: real column alignment, sticky header, Country and Mode columns, Sent/Rcvd split, zebra rows
- FT8/FT4 TX confirm dialog: up/down nudge buttons; FT4 countdown and title corrected for 7.5-second slots
- FT4 clock-sync fix (timing offset was using FT8's block geometry, ~3.3× wrong) and FT4 decode-quality fix (FT8's iteration cut no longer applied to FT4)
- QRZ/eQSL uploads more reliable (SD auto-archive paused during uploads); web UI stale-connection freeze capped at 5 s

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

- CW audio (shelved since v0.18.5 due to CPU contention; needs pipeline redesign)
- Offline maps (grid squares, distance visualization)
- Video tutorials & regional quick-start guides

## Download & Documentation

- **Source code:** [GitHub Repository](https://github.com/SteffenLav/qmx-panadapter)
- **Releases:** [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases)
- **User Guide:** [PDF](QMX-Panadapter-UserGuide-v0.19.5.pdf) or [Web](quick-start.md)
- **Build Guide:** [Build from Source](build/build.md)
- **Technical Details:** [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md)

---

**Have a question?** Check [Quick Start](quick-start.md) or [Troubleshooting](reference/troubleshooting.md).
