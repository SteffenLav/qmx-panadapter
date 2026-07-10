# QMX Panadapter

A standalone real-time panadapter — spectrum analyser and waterfall — for the [QRP Labs QMX/QMX+](https://www.qrp-labs.com/qmxp.html) HF transceiver.

Running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/tab5) (ESP32-P4 with a 5" 720×1280 touch display), the panadapter connects to the QMX as a USB host, decodes the I/Q in real time, and renders a touch-driven interface with tap-to-tune, pinch-zoom, onboard FT8/FT4 decoding and transmit, ADIF logging, and a matching browser web UI.

**By Steffen Lav (OZ1LAV)**

## Features

- **Spectrum & Waterfall** — Real-time 4800-sample/s FFT with adaptive noise floor, spectrum curve, and colour waterfall
- **Touch Interface** — Tap-to-tune, pinch-zoom, one-finger pan, edge-swipe navigation
- **FT8 & FT4 Decoding** — Onboard decode for both modes with configurable filtering, worked-before exclusion, and priority ranking
- **FT8 & FT4 Transmit** — Reply to CQ, run CQ, auto-QSO (robot mode), full exchange, ARRL Field Day mode
- **ADIF Logging** — Every QSO logged locally, with QRZ Logbook and eQSL upload
- **microSD Auto-Archive** — Insert a card and the diagnostic log, ADIF log, and config are mirrored automatically; a green SD dot in the bottom bar confirms it's active
- **Web UI** — Remote spectrum, waterfall, and control from any browser on the LAN
- **Offline Ready** — Tab5 RTC + SNTP sync; FT8 operates without WiFi (POTA/portable)
- **Multi-frequency Memory** — 32 memory channels (4×8 grid), any frequency/mode, not tied to a band
- **Optional Keyboard** — M5Stack Tab5 snap-on keyboard (70-key) for text entry and one-hand navigation

## Status

**v0.20.1 — Robustness release + pounce-crash hot-fix. All features are stable; the Beta label remains only until LoTW / TQSL log upload lands (v1.0.0).**

- **v0.20.0 headline — reliability**: the "WiFi dies after a few minutes" fault now **self-heals** instead of needing a reboot; opening a window no longer freezes the device; the radio-control (CAT) link rides out USB glitches; and the web UI pauses its stream while FT8 is running so decoding and WiFi stay steady
- **FT4 is temporarily disabled in v0.20.0** — it was exhausting the device's internal memory and crashing. Fully reversible, and FT8 is unaffected
- All panadapter features (spectrum, waterfall, zoom, memory, web UI) are production-ready
- FT8 receive **and transmit**, plus ADIF logging, are stable
- Also in v0.20.0: FT8 pile-up list, "Skip TX1" quick pounce, 11 m/CB band, a memory-channel overhaul (example channels, first-run tour, drag-to-wastebin delete), battery-care charge limit, and a batch of web-UI improvements (whole-band plan strip, draggable split, better screenshots and frequency keypad)
- Earlier fixes still in place: logbook uploads (QRZ/eQSL) reliable while FT8 runs (v0.19.1); USB reconnect + disconnect-race crashes (v0.19.2); QMX IQ-mode handshake auto-retry (v0.19.3–4); AM mode + Antenna Tune for QMX 1_04+ firmware (v0.19.5)
- See [Version History](releases.md) for all changes

## Get Started

**New user?** Start with the [Quick Start](quick-start.md) guide — 10 minutes to on-air.

**Want the whole guide at once?** Download the [User Guide PDF](QMX-Panadapter-UserGuide-v0.20.1.pdf) — a printable 40-page reference.

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
