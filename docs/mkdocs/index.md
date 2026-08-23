# QMX Panadapter

A standalone real-time panadapter — spectrum analyser and waterfall — for the [QRP Labs QMX/QMX+](https://www.qrp-labs.com/qmxp.html) HF transceiver.

Running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/tab5) (ESP32-P4 with a 5" 720×1280 touch display), the panadapter connects to the QMX as a USB host, decodes the I/Q in real time, and renders a touch-driven interface with tap-to-tune, pinch-zoom, onboard FT8/FT4 decoding and transmit, ADIF logging, and a matching browser web UI.

**By Steffen Lav (OZ1LAV)**

## What it does

Everything below is in the firmware **today**. Nothing needs a PC; only the items marked
*(needs WiFi)* need a network.

- **Spectrum & waterfall** — a 48 kHz window centred on the VFO at 30 Hz, adaptive
  per-bin noise floor, flat-spectrum mode, adjustable waterfall black level and contrast,
  selectable FFT window, a dBm-calibrated S-meter, and a colour band-plan strip that
  follows the VFO.
- **Touch tuning** — tap or drag to tune with mode-aware snapping, pinch-zoom (a real
  zoom-FFT, not a stretch), one-finger pan-and-retune, edge-swipe navigation between
  screens, and a frequency keypad.
- **QMX control** — frequency, mode (USB/LSB/CW/DiGi, plus AM on QMX firmware 1.04+),
  SSB filter width, CW passband, TX power and SWR, and QMX volume in decibels matching
  the radio's own display. Band presets with per-band frequency recall, and 32 memory
  channels holding any frequency and mode.
- **FT8 & FT4 receive** — continuous on-device decoding of both modes, in the same view
  as the panadapter. Callsign, country, signal report, slot-timing offset, audio tone,
  distance and bearing; include/exclude filters; band-aware worked-before exclusion; a
  pileup tracker; and the station you are working held at the top of the list.
- **FT8 & FT4 transmit** — tap a station to send the correct *next* message; call CQ from
  editable presets with an optional stop-after-N-calls limit; the full automatic exchange
  through to `73` and a log entry; a resend if your partner never heard your final; a
  polite hold for a station already working someone else; grey-listing; an optional
  unattended auto-answer robot; a drag-to-pick TX tone with a live occupancy strip, TX
  Hold and an EVEN/ODD time-window choice; and ARRL Field Day mode.
- **Logging & upload** — every QSO written to an ADIF log on the device, readable and
  editable on the Tab5 or in the browser, and uploaded *(needs WiFi)* to **QRZ Logbook**,
  **eQSL** and **ARRL LoTW** — LoTW QSOs signed on the device with your own certificate.
- **Live spots** *(needs WiFi)* — **POTA** activations, and optionally **RBN** CW skimmer
  spots and **DX cluster** spots, which are where phone activity comes from. Drawn on the
  trace where the station actually is, grey once you have worked them on that band, and one
  entry per station however many sources report it. Press and drag to pick one and lift to
  tune it *with the right mode*. See [Live Spots](guide/spots.md).
- **PSK Reporter** *(needs WiFi)* — the stations you decode are reported to the PSK
  Reporter map the way WSJT-X does. On by default, one checkbox to turn off, and never
  anything on the air.
- **Web UI** *(needs WiFi)* — the whole panadapter in a browser on the LAN: live spectrum
  and waterfall, click/drag/wheel tuning, band-mode-bandwidth-zoom control, live FT8 TX
  status with a **Call CQ** button, a sortable QSO log, config download and upload, a
  microSD file browser, screenshots, and the diagnostic log.
- **Built-in manual** — this whole guide is compiled into the firmware, so it is instant
  and needs no WiFi and no card. It opens at the chapter for the screen you are on,
  warning banners are tappable, and a **Need guidance?** panel takes your symptom in plain
  words. See [Getting Help](getting-help.md).
- **Time, on or offline** — SNTP *(needs WiFi)*, the Tab5's own supercap-backed RTC across
  power-off, the QMX's clock as an offline fallback, automatic GPS phase-lock if your QMX
  has one, a manual set-and-sync panel, and FT8 timing that self-corrects from the decoded
  band consensus.
- **microSD backup** — insert a card *before switching on* and your ADIF log, full config,
  LoTW certificate and key, and diagnostic log are mirrored automatically. Continuous with
  WiFi off (green SD dot); one complete backup per start-up with WiFi on (yellow dot).
- **Diagnostics** — an always-on log with nothing to enable: 5 MB in RAM, a rolling copy
  in flash that survives a power cut, and a full mirror to microSD. Downloadable from the
  browser or over USB serial.
- **Practice mode** — phantom stations that call and reply through the real decode
  pipeline, so you can rehearse a whole QSO with **no radio connected** — with a hard
  interlock that never keys an attached QMX.
- **Field-ready** — WiFi entirely optional, battery percentage and voltage with a charge
  limit, display sleep, a 180-degree flip for awkward mounting, config backup and restore
  as a text file, and optional snap-on keyboard and USB mouse support.

## Status

**v1.9.3 — a complete, self-contained FT8/FT4 station with no PC in the loop, a second
operating position in any browser, and the radio's own menus on the screen.** The
panadapter, FT8/FT4 receive and transmit, ADIF logging and all four logbook uploads —
QRZ, eQSL, ARRL LoTW and your own Cloudlog or Wavelog — are stable and in daily use.

**New in v1.9.3 — updating is one decision instead of a procedure.** The Tab5 now **fetches a new release quietly in the background** and asks you once, when it is ready, in a **window in the middle of the screen** with **Restart now** / **Later** — no long-press, and no cryptic line at the bottom trying to explain itself in twenty characters. The bottom bar breathes gently while an update waits for you and goes quiet once you have said "later". Switch the background download off under **Settings → Network → Download updates automatically** if you are on a metered connection; each update is about 3.3 MB, and nothing is ever installed without you asking. **The spectrum, waterfall and FT8 decoding now keep running while an update downloads** — before, everything stopped until it finished. **Audio is no longer dropped while a log upload is running**, which could previously cost you FT8 decodes during a QRZ, eQSL or LoTW upload. The **band-plan strip is much easier to hit** — its touch area now reaches up into the waterfall while it still looks the same. Plus **TXCQ ANY/EVEN/ODD on the web FT8 page** *(Randy N4OPI)* and **SSB tuning that snaps to 500 Hz** instead of 250 *(Dave KX3DX)*. Full detail in [docs/version-history.md](docs/version-history.md).

**New in v1.9.2 — a field-report release: five things fixed or added, three of them from Randy N4OPI.** A **stuck exchange that never logged** is fixed *(Roy KI0ER, working K7FD)*: when a caller's own first message to us was already a signal report, the QSO machine started itself in the wrong state and kept re-sending the same reply for over ten minutes while the far station sent RRR. **The SWR-protection fault is visible from the web page now** — before this, tripping it stopped transmission with no explanation on screen anywhere but the Tab5, and no way to clear it remotely *(Randy N4OPI)*. **"Who is hearing me" gets 15 min/30 min/6 h/24 h time windows and sortable columns**, and the FT8/FT4 decode list in the browser gets the same sortable columns with a "CQ callers on top" link back to the device's own ordering. **Working an older pileup caller from the web page now actually works** instead of refusing outright — it falls back to the same report-first reply the Tab5 has always used *(Randy N4OPI)*, and pileup entries now show their age on both screens. The Tab5's decode-list HRD column is now **AGE in seconds**, and how long a row survives before it drops off the list is **operator-tunable** (Filter modal, 30-90 s). **Simulation mode no longer leaves real stations flickering on screen** — entering it now clears the list immediately, and real decodes are suppressed for as long as it's on. Full detail in [docs/version-history.md](docs/version-history.md).

**New in v1.9.1 — a hotfix: updating from the device did not work over a real download, on any version, ever, until this fix.** v1.9.0 was meant to be the first release where the OTA offer could actually be used; testing it for real for the first time turned up two bugs. GitHub's own redirect response carries more header data than the firmware's HTTP client could receive, so every download failed instantly - this is why v1.9.0's offer downloaded nothing for anyone who tried it. Fixed, and a second bug then appeared: the same fix made each downloaded chunk larger, and the download loop never yielded control back to the rest of the firmware while processing it, which could crash and reboot the device right at completion. Both fixed and verified on hardware: a real download completed twice with zero crashes and zero audio interruption. **If you are on v1.8.9 or v1.9.0, this needs one cable flash - your device's own broken download code cannot fetch any release, including this one. After that one flash, OTA works normally.**

**New in v1.9.0 — the web page is fast again, the snap-on keyboard drives the whole app, and updating from the device can finally be seen working.** v1.8.9 added updating from the device, but the offer only appears when a *newer* release exists and at the time there was none — now there is. Hold the version at the bottom of the Tab5, or tap it in the web page; **and if you do not want to wait for the 30-minute check, hold it even while it says you are up to date** and it asks straight away. **The web page loads in under two seconds instead of about ninety** — it was being sent uncompressed, 263 KB of it, on a link shared with the live spectrum stream. **WiFi power saving is off**, which took requests that failed outright from 14.4% to 0.4% over 500 samples. And **the page was killing the very stream it displayed**: the web server evicts its least-recently-used connection, a connection's place in that queue is only refreshed when it *receives* something, and the spectrum stream only ever sends — the browser's own polling was enough to get it thrown out every few seconds. That is the "reconnecting" some of you have seen for a long time. **The snap-on keyboard** now drives the radio's menus, works in every window with buttons, scrolls the drawer and the manual, and carries **Ctrl shortcuts you can reassign yourself** from the web page *(Don N2VGU)*. **Five fixes from Randy N4OPI**: the TX power/SWR readout hidden behind the exchange status, the TX tone picker landing one or two slots off, "Busy: working …" and "QSO cancelled" never clearing, a worked station staying green, and **"Who is hearing me" quietly discarding reports** past the first 64. The decode list now shows the **country, spelled out**, in place of a grid the message already contained. Full detail in [docs/version-history.md](docs/version-history.md).

**New in v1.8.9 — the Tab5 updates itself, and a transmitter that could be left keyed no longer can.** Roy KI0ER's QMX transmitted continuously until he power-cycled it: a USB timeout mid-burst made every command fail, including the two that **stop** transmission, and although the link recovered two seconds later nothing ever re-sent them. The stop command is now retried, and if it still cannot get through it is handed to the radio-control task which keeps trying until the radio is demonstrably back in receive. **The version at the bottom of the screen now offers you a newer release** — touch it, hold for a second, let go, and it downloads in the background. It never restarts on its own, and nothing is fetched unless you ask. The web page says the same in the same words. **A crash now survives the reboot**, so an unexpected restart can finally be diagnosed from a diagnostic download, and **the microSD card keeps the log again while WiFi is on**. **The flasher download is 2.9 MB instead of 44 MB** *(Gyula HA3HZ)*. **FT8 can move off an occupied frequency mid-QSO** — to the nearest clear slot only, so a station with a narrow filter still hears you *(Roy KI0ER, Gyula HA3HZ)*. **Bandwidth stuck on a CW filter after switching to LSB**, **the out-of-band tuner missing from the browser** and **browser display stalls** are all fixed *(Samuel W7STF)*, and **spur suppression has been withdrawn** — it only ever worked at ×1 zoom. Full detail in [docs/version-history.md](docs/version-history.md).

**New in v1.8.8 — if the Tab5 ever restarts on you, the diagnostic download now says why.** Until now a crash left **nothing** on the device: the details went straight out of the serial port and were gone, so a diagnostic download contained everything except the one thing needed. The next boot now reports the previous crash — what happened, **which part of the firmware**, how far into the session, and where. If your Tab5 restarts unexpectedly, just send the diagnostic download; that is enough. **An FT8 reboot open since v1.3.0 is root-caused and fixed**: entering FT8 could start the decoder twice, and the second copy freed memory the first was still using. **FT8 was quietly discarding decodes** — 99 out of 54,142 measured — and a lost one looks exactly like the other station going quiet. **The bandwidth no longer stays on a CW filter after switching to LSB**, and **the out-of-band tuner now exists in the browser too** *(Samuel W7STF)*. **Browser stalls**: the message that tells a second browser "another browser took the live view" was malformed, so browsers hung up and grabbed the view straight back — a continuous tug-of-war whenever a phone or second tab was left open *(Samuel W7STF)*. And the **frequency readout can no longer stick** on an old value while the spectrum and radio are correct.

**New in v1.8.7 — the browser panadapter stops freezing, and your logs can go to your own Cloudlog.** The web display **hanging for seconds at a time** is fixed, and the cause was not what it looked like *(Samuel W7STF)*: measured over 9.6 hours, the browser session was being torn down **545 times**, each costing about **2.2 seconds** of frozen display while it reconnected. A partial WebSocket write was being reported as a complete one, which corrupted the stream and made the browser hang up — and the background feeds only made it more likely, which is why it worsened with every release. **Upload to a self-hosted Cloudlog or Wavelog** is new *(Mark G4MEM)*: plain `http://` is allowed when the server is on the same network as the Tab5, re-checked at every upload so it refuses from a field site. **Radio menus show the radio's own colours** instead of all white *(Samuel W7STF)*. A new **"Pick callers myself"** option lets you tap the hunter you want instead of the firmware answering the first one *(Eric K3FNB)*, a **Bluetooth mouse whose pointer jumped about** is fixed *(Kevin KW6E)*, and **entering FT8 can no longer reboot the device**. The **battery no longer reads 100% then 0% with no pack fitted** *(Randy N4OPI)*, **waking from the screensaver no longer acts on the tap** that woke it *(Randy N4OPI)*, and a **radio left receiving on VFO B is put back on A** and says so *(Markus DL8MBY)*.

**New in v1.8.6 — a same-day fix release, and the headline is that v1.8.5's browser interface was completely dead.** One broken text string stopped the whole page working: it drew its controls and then did nothing — no spectrum, no waterfall, no buttons, "disconnected" in the corner *(Randy N4OPI, Michael KZ4LY)*. The build now refuses to compile a page that does not parse, so this cannot ship again. **A crash that looked like a radio fault** is fixed too: an overnight soak aborted inside the USB driver, and because that reboot happens with the radio attached it left the QMX unable to reconnect for the rest of the night — the morning's report was "the QMX wedged", and the QMX was fine. **In CW the displayed frequency and tap-to-tune are corrected** — a signal on 7.060.000 showed 40 Hz high, from a stale calibration default plus the display rounding to whole analysis bins *(Roy KI0ER)*. And in **Radio menus** you can now see what you are typing past message 9, the help says to power-cycle the radio, and the **two-finger screen blank actually works** instead of about one try in ten *(Michael KZ4LY)*.

**New in v1.8.5 — the fixes people were already told about, and a batch more found the same evening.** **Radio menus** gained a **cursor**, a **BS key that deletes**, an **on-screen QWERTY** so a Tab5 without the snap-on keyboard can type a value at all, the **menu path on screen** when the radio has no second port, and **"Exit terminal" no longer re-opens the session** (Randy N4OPI, Michael KZ4LY). The **clock no longer claims `UTC(GPS)` on a radio with no GPS**, and in CW the display **follows the offset you actually set** instead of one read once at connect — which had left tap-to-tune about 30 Hz off (Roy KI0ER). The **web log viewer can correct a report**, and a report is **logged only if it was transmitted** (Gyula HA3HZ). A caller who **answers your CQ with a report instead of a grid** now gets `R` plus your report, the **station you are working is never hidden by a display filter**, **leaving Radio menus hands the radio back properly**, there is a **dated Today-only ADIF export**, and the **red transmitting banner no longer covers the text under it**.

**New in v1.8.4 — the QMX's own menus on the Tab5, and a batch of fixes that stop it doing things you did not ask for.** **Radio menus** puts the radio's own 80×24 menu system on the Tab5 and in the browser, running on its *second* USB serial port so the panadapter keeps decoding while you are in there — for a **QMX+ with no control panel it is the only way in** (Randy N4OPI, Michael KZ4LY). **Auto-answer now switches itself off** when you cancel a transmission, when you change band by any route, and at every startup, and it waits until it has heard both transmit windows before its first call (Roy KI0ER). A **transmit offset chosen during a QSO is used** instead of being refused whenever a burst happened to be on the air (Roy KI0ER). **Spur suppression offers the setting that works first** — measured, Erase takes the comb down about 78% against Subtract's 28% (Samuel W7STF). A **USB mouse is read from its own description**, fixing a pointer that flew sideways and barely moved vertically (Kevin KW6E). And a **WiFi hiccup can no longer restart the Tab5**.

**New in v1.8.2 — your radio's own spurs can be removed from the display, and a POTA clock that stopped being stolen.** If you have ever seen evenly spaced signals that never move and are still there with the antenna unplugged, those come from the QMX's own synthesizer — **spur suppression** finds them by nudging the dial 25 Hz and can subtract or erase them, **off by default** under Settings → Waterfall. A **QMX without GPS no longer overwrites an accurate clock** when you switch it on at a POTA site; the Tab5's own RTC wins and sets the radio instead. **RIT can be parked** with a long press and restored unchanged, and its offset is now printed beside its marker on the waterfall. The **band strip stays visible out of band**. And the panadapter no longer **switches your radio off** trying to recover a fault it cannot fix.

**New in v1.8.1 — the fixes from the first day of v1.8.0 in the field.** Most of the spectrum had stopped responding to **tap-to-tune**; the rule now is that if the mouse pointer is white, clicking tunes, and a finger behaves the same. **CW centre** covers the radio's real 500–950 Hz in 25 Hz steps and is read from the radio at connect. The **CW transmit offset** puts VFO B back where it found it. **RF gain and volume** agree between the Tab5 and the browser. Browser **spot labels** no longer swallow clicks meant for a signal kilohertz away. The **seconds are settable again** — hold the SS box and release on the minute, the only way to set the clock with no WiFi and no GPS. And the **RIT button can be hidden**.

**New in v1.8.0 — RIT you set by tapping, summit spots, and a browser that finally matches the Tab5.** Arm **RIT** and tap a caller to receive off your transmit frequency, with a marker showing where you are listening. **SOTA spots** join POTA, RBN and the DX cluster. **Fox/Hound** works DXpeditions from the hound side, simulation-verified so far. And the browser gained RIT, activation start/stop, the last Tab5-only settings, and a spectrum and waterfall drawn the way the Tab5 draws them.

⚠ **If you use the browser, two things are worth checking.** **SWR protection set from the browser was never saved** on any v1.7.x build — check it on the Tab5 under Settings. And **spot labels were stealing clicks** near their own callsign. Both fixed.

**In v1.7.0 — a mouse, the phone spots that were missing, and knowing who hears you.**
A **Bluetooth mouse** drives the Tab5 *while the QMX stays plugged in* — the case a USB
mouse can never serve, because the radio owns the only USB port. Pair it once and it
reconnects by itself; the wheel scrolls whatever is under the pointer. **DX cluster spots**
add the SSB activity RBN structurally cannot see, because skimmers are machines and no
machine recognises a callsign spoken into a microphone. **Activation mode** stamps every
contact with your park or summit, counts them against the threshold, and can export that
one reference on its own. **SWR protection** cuts a transmission short and latches off if
the antenna is wrong. And **who is hearing me** asks PSK Reporter which receivers copied
*your* call — the only way to tell a dead band from a transmit-side fault.

Every release, newest first, is on the [Releases](releases.md) page.

## Get Started

**New user?** Start with the [Quick Start](quick-start.md) guide — 10 minutes to on-air.

**Stuck, or not sure what something is called?** The Tab5 can help you itself — see [Getting Help](getting-help.md).

**Want the whole guide at once?** Download the [User Guide PDF](QMX-Panadapter-UserGuide-v1.9.3.pdf) — the whole user guide as one printable document.

**Builder?** Head to [Build from Source](build/build.md) for ESP-IDF setup and the complete module map.

## The QMX Connection

The QMX exposes two USB interfaces:

| Interface | Data |
|-----------|------|
| **UAC** (USB Audio Class) | I/Q stereo audio, 48 kHz, 24-bit |
| **CDC-ACM** (serial) | Kenwood-style CAT commands (FA, MD, FW, TX, RX, etc.) |

The Tab5 connects as a **USB host**, receiving both streams over a single USB-A to USB-C cable. No drivers needed on the Tab5 — they're built into ESP-IDF.

## Quick Links

- **[GitHub Repository](https://github.com/SteffenLav/qmx-panadapter)** — source code, releases, issue tracking
- **[QRP Labs QMX Manual](https://www.qrp-labs.com/qmx.html)** — radio specs and CAT reference
- **[M5Stack Tab5 Docs](https://docs.m5stack.com/en/core/tab5)** — hardware documentation

---

*QMX and QMX+ are products of [QRP Labs](https://www.qrp-labs.com). Tab5 is a product of [M5Stack](https://m5stack.com).*
