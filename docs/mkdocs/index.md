# QMX Panadapter

A standalone real-time panadapter — spectrum analyser and waterfall — for the [QRP Labs QMX/QMX+](https://www.qrp-labs.com/qmxp.html) HF transceiver.

Running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/tab5) (ESP32-P4 with a 5" 720×1280 touch display), the panadapter cconnects to the QMX as a USB host, decodes the I/Q in real time, and renders a touch-driven interface with tap-to-tune, pinch-zoom, onboard FT8 decoding and transmit, ADIF logging, and a matching browser web UI.

**By Steffen Lav (OZ1LAV)**

## Features

- **Spectrum & Waterfall** — Real-time 4800-sample/s FFT with adaptive noise floor, spectrum curve, and colour waterfall
- **Touch Interface** — Tap-to-tune, pinch-zoom, one-finger pan, edge-swipe navigation
- **FT8 Decoding** — Onboard decode with configurable filtering, worked-before exclusion, and priority ranking
- **FT8 Transmit** — Reply to CQ, run CQ, auto-QSO (robot mode), full exchange, ARRL Field Day mode
- **ADIF Logging** — Every QSO logged locally, with QRZ Logbook and eQSL upload
- **Web UI** — Remote spectrum, waterfall, and control from any browser on the LAN
- **Offline Ready** — Tab5 RTC + SNTP sync; FT8 operates without WiFi (POTA/portable)
- **Multi-frequency Memory** — 10 per-band memory channels, band-specific frequency/mode recall
- **Optional Keyboard** — M5Stack Tab5 snap-on keyboard (70-key) for text entry and one-hand navigation

## Status

**v0.19.0 — Stable for all features except FT8/FT4 transmit, which is functional but un-soaked for multi-hour sessions.**

- All panadapter features (spectrum, waterfall, zoom, memory, web UI) are production-ready
- FT8 and FT4 receive, plus ADIF logging, are stable
- FT8 and FT4 transmit work but lack multi-hour soak testing and duty-cycle protection (FT4 TX is new in v0.19.0)
- See [Version History](releases.md) for all changes

## Get Started

**New user?** Start with the [Quick Start](quick-start.md) guide — 10 minutes to on-air.

**Want the whole guide at once?** Download the [User Guide PDF](QMX-Panadapter-UserGuide-v0.19.0.pdf) — a printable 40-page reference.

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
