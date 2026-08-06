# Version History

All releases are available on [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases).

## Latest Release

**v1.5.0** — 2026-08-06

The manual answers questions now, instead of being a manual.

- **Help that opens where you are.** The settings drawer's **User Manual** button no longer lands on a contents page you have to search: it opens the chapter covering the screen you were on — the panadapter, FT8 **receive**, or FT8 **transmit** if a transmission is armed or running, because someone mid-transmission is asking a different question. See [Getting Help](getting-help.md).
- **"Need guidance?" — say it in your own words.** A second drawer button opens a short list of symptoms and questions written the way you would say them out loud: "My radio is not showing up", "Nothing appears in the decode list", "It never transmits", "How do I change what my CQ says?" Pick one and the manual opens at the answer — you never need to know what we call the thing that is wrong. The list scrolls, and it carries how-to questions as well as faults.
- **The device ranks; you choose.** Rows the Tab5 can see are happening right now are highlighted and moved to the top — no CAT link to the radio, IQ mode never confirmed, an empty decode list, WiFi switched on but not connected. It never navigates for you on a guess, however confident, and WiFi deliberately switched off for POTA is not reported as a fault. Rows are scoped to the screen you opened the panel from, so FT8 is never asked about the spectrum.
- **Warnings you can tap.** The red **"QMX IQ mode not confirmed"** banner is now a button that opens the section on that fault, and the **"Waiting for QMX"** message carries a small **Need help?** button — *help*, not *what is wrong*, because a radio that is off is often off on purpose.
- **The built-in manual stopped failing on well-used devices.** Some units reported "could not cache the page". The manual was being copied out of the firmware into internal storage just so it could be read back — on a device whose 1 MB of internal storage already holds your QSO log, LoTW certificate and key, and the diagnostic log, there was nothing left. The copy is gone: pages are read straight out of the firmware. No free space needed and nothing left to fail.
- **Missing characters in the manual, fixed properly.** Checked against every character the manual actually contains: 43 distinct non-ASCII characters, 37 of which now render. The waterfall and occupancy sketches were not drawing boxes — they were silently *vanishing*, leaving captioned blank space.
- **Call CQ from the browser** (Dennis WN4FLA). A CQ run that timed out or reached its call limit needed a walk back to the Tab5. There is now a **Call CQ** button under the web page's TX status banner. It confirms first, since it keys the radio, and it uses exactly what the Tab5 would: the active preset, the current TX tone (honouring TX Hold) and the TXCQ ANY/EVEN/ODD parity. See [Web UI](guide/web-ui.md#call-cq-from-the-browser).
- **The station you are working stays at the top of the decode list** (Don WB0LQW) — "there is no station that I am as interested in as the one I am trying to contact." Mid-exchange, their messages used to sort down-screen where they had to be hunted for, including a message to a third station, which is exactly when you most want to see it. It releases the moment the QSO completes.

**Not yet confirmed on the air:** Don's decode-list pinning (the mechanism is verified present and inert with no contact in progress, but the pin actually firing needs a real QSO — it logs when it engages and releases, so the diagnostic log will show it), the final key-down of the web **Call CQ** button, and how well the guidance panel's wording matches the way operators describe these faults. Reports on all three are welcome.

### Installing v1.5.0

1. Use the one-click flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases)
2. Or follow [Build from Source](build/build.md)

Your settings are preserved during a normal flash.

## Previous Releases

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

1. **An A–Z index in the built-in manual** — a touch-first term index, no typing on glass. v1.5.0's guidance panel deliberately says "Open the manual" rather than "Show all topics" because the index does not exist yet
2. **Web-UI audio streaming** — listen to the receiver in any browser on your LAN, demodulated on the Tab5. Already working in development; held for quality tuning and an overnight streaming soak
3. **CW page** — canned-message CW TX memories, then decoded-CW display
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
- **User Guide:** [PDF](QMX-Panadapter-UserGuide-v1.5.0.pdf) or [Web](quick-start.md)
- **Build Guide:** [Build from Source](build/build.md)
- **Technical Details:** [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md)

---

**Have a question?** Check [Quick Start](quick-start.md) or [Troubleshooting](reference/troubleshooting.md).
