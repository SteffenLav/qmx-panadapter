# QMX Panadapter

A standalone real-time spectrum analyzer and waterfall display for the [QRP Labs QMX / QMX+](https://qrp-labs.com/qmx.html) family of transceivers, running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5) (ESP32-P4, MIPI-DSI, 5" touchscreen).

The Tab5 connects to the QMX via a single USB cable. It draws I/Q audio over USB Audio Class (UAC) and controls the radio via USB CDC-ACM using the Kenwood TS-480 CAT protocol — no analog tap, no extra wiring.

## Current status

Working signal chain end-to-end:

> QMX RX → USB UAC (48 kHz, 24-bit I/Q) → Tab5 → 1024-pt complex FFT → spectrum canvas at 30 Hz

The live radio frequency is read from the QMX via CAT and displayed on screen. The spectrum updates in real time as the QMX hears RF.

![status](docs/status.png) <!-- replace with a real photo -->

### Done

- **Phase 1** — LVGL UI on the ST7123 panel (M5Stack UserDemo BSP)
- **Phase 2** — USB host + CDC-ACM + Kenwood CAT polling (live frequency display)
- **Phase 3** — USB UAC audio streaming (48 kHz, 2 ch, 24-bit packed I/Q), decoded into a ring buffer
- **Phase 4** — Real-time 1024-pt complex FFT with Blackman-Harris window via [esp-dsp](https://github.com/espressif/esp-dsp), ~48 frames/s
- **Phase 5.1** — Spectrum line graph rendered to an LVGL canvas at 30 Hz, with proper FFT-shifted bin mapping (negative frequencies left, positive right)

### Planned

- **Phase 5.2** — Waterfall (scrolling spectrogram)
- **Phase 5.3** — Frequency axis labels, dB grid
- **Phase 5.4** — Averaging, autoscale, smoothing
- Touch interaction (slip-tune, span control, reference level)
- Memory channels, mode/filter display
- NVS settings persistence
- Optional FT8 decode (via [ft8_lib](https://github.com/kgoba/ft8_lib))

## Hardware

| Item | Notes |
|---|---|
| M5Stack Tab5 | ESP32-P4 v1.3 silicon, 32 MB hex PSRAM, ST7123 panel + touch, MIPI-DSI 1280×720 |
| QRP Labs QMX+ | All-HF-band CW/SSB transceiver, USB composite (CDC + UAC) |
| QRP Labs QMX | 20m and up, QRP version — also works as a target |

> **Important:** Steffen's Tab5 has the **ST7123** panel variant. The publicly distributed M5Stack Tab5 BSP (`espressif/m5stack_tab5` on the component registry) targets ILI9881C and **does not work** on the ST7123 variant. This project uses the local BSP from M5Stack's UserDemo as `components/m5stack_tab5/`.

## Software stack

- **ESP-IDF v5.4.4** (pinned — required for v1.3 silicon + BSP compatibility)
- **FreeRTOS** SMP
- **LVGL v9.2.2**
- **esp-dsp 1.8.2** (FFT and windows)
- **espressif/usb_host_cdc_acm 2.4.0**
- **espressif/usb_host_uac 1.4.0**

CPU is capped at 360 MHz (not the 400 MHz default) — required for the v1.3 engineering-sample silicon.

## Architecture

```
                                  ┌─────────────────────┐
                                  │   M5Stack Tab5      │
                                  │   ESP32-P4          │
                                  │                     │
   QMX+/QMX                       │  ┌───────────────┐  │
   ┌────────┐   USB    CDC-ACM    │  │ cat task      │  │
   │        │◄──────►  ──────────►├──│  (Kenwood)    │  │──► freq label
   │  STM32 │          UAC RX     │  └───────────────┘  │
   │ comp.  │  I/Q 48k/24b ─────► │  ┌───────────────┐  │
   │  USB   │                     │  │ audio task    │  │
   └────────┘                     │  │  ring buffer  │  │
                                  │  └──────┬────────┘  │
                                  │         ▼           │
                                  │  ┌───────────────┐  │
                                  │  │ fft task      │  │
                                  │  │  esp-dsp      │  │
                                  │  │  1024 cplx    │  │──► spectrum[1024]
                                  │  └──────┬────────┘  │
                                  │         ▼           │
                                  │  ┌───────────────┐  │
                                  │  │ render task   │  │
                                  │  │  LVGL canvas  │  │──► MIPI-DSI panel
                                  │  └───────────────┘  │
                                  └─────────────────────┘
```

- `cat/` — USB CDC-ACM client, Kenwood CAT polling, frequency parsing
- `audio/` — USB UAC client, 24-bit packed decode, ring buffer producer
- `dsp/` — Blackman-Harris window + complex FFT + magnitude → dB
- `render/` — 30 Hz canvas updater (Phase 5)
- `ui/` — LVGL widgets, layout, canvas surface
- `display/` — Local M5Stack BSP wrapper, LVGL port init

## Build & flash

Prerequisites:

- ESP-IDF v5.4.4 installed at `C:\esp\v5.4.4\esp-idf` (or equivalent)
- M5Stack Tab5 with ST7123 panel
- QRP Labs QMX or QMX+

```powershell
# One-time
. C:\esp\v5.4.4\esp-idf\export.ps1

# Build & flash
idf.py build
idf.py -p COM3 flash monitor
```

`sdkconfig.defaults` already includes the required tunings:

```ini
CONFIG_ESP32P4_REV_MIN_0=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=2048
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y
```

## Hard-won lessons

A few things that cost real time and may save you yours:

- **`uac_host_install()` needs `create_background_task = true`.** Without it the UAC driver never gets `NEW_DEV` events even though the client is registered. Compositely, this is the single most opaque failure mode in the stack.
- **The QMX delivers 24-bit packed PCM**, not 16-bit and not 24-in-32. Each stereo I/Q pair is exactly 6 bytes, little-endian signed. Misinterpret this and you'll see 1.5× sample rate and pinned peaks.
- **Kenwood `FA;` response is 14 bytes** (not the 15 you might count). 11 digits between `FA` and `;`.
- **`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=2048` is required** — the QMX's composite config descriptor is 268 bytes, the default 256 buffer is too small and enumeration fails silently with `CHECK_SHORT_DEV_DESC FAILED`.
- **ESP32-P4 chip revision must match the menuconfig minimum**, or you'll get an `Illegal instruction` at `call_start_cpu0` before any of your code runs. Set `CONFIG_ESP32P4_REV_MIN_0=y` for engineering-sample silicon.
- **For panadapter use, do a complex FFT with I as real and Q as imaginary.** Real-only FFT throws away the sign of frequency offset from the LO — fine for a single SSB receiver, wrong for a panadapter.
- **No antenna ≠ silence.** The QMX's LO leakage and ADC noise floor will easily peg the I/Q channels. With no antenna, expect `peak=32767` constantly — it's real RF, not a bug.

## License

MIT License

Copyright (c) 2026 Steffen Lav, OZ1LAV

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.


## Author

Steffen Lav OZ1LAV — ham radio operator, embedded developer.
Built across a generous handful of late-night sessions with [Claude](https://claude.ai) as a pair-programming companion.
