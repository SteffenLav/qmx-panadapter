# QMX Panadapter

A standalone real-time panadapter — spectrum analyser and waterfall — for the [QRP Labs QMX/QMX+](https://www.qrp-labs.com/qmxp.html) HF transceiver.

Running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/tab5) (ESP32-P4 with a 5" 720×1280 touch display), the panadapter connects to the QMX as a USB host, decodes the I/Q in real time, and renders a touch-driven interface with tap-to-tune, pinch-zoom, onboard FT8/FT4 decoding and transmit, ADIF logging, and a matching browser web UI.

**By Steffen Lav (OZ1LAV)**

## Features

- **Spectrum & Waterfall** — Real-time 4800-sample/s FFT with adaptive noise floor, spectrum curve, and colour waterfall
- **Touch Interface** — Tap-to-tune, pinch-zoom, one-finger pan, edge-swipe navigation
- **FT8 & FT4 Decoding** — Onboard decode for both modes with configurable filtering, worked-before exclusion, and priority ranking
- **FT8 & FT4 Transmit** — Reply to CQ, run CQ, auto-QSO (robot mode), full exchange, ARRL Field Day mode
- **ADIF Logging** — Every QSO logged locally, with QRZ Logbook, eQSL, **and ARRL LoTW** upload (LoTW QSOs are signed on the device with your own callsign certificate)
- **microSD Auto-Archive** — Insert a card *before switching on* and your ADIF log, config, LoTW certificate and diagnostic log are mirrored automatically. Continuous with WiFi off (green SD dot); one complete backup per start-up with WiFi on (dot turns yellow)
- **PSK Reporter** — Reception reports uploaded automatically so you appear on the PSK Reporter map, as WSJT-X does. On by default; one checkbox to turn off
- **Built-in User Manual** — This entire guide ships inside the firmware: Settings drawer → User Manual, working instantly with no WiFi and no SD card
- **Web UI** — Remote spectrum, waterfall, and control from any browser on the LAN
- **Offline Ready** — Tab5 RTC + SNTP sync; FT8 operates without WiFi (POTA/portable)
- **Multi-frequency Memory** — 32 memory channels (4×8 grid), any frequency/mode, not tied to a band
- **Optional Keyboard** — M5Stack Tab5 snap-on keyboard (70-key) for text entry and one-hand navigation

## Status

**v1.4.0 — a complete, self-contained FT8/FT4 station with no PC in the loop.**

- **v1.4.0 — live spots on the spectrum, and three instability causes root-caused**: **POTA** (and optionally **RBN**) activations drawn straight onto the trace where the station is operating — grey when you have already worked them on that band, press-and-drag to pick one and lift to tune it **with the right mode**. Behind that: **all internet uploads were failing** (QRZ/eQSL/LoTW and the update check — a hardware crypto engine starved of memory), **52 KB of internal memory was being held by our own tables** (low-water mark 0 KB → 32 KB, which explains the SD remount, USB-after-power-cycle and after-an-hour-reboot faults as ONE cause), and **power-cycling the QMX could freeze the Tab5** by pinning a CPU core on the dead connection. Plus **WiFi remembers up to six networks** and moves between them itself, and four of Roy KI0ER's five FT8 findings are fixed
- **v1.3.6 — the USB reconnect saga solved, a field crash fixed, WiFi scan that works away from home** (a pure fixes release): the years-old "restart the QMX again and again" USB mystery turned out to be **two separate bugs** — the Tab5-side one (a QMX powered off mid-stream jammed the USB port until reboot) is **fixed with self-healing on top** (QMX off → on reconnects in a second, hands off), and the QMX-side one (the radio answers enumeration incorrectly after some Tab5 restarts — reported to QRP Labs) is now **detected and explained on screen** ("QMX USB is stuck - power-cycle the QMX"). A **crash on radio power-on in FT8 mode is fixed** (Dennis WN4FLA — reproduced on the bench with his exact steps, root-caused, torture-tested). **WiFi Scan works when your stored network is out of range** (hotels/POTA — it used to always say "No networks found"). The **web page shows live TX status** in FT8/FT4 mode with a red transmitting banner, the "call 2 of 4" counter and a red dot in the browser tab title. And the **"Diag(saved)" download actually delivers the crash log** with an SD card inserted
- **v1.3.5 — managing the log, pacing the CQ, and a way out of the wait** (Don WB0LQW's three requests plus Roy KI0ER's bug report): **CQ auto-stop** — long-press Call CQ and a **CQ stop** button sets a limit (never / 1–5 / 10 calls); the TX status counts "call 2 of 4" live, and after the last unanswered call the Tab5 listens one more slot (an answer still starts the QSO) before going idle. The **QSO log opens in your browser** — QSO Logs → **View / edit log** is a sortable table (click a column header) with per-record delete and a type-`DELETE` Delete-all — and the Tab5's own ADIF viewer gets a two-tap **Delete all** as well. The **busy-station hold can be cancelled** — the polite wait for a station working someone else now shows **TAP TO CANCEL** instead of locking you in. Drawer **sliders and checkboxes stay grabbed** instead of the drawer scrolling out from under your finger. The QMX volume tops at **50 dB** and the dB figure is **verified identical to the radio's own LCD** (Randy N4OPI). Also fixed before anyone hit it: clearing the log now resets the QRZ/eQSL/LoTW upload positions
- **v1.3.4 — finishing the QSO properly** (the same-day field reports on v1.3.3): a partner who never decoded your closing `73` keeps asking for it, and the Tab5 now **sends it again** instead of moving on — and taking over by hand no longer writes a **duplicate log entry**. An invented `599` signal report is no longer put in your log when none was exchanged. The occupancy map is **filtered by time window**, so a slot busy only in the other window no longer reads as busy for you. The **QMX volume slider is confirmed working on a real radio** (Randy N4OPI). The **TX frequency is a permanent button on the main screen** with a live mini occupancy strip beside it, plus **TX Hold** (WSJT-X's "Hold Tx Freq"); `Active: N` is gone and the parity buttons became one `TXCQ ANY / EVEN / ODD` cycle. WiFi strength in the bottom bar is now a **fan icon** rather than a dBm figure, giving the width to the network name
- **v1.3.3 — your TX frequency, on screen and under your control** (almost all of it Roy KI0ER's field feedback): the transmit tone is shown on the TX status line, and a **TX nnnn Hz** button opens a picker with a **live occupancy strip** across the whole audio window — drag along it to choose a clear slot, changeable mid-QSO but never mid-burst. The Tab5 also **stops calling a station that is busy with someone else** until they sign off or call CQ again, which additionally stops a merely-popular station being grey-listed; an incoming **`RRR`** now closes a QSO like `RR73`; there is a **QMX volume** slider in dB matching the radio's own LCD (for QMX+ builds with no control panel — Randy N4OPI); and **LoTW uploads finally carry US state and county**, which were missing entirely and cost US operators their Worked All States and county credit
- **v1.3.2 — grid squares logged again** (a long-standing bug left `GRIDSQUARE` off almost every logged QSO — John W5JSS), **PSK Reporter spotting** (on by default; sends your call/grid and the stations you decode over the internet, never on the air), the **User Manual built into the firmware** (instant, no WiFi or SD card — the "Save offline" button is gone because it is no longer needed), **grey-listing** for stations that never answer and **pileup replies** using the intelligent-Transmit laddering (both Roy KI0ER), and an explicit **microSD backup contract** (continuous with WiFi off, one backup per start-up with WiFi on)
- **v1.3.1 — DT + HZ decode-list columns** (each station's slot-timing offset and audio tone, Roy KI0ER's request), 3-letter country codes, an ST7121 boot-diagnostics fix (Paul VE3PIK), and UI alignment polish
- **v1.3.0 — intelligent Transmit & faster replies** (from Roy KI0ER's field feedback): tapping a decoded station sends the correct *next* message WSJT-X-double-click style, hand-run QSOs log to ADIF, replies land on the beat, and a new **Fast pounce** toggle surfaces decodes before the slot boundary so a fresh CQ can be answered in the very next slot *(on by default; not yet verified on a live band — turn it off if your decode counts drop, and please report)*. Plus a rebuilt no-radio-needed **practice simulator**, **USB mouse** support (radio unplugged), and a **web file browser** for the microSD card
- **v1.2.0 — on-device User Manual**: Settings drawer → **User Manual** reads this whole guide on the Tab5 (a native reader with a drag-to-pick Contents page), and flags a firmware update when one is available. *(As of v1.3.2 the manual is built into the firmware and always available — the original download-and-save-to-SD mechanism is gone.)*
- **v1.1.0 — FT8 decode collapse solved**: full decode rate every slot (was ~200–350 ms of QMX audio lost at the USB wire per slot); **microSD is a full station backup** and **GPS time sync is automatic**
- **v1.0.0 foundation — LoTW upload**: QSOs are signed on the device with your own ARRL certificate, completing the logbook trio (QRZ, eQSL, LoTW); plus the FT8 double-send fix, `<...>` callsign resolution, broken-QSO resume, Today/POTA log view, and display sleep
- All panadapter features (spectrum, waterfall, zoom, memory, web UI) are production-ready; FT8 **and FT4** receive/transmit, ADIF logging, and all three logbook uploads are stable
- See [Version History](releases.md) for all changes

## Get Started

**New user?** Start with the [Quick Start](quick-start.md) guide — 10 minutes to on-air.

**Want the whole guide at once?** Download the [User Guide PDF](QMX-Panadapter-UserGuide-v1.4.0.pdf) — a printable 40-page reference.

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
