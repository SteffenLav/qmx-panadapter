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
- **microSD Auto-Archive** — Insert a card and the diagnostic log, ADIF log, and config are mirrored automatically; a green SD dot in the bottom bar confirms it's active
- **Web UI** — Remote spectrum, waterfall, and control from any browser on the LAN
- **Offline Ready** — Tab5 RTC + SNTP sync; FT8 operates without WiFi (POTA/portable)
- **Multi-frequency Memory** — 32 memory channels (4×8 grid), any frequency/mode, not tied to a band
- **Optional Keyboard** — M5Stack Tab5 snap-on keyboard (70-key) for text entry and one-hand navigation

## Status

**v1.3.1 — a complete, self-contained FT8/FT4 station with no PC in the loop.**

- **v1.3.1 — DT + HZ decode-list columns** (each station's slot-timing offset and audio tone, Roy KI0ER's request), 3-letter country codes, an ST7121 boot-diagnostics fix (Paul VE3PIK), and UI alignment polish
- **v1.3.0 — intelligent Transmit & faster replies** (from Roy KI0ER's field feedback): tapping a decoded station sends the correct *next* message WSJT-X-double-click style, hand-run QSOs log to ADIF, replies land on the beat, and a new **Fast pounce** toggle surfaces decodes before the slot boundary so a fresh CQ can be answered in the very next slot *(on by default; not yet verified on a live band — turn it off if your decode counts drop, and please report)*. Plus a rebuilt no-radio-needed **practice simulator**, **USB mouse** support (radio unplugged), and a **web file browser** for the microSD card
- **v1.2.0 — on-device User Manual**: Settings drawer → **User Manual** reads this whole guide on the Tab5 (a native reader with a drag-to-pick Contents page); with a microSD card it can **Save offline**, and it flags a firmware update when one is available
- **v1.1.0 — FT8 decode collapse solved**: full decode rate every slot (was ~200–350 ms of QMX audio lost at the USB wire per slot); **microSD is a full station backup** and **GPS time sync is automatic**
- **v1.0.0 foundation — LoTW upload**: QSOs are signed on the device with your own ARRL certificate, completing the logbook trio (QRZ, eQSL, LoTW); plus the FT8 double-send fix, `<...>` callsign resolution, broken-QSO resume, Today/POTA log view, and display sleep
- All panadapter features (spectrum, waterfall, zoom, memory, web UI) are production-ready; FT8 **and FT4** receive/transmit, ADIF logging, and all three logbook uploads are stable
- See [Version History](releases.md) for all changes

## Get Started

**New user?** Start with the [Quick Start](quick-start.md) guide — 10 minutes to on-air.

**Want the whole guide at once?** Download the [User Guide PDF](QMX-Panadapter-UserGuide-v1.3.1.pdf) — a printable 40-page reference.

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
