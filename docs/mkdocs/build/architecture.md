# Architecture Overview

## Module Map

| Path | What lives there |
|---|---|
| `main.c` | app_main, task launch, orchestration |
| `display/` | display driver, BSP init, LCD + touch |
| `ui/` | LVGL widgets, spectrum/waterfall canvases, FT8 screen |
| `cat/` | USB CDC-ACM CAT polling, QMX control |
| `audio/` | USB UAC consumer (core 0, polling mode) |
| `dsp/` | FFT, spectrum, I/Q balance, CW demod |
| `ft8_tx.c` | FT8 TX engine, arm/run/abort, tone finder |
| `ft8_test.c` | FT8 RX: slot loop, capture, decode, state machine |
| `ft8_sim.c` | FT8 simulation mode (phantom stations) |
| `ft8_qso.c` | FT8 QSO state machine (pounce + CQ-run) |
| `render/` | 30 Hz render task, spectrum smoothing, waterfall |
| `wifi/` | WiFi + SNTP |
| `net/webserver.c` | HTTP server, web UI endpoints, WebSocket |
| `adif/` | ADIF logging, QRZ/eQSL upload |
| `storage/` | NVS settings, config I/O |
| `rtc/` | RTC driver (RX8130CE) |
| `time_sync/` | Time orchestrator (SNTP/RTC/QMX/manual) |
| `util/` | FPS counter, diagnostic logging, bandplan |
| `espressif__usb_host_uac/` | UAC + CDC-ACM coexistence (patched) |
| `espressif__esp_lcd_touch_st7123/` | ST7121/ST7123 compatibility (patched) |

## Data Flow

### Audio Path (Spectrum + Waterfall)

```qmxdiagram
type: flow
title: From the radio to the screen
node: QMX USB audio - 48 kHz, 24-bit I/Q
node: audio_task - core 0, polling
branch: I/Q balance correction, per sample
branch: DC blocker
node: ring buffer in PSRAM
node: fft_task - core 1
branch: 512-point FFT, Blackman-Harris window
branch: EMA smoothing, dB scaling
node: spectrum mutex
node: render_task - 30 Hz
node: LVGL canvases - spectrum and waterfall
note: dim | the audio task polls rather than waiting on events: event-driven reads truncated the UAC chunks and pumped the noise floor
```


### FT8 Receive Path

```qmxdiagram
type: flow
title: From the slot boundary to a row on screen
node: RX slot boundary - every 15 s, UTC-aligned
node: ft8_task on core 1
branch: dsp_ft8_capture() - runs TO the next UTC boundary, ~180000 samples at 12 kHz
branch: streaming STFT builds the waterfall DURING capture, into PSRAM
branch: candidate search - up to FT8_MAX_CANDIDATES (140) sync hits
branch: decode_candidate_range() - LDPC per candidate
node: ft8_dec0 on core 0 takes the other half of the candidates in parallel
node: ft8_screen_record_decode() then ft8_qso_advance()
node: ft8_screen_view - the live decode list
note: dim | the candidate cap is 140, not ~10: on a busy band the search hits it every slot, and most are false syncs that burn a full LDPC budget before failing
```

### FT8 Transmit Path

```qmxdiagram
type: flow
title: From a tap to a keyed radio
node: operator taps Transmit, or the QSO machine decides
node: ft8_tx_build_request() - build the message text
node: ft8_tx_arm() - stage it for a slot
branch: refuses while the radio is released (see "Let me use the QMX menus")
node: ft8_tx_run() at the slot boundary
branch: cat_poll_set_paused(true) for the whole burst
branch: TX; then 79 x TA<freq>; at 160 ms, then TA0; and RX;
branch: FT4: 105 tones at 48 ms instead
node: ft8_qso_on_tx_complete() - re-arm the next message
note: amber | the tail always runs, even on an abort or an error, so the radio cannot be left keyed
```

## Task Priorities

```
Priority 25 (highest) — app_main (setup only)
Priority 24 — WiFi / SNTP
Priority 10 — web server
Priority 6  — FT8 transmit ISR
Priority 4  — FFT (ring buffer consumer, spectrum producer)
Priority 1  — FT8 decode, CAT poll, render, LVGL, time sync
```

**Critical:** FT8 decode runs at priority 1 — lowest. This ensures real-time tasks (FFT, USB) never starve, and the UI thread remains responsive.

## DSP Pipeline

### Spectrum Calculation

```qmxdiagram
type: flow
title: Ring buffer to canvas
node: ring buffer - 64 KB in PSRAM, 16384 I/Q pairs, about 1.4 s at 12 kHz
node: FFT - 1024-point complex, Blackman-Harris by default
node: magnitude, then scaled
branch: dBm = 20*log10(mag) - 148 dB calibration offset
branch: flat = 20*log10(mag / per-bin floor)
node: spectrum mutex
node: render_task at 30 Hz - EMA smoothing
node: LVGL canvas push
note: dim | the FFT window is selectable in the drawer: Blackman-Harris, Hann or Nuttall
```

Details worth knowing, each of them load-bearing:

- **FFT** — 1024-point complex. The **esp-dsp ANSI fallback** is used deliberately: the PIE/vector build crashes under sustained WebSocket load on this silicon.
- **IF offset** — the QMX presents I/Q at **+12 kHz**, so the spectrum, waterfall *and* S-meter all shift bin selection by `n_bins/4` to put the VFO signal at the visual centre. Miss that shift and the S-meter reads the DC/LO spike instead of the signal.
- **DC blocker** — a one-pole IIR on the I/Q stream, ahead of the FFT.
- **Spectrum smoothing** — per-bin EMA, α = 0.4 by default and adjustable in the drawer.
- **dBm calibration** — `DSP_DB_CALIBRATION_OFFSET = −148.0 dB`, measured on a dummy load: the noise floor reads −130 dBm and S9 = −73 dBm.
- **Waterfall scroll** — a 1280×824 double-height canvas, so a new row costs about **130 µs** instead of the ~92 ms a `memmove` would take.
- **Audio task** — polling on core 0 with a drain loop, **not** event-driven. Event-driven reads returned truncated UAC chunks that saturated the FFT input and pumped the noise floor on a slow ~13 s cycle.

### I/Q Balance (Gram-Schmidt)

Per-sample correction applied in `audio.c` before ring buffer:

```qmxdiagram
type: flow
title: Blind adaptive correction, per sample
node: I_in, Q_in - core 0, 48 kHz
node: DC tracker, tau = 1 s
branch: I_out = I_in - I_dc, Q_out = Q_in - Q_dc
node: power and cross-product trackers
branch: K_amp from the I and Q powers, tau = 200 ms
branch: K_phi from the I*Q cross product, tau = 1 s
node: correction - Q_final = (Q_out - K_phi * I_out) * K_amp
node: ring buffer push
note: steel | I is never touched; the whole correction lands on Q
note: dim | all constants run 8x faster for the first 2 s after a reset, so it converges in about 125 ms
```

## Waterfall

```qmxdiagram
type: flow
title: Why the waterfall scrolls without moving memory
node: dsp_ft8_capture() during FT8 RX
branch: STFT while the slot is still being captured
branch: per-bin PSRAM buffer, rolling window
node: render_waterfall_tick()
branch: 1280 x 824 canvas - twice the visible height
branch: the new row is written at s_wf_head AND at s_wf_head + WATERFALL_H
branch: the VIEW pointer moves; the pixels do not
node: colorise with the SDR gradient, then push to LVGL
note: trace | this is the trick: a memmove per tick costs ~92 ms, moving the view pointer costs ~130 us
```

## FT8 Decode List

```qmxdiagram
type: flow
title: From a decode to a row you can tap
node: ft8_screen_decode_queue - 4-buffer pool
node: ft8_screen_record_decode()
branch: call, grid, SNR, audio tone, time
node: ft8_screen_get_all() - snapshot for the UI
branch: expire anything not re-heard within 60 s
branch: apply the include / exclude / worked-before filters
branch: sort - CQ callers and stations answering us first
node: ft8_screen_view.c - the LVGL list
branch: own-call rows inverted, red fill and white text
note: dim | the list is a picture of who is on frequency NOW, not a log: a station that stops transmitting drops off after a minute
```

## Storage

### NVS (Non-Volatile Storage)

Settings namespace: `QMX`

```
Key             Type     Example
wifi_ssid       string   "MyNet"
wifi_pass       string   "password123"
callsign        string   "OZ1LAV"
grid            string   "JO45"
last_freq_20m   uint32   14074000
last_mode_20m   uint8    2 (USB)
memory_1        blob     { freq, mode, bw, name }
...
diag_log        uint8    1 (on/off)
```

### ADIF Log

File: `/spiffs/qso.adi`

Format: Standard ADIF (one QSO per `<EOR>` record). Each QSO stores:

```
CALL, GRIDSQUARE, RST_SENT, RST_RCVD, QSO_DATE, TIME_ON, FREQ, BAND,
MODE (+ SUBMODE on FT4), STATION_CALLSIGN, MY_GRIDSQUARE,
MY_SIG/MY_SIG_INFO (our activation), SIG/SIG_INFO (theirs), ...
```

Downloaded via web UI or serial.

## Timing & Slots

### FT8 Slot (15 seconds)

```qmxdiagram
type: timeline
title: One FT8 slot, from the UTC boundary
span: 15
seg: 0-13.2 steel Capture
seg: 13.2-15 trace Decode
mark: 0-2.8 amber TX window
tick: 0, 2.8, 13.2, 15
note: steel | 0 s - boundary. ft8_task wakes, checks for an armed TX, then dsp_ft8_capture_begin()
note: amber | 0-2.8 s - an armed reply fires on THIS slot, not the next one (FT8_REPLY_TX_WINDOW_MS)
note: trace | 13.2-15 s - capture is cut 1.8 s early (FT8_DECODE_RESERVE_MS) so the candidate search and LDPC finish BEFORE the boundary
note: dim | the signal itself ends at 12.64 s - 79 symbols x 0.16 s
note: dim | with Fast pounce off, capture instead runs to the boundary and the decode lands in the next slot
```


### UTC Boundary Alignment

The capture window is **UTC-clamped**, not fixed-sample-count. Actual window length is computed:

```
ms_to_boundary = 15000 - (sys_time_ms % 15000)
dsp_ft8_capture(timeout = min(ms_to_boundary, SLOT_TIMEOUT_MS))
```

If samples run short before the boundary, DSP zero-pads the rest. This keeps the capture window locked to UTC, preventing the multi-slot drift problem seen in earlier versions.

---

**Next:** Read [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md) for the detailed quirks and critical knowledge, or contribute via [Contributing](contributing.md).
