# Version History

All releases are available on [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases).

## Latest Release

**v1.3.4** — 2026-07-29

v1.3.3 went out in the morning and the reports came back the same day. This release is those reports, plus the TX frequency control moved out of a corner and onto the main screen.

- **A QSO is not over just because you said 73** (Roy KI0ER). Roy's partner never decoded his closing `R73`, so he kept sending his report, waiting for it — while the Tab5, which had already logged the QSO, moved on to the next CQ. Roy never made it into his log. Now, if the station just worked comes back with a report rather than `RR73`/`73`/`RRR`, the final is **sent again**, up to three times within four minutes, and that check runs *before* anything can start a new contact. Taking over by hand also no longer produces **duplicate log entries** — the same callsign on the same band inside ten minutes is recognised as the same contact.
- **Your log no longer contains signal reports nobody sent** (Roy KI0ER, who found `599` in his received-report column and asked whether stations really were sending it). They were not: when a QSO finished without a numeric report ever being exchanged, the field was filled in with `599`. Harmless in a CW or SSB log; in an FT8 log it is a **fabricated measurement**, and it was going to QRZ, eQSL and LoTW as though measured. Unknown reports are now left out entirely, and the log viewer shows a dash. Records already written are not touched.
- **The occupancy map answers the right question now** (Roy KI0ER). It counted every decoded station regardless of which time window it was heard in, so a tone busy only in the *opposite* window read as busy for you — on a crowded band, most of the strip — and the automatic clear-slot picker made the same mistake. Occupancy is now filtered by slot parity whenever your own transmit window is known; with `TXCQ ANY` and nothing armed it still shows both windows combined, and the free-slot line names the window when it knows it.
- **The QMX volume slider is usable — and confirmed working on a real radio** (Randy N4OPI, the first person to try it against one). The command format, the read-back and the live dB on the LCD all work. But everything usable sat inside the first tenth of the slider's 0–199 dB travel, so it now tops out at **40 dB** — double the highest useful value reported — for five times the resolution. Past 40 dB on the radio's own knob the slider pins at the end while the number still shows the rig's true dB.
- **The TX frequency is a control on the main screen.** It was a small chip that only appeared while a CQ or QSO was running — hidden at exactly the moment you were choosing where to transmit. It is now a permanent button in the FT8 left pane, always showing a number, with a **live mini occupancy strip** under the slot countdown beside it. New **TX Hold** (WSJT-X's "Hold Tx Freq") pins your tone for every CQ and reply instead of taking the nearest clear slot, and both the tone and the hold setting survive a power cycle. The parity preference is one cycling `TXCQ ANY / EVEN / ODD` button instead of two, `Active: N` is gone (the wrapping status text took its line), and in the picker your tone is now **white** and your partner's **pink**.
- **WiFi strength is an icon, not a number.** The `-NN dBm` figure in the bottom bar is replaced by a **fan icon** — dot above 25 %, plus one bow above 50 %, plus a second above 80 % — and the width it freed goes to the network name, which is far more often too long to fit. The IP address is pinned to the right edge and the centred UTC clock never moves.

### Installing v1.3.4

1. Use the one-click flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases)
2. Or follow [Build from Source](build/build.md)

Your settings are preserved during a normal flash.

## Previous Releases

**v1.3.3** — 2026-07-29 — Your TX audio frequency became visible and adjustable for the first time: a tone picker with a **live occupancy strip** across the 200-2800 Hz window, drag-to-pick, ±50 Hz, **Find clear slot**, changeable mid-QSO but never mid-burst (Roy KI0ER). The Tab5 also **stopped calling stations already working someone else** — which additionally stopped a merely-popular station being grey-listed for it — an incoming **`RRR`** began closing a QSO like `RR73` (it used to deadlock), a **QMX volume** slider in dB matching the radio's own LCD arrived for control-panel-less QMX+ builds (Randy N4OPI), and **LoTW uploads gained US state and county**, whose absence had been costing US operators their Worked All States and county credit (found via Paul N8HM's CardSat).

**v1.3.2** — 2026-07-26 — Grid squares are logged again (almost every QSO was missing `GRIDSQUARE` — John W5JSS), **PSK Reporter spotting** on by default (**since confirmed working**: "QMX Panadapter v1.3.2" is listed under "Software in use" at pskreporter.info with spots from six stations — note the 5½-minute delay before the first report, and that PSK Reporter never acknowledges anything, so that page is the only place to check), the **User Manual built into the firmware** so it needs no WiFi and no SD card and the "Save offline" button is gone, an explicit **microSD backup contract** (continuous with WiFi off and a green dot; one complete backup per start-up with WiFi on and a yellow dot — insert the card *before* switching on), **grey-listing** for stations that never answer (opt-in), and **pileup replies** using the intelligent-Transmit laddering (both Roy KI0ER).

**v1.3.1** — 2026-07-24 — Two decode-list columns requested by Roy KI0ER (**DT**, the station's slot-timing offset, and **HZ**, its audio tone, with the country column compacted to a 3-letter code), a boot-diagnostics fix so ST7121 units report the hardware actually detected (Paul VE3PIK), and UI alignment polish (QMX prompt placement, FT8 left-pane grid, settings-drawer dropdowns and sliders).

**v1.3.0** — 2026-07-23

Smarter manual FT8 operation — built around field feedback from **Roy KI0ER** — plus a rebuilt practice simulator, USB mouse support, and a web file browser for the microSD card.

- **Intelligent Transmit.** Tapping a decoded station's **Transmit** now sends the correct *next* message for that exchange — their CQ gets your grid (or your report with Skip-TX1 on), their grid gets your report, their report gets your roger, and so on — the same behaviour as a WSJT-X double-click. You can run a whole QSO by hand, one tap per step, and it **logs to ADIF** like any other contact when you send the closing RR73/73.
- **Faster replies.** The mid-slot reply window is wider and hand-armed exchanges now land on the beat instead of a cycle late. A subtle bug that made every exchange step transmit twice in some cases is fixed.
- **Fast pounce (early decode) — new toggle, on by default.** Decodes appear *before* the slot boundary (the way WSJT-X works), so answering a fresh CQ can transmit in the very next slot instead of waiting 30 s. ⚠️ *Honest note: this specific feature hasn't been verified on a live band yet (no antenna at the development QTH until mid-August). The known trade-off is that a station transmitting late in the slot can occasionally be missed. If your decode counts drop with it on, switch it off in the FT8 settings drawer — and please report your before/after numbers on the groups.io thread; that's exactly the field data we need.*
- **Pileup no longer hides the log.** While callers are waiting the ADIF-log button reads "Pileup" — **holding it now always opens the ADIF log** (a one-time hint teaches the gesture). **Auto-work pileup** also starts draining immediately when you enable it with callers already waiting, and worked-before stations only vanish from the pileup when you've actually checked "Exclude worked-before".
- **Practice simulator, rebuilt.** FT8 Simulation Mode now needs **no radio at all** — six phantom stations (US + DX) call CQ, four of them answer your CQ at once (a real pileup to practice on), and they're patient like real operators: each repeats its message up to four times before giving up. They answer manual step-by-step Transmit, Auto Pounce, and CQ-runs alike. When you're done, a **"Del N test"** button in the ADIF viewer (only visible while practice contacts exist) wipes them from the log in two taps.
- **USB mouse.** Plug a mouse into the USB-A port — a cursor appears and clicks drive everything. *(Limitation: the mouse and QMX can't share the port, and a USB hub can't bridge them on this hardware — so it's for setup, log review and manual reading with the radio unplugged.)*
- **microSD file browser.** **Files → SD Files** in the web page opens a browser for the card — download logs and backups, upload, delete — without pulling the card out.
- **User Manual on a fresh boot** now waits for WiFi instead of asking for a reboot.

**v1.2.0** — 2026-07-20 — A built-in **User Manual on the Tab5 itself** (Settings drawer → User Manual): native markdown reader with a drag-to-pick two-column Contents page, offline copies to microSD (**Save offline**), and a quiet GitHub firmware-update check. *(The Save-offline mechanism was superseded in v1.3.2 — the manual now ships inside the firmware.)* Plus the FT8 pileup fix: a worked (or late-answering) station no longer lingers in the pileup. *(Thanks to Dirk DK7CVD.)*

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
3. **Live microSD mirroring while WiFi is up** — the card and the WiFi co-processor share a DMA controller, so continuous mirroring only works with WiFi off. v1.3.2 made the behaviour explicit and reliable (one complete backup per start-up with WiFi on); making it continuous in both cases needs the underlying bus contention solved

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
- **User Guide:** [PDF](QMX-Panadapter-UserGuide-v1.3.4.pdf) or [Web](quick-start.md)
- **Build Guide:** [Build from Source](build/build.md)
- **Technical Details:** [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md)

---

**Have a question?** Check [Quick Start](quick-start.md) or [Troubleshooting](reference/troubleshooting.md).
