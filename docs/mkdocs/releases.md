# Version History

All releases are available on [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases).

## Latest Release

**v1.7.2** — 2026-08-10 — bug-fix patch

**A Bluetooth mouse could stop reconnecting until you restarted the Tab5.**

- If a reconnection attempt failed part-way, the Tab5 stopped trying — the pointer never came back, even with the mouse awake and right beside it, and only a restart brought it back. It happens routinely, because the mouse powers itself down after about half a minute to save its battery. Verified over 14 sleep/wake cycles: fourteen disconnects, fourteen reconnects, none missed.

Nothing else changed. **If you do not use a Bluetooth mouse there is no reason to install this.**

## Previous Releases

**v1.7.1** — 2026-08-10 — bug-fix patch

**Tried a Bluetooth mouse on v1.7.0? Install this so your WiFi doesn't go flaky.**

- **The Bluetooth mouse was making WiFi unstable.** Switching the mouse on could leave the web UI flapping between Connected and Disconnected, with the link dropping and recovering for as long as it stayed on. The two look unrelated, which is why it took days to find: WiFi and Bluetooth share one link to the wireless co-processor, and the Tab5 was listening for *every* Bluetooth device in the building, continuously — thousands of advertisements a minute in a busy room, all of it crowding WiFi off the same pipe. It now listens in short bursts, and once your mouse is paired the co-processor ignores everything else, so that traffic never reaches the Tab5 at all. On the bench the underlying link errors went from about five a minute to none, with the mouse still connecting by itself. **If you have not used a Bluetooth mouse, nothing changes for you.**
- **The settings drawer would not close.** The swipe only worked if your finger started on bare background, so it felt random. **Tap anywhere outside the drawer.** *(Michael KZ4LY)*
- **CW transmit offset by the hertz** — **−50 / −10 / +10 / +50** buttons under the slider, which covers 2000 Hz in two-pixel steps. Range deliberately unchanged. *(Michael KZ4LY)*
- **DX cluster spots stop vanishing.** A cluster with nobody typing was treated as a dead connection and dropped about every 70 seconds, losing its held spots. Quiet is normal on a cluster.
- **The browser said GPS with no radio attached** — the clock fault Don N2VGU reported, fixed on the Tab5 in v1.7.0 but missed in the web page.
- **A failed WiFi scan says why**, and one that never returns no longer sits on "Scanning..." forever.

**v1.7.0** — 2026-08-09

A mouse, the phone spots that were missing, and knowing who hears you.

- **A Bluetooth mouse, with the radio still plugged in.** A USB mouse cannot do this: the QMX occupies the Tab5's only USB port, and sharing it through a hub does not work on this hardware. A Bluetooth mouse never touches that port. Pair it once — it reconnects by itself afterwards, across reboots and updates — and you get a pointer, a left click, and a wheel that scrolls whatever is under it. For anyone whose hands are cold or unsteady, every control becomes a click instead of a precise tap on glass. A symbol in the bottom bar shows off, scanning or connected. Off by default; see [Settings](guide/settings.md).
- **DX cluster spots — the phone activity RBN cannot see.** RBN is automated skimmers, and no machine recognises a callsign spoken into a microphone, so every SSB station on the band was invisible to it. A cluster is people typing. Mode is worked out from the spotter's comment or your band plan, park and summit references are picked out of the text, and skimmer spots relayed onto the cluster are dropped so they cannot double what RBN already shows. See [Live Spots](guide/spots.md).
- **Activation mode for POTA and SOTA.** Start an activation and every logged contact carries the reference automatically, with the count shown against the threshold — ten for POTA, four for SOTA — read from the log itself so it survives a reboot. Chases are tagged too, from spots already on screen. The ADIF download can be limited to one reference, so you upload that park's log rather than your whole file. Stopping is as prominent as starting, because the mistake that actually happens is driving home with it still on.
- **SWR protection while transmitting.** The QMX reports SWR over the control link; above your limit — 3.0:1 by default — the transmission is cut short and the transmitter latched off until you clear it. An FT8 burst is nearly thirteen seconds of continuous key-down, so a disconnected or wrong-band antenna has real time to do damage.
- **Who is hearing me.** Asks PSK Reporter which receivers have copied *your* callsign, listing distance, bearing and the report they gave you. The valuable case is the mismatch: stations you can hear that cannot hear you is a transmit-side fault, and from the receiving side alone that looks exactly like a dead band.
- **Spots appear once.** An activator spotted on POTA *and* heard by RBN was drawn twice, in two colours, almost on top of each other; RBN also doubled itself where two skimmers rounded the same signal differently. One station is now one entry, and the RBN sighting is folded in as corroboration — meaning a receiver actually copied them just now, rather than a self-spot typed an hour ago.
- **Fixes:** the clock claimed GPS time with no radio attached (Don N2VGU) — the Tab5 has no GPS of its own, and it was trusting a remembered verdict; each spot source checkbox is now genuinely its own source, where switching off POTA used to blank the whole lane and leave its spots behind; a tune started from the web UI now says so on the Tab5; and Antenna Tune no longer cancels itself after a second while leaving the radio keyed.

**Not yet confirmed on the air:** SWR protection has never seen a real mismatch, and the DX cluster lane has been verified as a feed but not watched drawing on a busy band. Reports welcome.

### Installing v1.7.0

1. Use the one-click flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases)
2. Or follow [Build from Source](build/build.md)

Your settings are preserved during a normal flash.

**v1.6.0** — 2026-08-09 — the browser became a second operating position: reply to a station, pick your TX tone, edit settings and memories and read the manual from any browser; `qmx.local`; CW transmit offset, RF gain and Release radio; self-recovering audio; the manual gained an A-Z index and drawn diagrams; the settings drawer was grouped with a Basic/Expert toggle.

**v1.5.0** — 2026-08-06 — Context-sensitive help: the **User Manual** button opens the chapter for the screen you are on, warning banners are tappable, and a **Need guidance?** panel lists symptoms in plain words with the ones the device can see highlighted. Plus Call CQ from the browser (Dennis WN4FLA) and the station you are working held at the top of the decode list (Don WB0LQW).

**v1.4.0** — 2026-08-05 — **Live spots on the spectrum** (POTA, with RBN as an opt-in second source): callsigns drawn on the trace at the frequency the station is actually using, **grey when you have already worked them on that band**, press-and-drag to pick one and lift to tune it *with the right mode*; spots fade with age and go after 30 minutes, and corner counts take you to the ones just off-screen. Behind that, three long-standing causes of instability root-caused rather than patched around: **all internet uploads were failing** (QRZ, eQSL, LoTW and the update check — a hardware crypto engine starved of memory), **52 KB of internal memory was being held by the firmware's own tables** (low-water mark 0 KB → 32 KB — one cause covering the SD remount failures, USB not re-opening after a radio power-cycle, and reboots after an hour of FT8), and **power-cycling the QMX could freeze the Tab5** on the dead USB connection. Plus **WiFi remembers up to six networks** and moves between them itself (Roy KI0ER), four of Roy KI0ER's five FT8 findings fixed, and the **QRZ / eQSL / LoTW setup no longer hidden** until you have logged a contact (Brian WA6JFK).

**v1.3.6** — 2026-08-03 — A pure fixes release, two of them closing problems as old as the project: the **"restart the QMX again and again" USB mystery solved** as two separate bugs (the Tab5-side one fixed with self-healing; the QMX-side one detected and explained on screen, reported to QRP Labs), a **crash on radio power-on** fixed (Dennis WN4FLA), **WiFi Scan working away from home**, **live TX status on the web page**, and the **"Diag(saved)" download delivering the crash log** with an SD card inserted.

**v1.3.5** — 2026-07-31 — Don WB0LQW's three requests, Roy KI0ER's bug report and the drawer touch fix: **CQ stops calling after a limit you set** (long-press Call CQ → **CQ stop**, never/1-5/10, live "call 2 of 4" counter, one extra listening slot after the final call), the **QSO log opens in your browser** (QSO Logs → View / edit log — sortable columns, per-row delete, type-DELETE Delete-all) with **Delete all on the Tab5 too** (two-tap, "ALL 34?"), an escape for the **busy-station hold** (TAP TO CANCEL), the **settings drawer stops stealing your finger** mid-drag, **QMX volume verified identical to the radio's own LCD** and capped at 50 dB (Randy N4OPI), and a latent bug fixed before it bit anyone: clearing the log would have silently disabled all future QRZ/eQSL/LoTW uploads.

**v1.3.4** — 2026-07-29 — The same-day field reports on v1.3.3, almost all from Roy KI0ER: a partner who never decoded your final now gets it **re-sent** (up to three times in four minutes) instead of the Tab5 moving on without ever making their log, finishing by hand no longer writes **duplicate log entries**, the fabricated `599` **signal reports nobody sent** are gone from the ADIF log, and the **occupancy map is filtered by time window** — a tone busy only in the opposite slot no longer reads as busy for you. The **TX frequency became a permanent button** on the FT8 screen with a **live mini occupancy strip** under the slot countdown, **TX Hold** (WSJT-X's "Hold Tx Freq") pins your tone, the parity choice is one cycling `TXCQ ANY / EVEN / ODD` button, and WiFi strength in the bottom bar is a **fan icon** with the freed width going to the network name.

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
3. **Binaural CW audio** — a stereo sound stage so two stations a few tens of hertz apart land in different places in your head (asked for by Roy KI0ER; shaped by Don N2VGU and Michael KZ4LY, whose point that the stage **width** should be a setting rather than a fixed angle is now the plan). Waits on the same audio-output rework as the CW page
4. **Live microSD mirroring while WiFi is up** — continuous mirroring currently only runs with WiFi off; v1.3.2 made that explicit and reliable (one complete backup per start-up with WiFi on). This was believed to be bus contention between the card and the WiFi co-processor. v1.4.0 found that a large part of it was actually a memory shortage — the pool the card needs to mount had been squeezed to almost nothing, and now has room again — so this may already behave better than documented. Needs a retest before the behaviour is changed

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
- **User Guide:** [PDF](QMX-Panadapter-UserGuide-v1.7.2.pdf) or [Web](quick-start.md)
- **Build Guide:** [Build from Source](build/build.md)
- **Technical Details:** [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md)

---

**Have a question?** Check [Quick Start](quick-start.md) or [Troubleshooting](reference/troubleshooting.md).
