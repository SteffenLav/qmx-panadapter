# Version History

All releases are available on [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases).

## Latest Release

**v1.2.0** — 2026-07-20

A built-in User Manual, right on the Tab5.

- **On-device User Manual.** Open the Settings drawer and tap **User Manual** to read this whole guide on the Tab5 itself — no phone or laptop. It's a native reader (not a web browser): headings, **bold**, lists, tables, code blocks and a two-column **Contents** page. On the Contents page, just press and slide your finger down the list — a highlight bar follows — and lift to open that page. **Back** steps through the pages you've viewed; **Exit** leaves the manual.
- **Reads the live docs.** It fetches the same documentation that builds this website, so it always matches what's published here.
- **Offline copies to microSD.** With a card in the slot, tap **Save offline** and the whole manual is copied to the card — then it reads with no internet at all, ideal for POTA/SOTA. It uses WiFi when you're online and the card when you're not.
- **Firmware update check.** The reader quietly checks GitHub for a newer release and shows a banner when one is available (it never flashes anything itself — that's always your choice).
- **FT8 pileup fix.** A station you'd already worked — including one that answers late, a few cycles after a time-out — no longer stays stuck in the pileup list. *(Thanks to Dirk DK7CVD.)*

### Installing v1.2.0

1. Use the one-click flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases)
2. Or follow [Build from Source](build/build.md)

Your settings are preserved during a normal flash.

---

## Previous Releases

**v1.1.0** — 2026-07-19 — A years-old FT8 decode mystery solved: the panadapter used to hear 60+ stations in its first slots then collapse to a fraction; the cause was ~200–350 ms of QMX audio lost at the USB wire every slot (invisible to every counter). Fixed — full decode rate every slot now. Plus the microSD card promoted to a full grab-and-go station backup (QSO log + settings + LoTW cert/key), automatic GPS time sync (~10 ms, no toggle), a band-plan drag from the bottom bar, and an ADIF-viewer crash fix.

**v1.0.1** — 2026-07-17 — Point fix: when you answer a CQ, the report you send back is your own measurement of their signal, not an echo of theirs. (Reported by Steve N0SZ, Jonathan KN6LFB.)

### v1.0.0 — 2026-07-16

**The 1.0.** The beta label is gone: the QMX Panadapter is a complete, self-contained FT8/FT4 station — receive, transmit, auto-QSO, logging, and upload to **all three major logbooks** — with no PC anywhere in the loop.

- **LoTW upload — the final piece.** QSOs are signed on the device with your own ARRL callsign certificate and uploaded directly to Logbook of the World. One-time guided setup from the web page (step-by-step TQSL certificate export, then import — your certificate passphrase never leaves your browser). Live-verified against lotw.arrl.org. Certificate renewal: Ctrl-click the LoTW button
- **The FT8 "everything sent twice" behaviour is fixed.** Replies used to miss their own slot by ~2 seconds and go out again next cycle, doubling QSO duration; the transmitter now holds ~2 seconds for the fresh reply and fires it on the same slot. Field-verified on air
- **Nonstandard callsigns work now**: special/compound calls (special-event stations, `PJ4/...`) no longer show as `<...>` once heard in full, and their answers to your CQ are recognized
- **Correct received reports in the log**: the partner's `R-06`-style roger is their real measurement of your signal and is now logged as such — no more 599 placeholders from CQ runs. (Old 599 rows can be deleted — see next)
- **QSO log viewer grows up**: a Today/All filter with a **POTA activation counter** (title turns green at 10 QSOs today), and **single-record delete** — long-press a row, drag to the right line, release, confirm. Deletions keep all three logbook upload positions consistent
- **Broken QSOs resume**: if a station fades mid-exchange and comes back within a few minutes, the exchange continues where it stopped (automatically, or by tapping their row) instead of restarting from scratch
- **Steadier decode list during CQ runs**: stations you can't currently hear (they transmit when you do) no longer vanish mid-run, and the list keeps entries for 2 minutes instead of 1
- **Display sleep**: set an idle timeout and the screen turns off while everything keeps running (FT8, radio link, web UI). Tap to wake — the wake tap can't press anything. Two-finger double-tap blanks immediately. A real battery win for unattended and web-only use
- **Reorganized settings drawer**: setup items (WiFi, callsign, band-plan region, brightness, battery care, display sleep) grouped at the top, display-tuning controls below
- **Web page polish**: the bottom bar's many buttons are now three tidy menus (QSO Logs / Files / Miscellaneous); a congested WiFi link can no longer freeze the page for seconds
- **A rare crash fixed**: a momentary USB glitch on the radio-control link could reboot the whole device; it now just retries
- Full writeup in [Version History Document](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md)

### v0.21.0

- **FT4 returned** — the panadapter no longer redraws itself behind the FT8/FT4 screen, freeing the processor headroom FT4's 7.5-second cadence needs; verified end-to-end (RX + TX)
- Fixed a full-screen display flash (mostly in FT4); QRZ upload no longer gets stuck on already-logged contacts; reset settings or Wi-Fi from the web page; QMX VOX switched off automatically at link time

### v0.20.1 / v0.20.0

- **Robustness release.** WiFi "dies after a few minutes" fault now self-heals instead of needing a reboot; opening a window no longer freezes the device; the radio-control (CAT) link rides out USB glitches; the web UI pauses its stream while FT8 runs
- FT8 decode timing anchored to the exact slot boundary; FT8 pile-up list + "Skip TX1" quick pounce; 11 m/CB band; memory-channel overhaul; battery-care charge limit; web-UI whole-band plan strip, draggable split, better screenshots and keypad
- microSD now on its own bus (SD auto-archive still off — it squeezed memory needed by FT8)
- v0.20.1 hot-fix: fixed a reboot on every pounce (a scratch table overflowed a small task stack; moved to PSRAM)
- *(FT4 was switched off across v0.20.x for the memory pressure now resolved in v0.21.0.)*

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

### Next up (post-v1.0.0)

1. **Web-UI audio streaming** — listen to the receiver in any browser on your LAN, demodulated on the Tab5. Already working in development; held for quality tuning and an overnight streaming soak
2. **CW page** — canned-message CW TX memories, then decoded-CW display
3. **Re-enable SD auto-archive** — cut the mount's internal-memory cost (or gate it off during FT8) so it no longer starves the decoder

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
- **User Guide:** [PDF](QMX-Panadapter-UserGuide-v1.1.0.pdf) or [Web](quick-start.md)
- **Build Guide:** [Build from Source](build/build.md)
- **Technical Details:** [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md)

---

**Have a question?** Check [Quick Start](quick-start.md) or [Troubleshooting](reference/troubleshooting.md).
