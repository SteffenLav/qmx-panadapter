# Version History

All releases are available on [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases).

## Latest Release

**v0.20.0** — 2026-07-10

**A major robustness release.** Most of this update is about the device *staying up* — the WiFi drop-out, the freeze on opening a window, and the radio-link hiccups that field reports kept hitting are now fixed or self-healing. Plus a decision to pause FT4, keep the web UI out of FT8's way, and a batch of interface, band, memory and battery improvements.

- **WiFi no longer dies until you reboot**: the long-standing "WiFi stops after a few minutes (FT8 and CAT keep working), only a power-cycle brings it back" fault is fixed. A low-level lock-up in the link to the WiFi co-processor now recovers automatically (it drops one packet, which is simply re-sent) instead of wedging forever. Verified on hardware
- **Opening a window no longer freezes the device**: the ADIF log / CQ editor / Filter / Sync-Time windows could occasionally hard-freeze the whole Tab5 (needing a power-cycle). Traced to the faint on-screen watermark forcing an expensive redraw; drawn a cheaper way now — looks identical, freeze gone
- **The radio-control (CAT) link rides out USB glitches** instead of going dead for the rest of the session on a brief hiccup
- **The web UI stays out of FT8's way**: switching to FT8/FT4 pauses the browser's live spectrum stream (it competed with FT8 for the radio link and CPU) and shows the log + upload controls instead — steadier decoding and a more stable WiFi link while operating digital modes
- **SD card handling reworked for stability**: the microSD card now runs on its own dedicated bus instead of sharing one with WiFi. The automatic copy-to-card backup is **currently disabled** (mounting the card squeezes memory enough to hurt FT8 decoding) — your log and settings are unaffected, they always live in the device's own storage
- **FT4 is temporarily switched off** this release — it was running the device out of memory and crashing. Fully reversible; FT8 is unaffected
- **FT8 decode timing fixed**: each 15-second slot is now anchored to the exact time boundary, so the start of every signal is no longer clipped
- **FT8 pile-up list**: stations that answer you mid-QSO are collected in a tappable "Pileup" list instead of vanishing; tap to work them, or ✕ to dismiss. Plus a "Skip TX1" option for quicker pounces
- **11 m / CB band** support for QMX+, and the band picker no longer confuses 10 m and 11 m
- **Interface**: smooth backlight fade-in at boot, a "turn on / reboot your QMX" prompt while waiting for the radio, and a memory-channel overhaul (example channels on first use, a one-time drag-and-delete tour, and a drag-to-wastebin delete)
- **Battery care**: optional charge limit (stop at a set %, default 80 %) and an accurate charge reading while charging
- **Web UI**: whole-band plan strip with a draggable window, a draggable spectrum/waterfall divider, screenshots that capture open pop-ups, and a nicer frequency keypad
- Full writeup in [Version History Document](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md)

### Installing v0.20.0

1. Use the one-click flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases)
2. Or follow [Build from Source](build/build.md)

### Upgrading from Earlier Versions

Your settings (callsign, grid, WiFi, memory channels) are preserved during a normal flash. If something seems stuck, use a **clean flash** to erase all settings:

1. Run the flasher
2. When prompted, type **E** for clean flash
3. Re-enter your settings on first boot

## Previous Releases

### v0.19.5

- AM mode + Antenna Tune for QMX 1_04+ firmware (invisible on stable 1_03_002)
- Fixed a crash on leaving FT8 mode; WiFi on/off now applies live and no longer wipes a saved password; FT8/FT4 remembers its own frequency
- Band picker shows all bands in two columns; band-plan strip is a see-through framed window with 6 m segments; steadier point-to-tune; settings-drawer slider/scroll fixes; DiGi-gated memory recall in FT8/FT4

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

1. **Re-enable FT4** — bring its memory use under control so it can be switched back on (disabled in v0.20.0)
2. **Multi-hour FT8 TX soak** — confirm no duty-cycle crashes, no over-temp, clean shutdown
3. **LoTW (TQSL) upload** — certificate-based API (harder than QRZ/eQSL)
4. **Re-enable SD auto-archive** — cut the mount's internal-memory cost (or gate it off during FT8) so it no longer starves the decoder

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
- **User Guide:** [PDF](QMX-Panadapter-UserGuide-v0.20.0.pdf) or [Web](quick-start.md)
- **Build Guide:** [Build from Source](build/build.md)
- **Technical Details:** [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md)

---

**Have a question?** Check [Quick Start](quick-start.md) or [Troubleshooting](reference/troubleshooting.md).
