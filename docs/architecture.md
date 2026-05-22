# Architecture

This document explains how the QMX panadapter is structured: what runs where, why each design choice was made, and what was tried that didn't work. It's deliberately more opinionated than the README — this is the captain's log of the conceptual ship.

## Design goals

1. **Single USB cable** between QMX and Tab5. No analog tap, no soldering. The QMX already exposes a composite USB device (CDC for CAT + UAC for I/Q audio); we just have to use both ends of it.
2. **Standalone** — no PC, no SDR software, no host. The Tab5 is the panadapter.
3. **Real-time** — spectrum should feel responsive: a strong signal appears within ~50 ms of the antenna seeing it.
4. **Honest** — show what the radio actually hears. No fake smoothing that hides weak signals. (Smoothing is opt-in.)
5. **Clean separation** — audio acquisition, DSP, and rendering should not know about each other beyond well-defined interfaces.

## High-level data flow

```
  ┌──────────────────────────────────────────────────────────────────┐
  │                          QMX / QMX+                              │
  │   Antenna → mixers → I/Q baseband → CODEC → USB composite        │
  │                                                                  │
  │     CDC interface (IF 0/1)        UAC interface (IF 2/3/4)       │
  │       ↕  CAT commands               ↓ isoc IN, 48 kHz 24-bit     │
  └──────────────────────────────────────────────────────────────────┘
           ↑                                    ↓
           │ USB bulk OUT/IN                    │ USB isoc IN
           │ (Kenwood ASCII)                    │ (I/Q stereo 24-bit)
           │                                    │
  ┌────────┴────────────────────────────────────┴────────────────────┐
  │                       M5Stack Tab5 (ESP32-P4)                    │
  │                                                                  │
  │   ┌─────────────┐                  ┌─────────────────┐           │
  │   │ cat task    │                  │ audio task      │           │
  │   │ (core 0)    │                  │ (core 1, prio 5)│           │
  │   │             │                  │                 │           │
  │   │ poll FA;    │                  │ decode 24b →    │           │
  │   │ every 200ms │                  │ int16 stereo →  │           │
  │   │             │                  │ ring buffer     │           │
  │   └──────┬──────┘                  └────────┬────────┘           │
  │          │                                  │                    │
  │          ▼                                  ▼                    │
  │   ui_update_frequency()         FreeRTOS RingBuffer 64 KB        │
  │          │                                  │                    │
  │          │                                  ▼                    │
  │          │                         ┌─────────────────┐           │
  │          │                         │ fft task        │           │
  │          │                         │ (core 1, prio 4)│           │
  │          │                         │                 │           │
  │          │                         │ read 1024 pairs │           │
  │          │                         │ window × FFT    │           │
  │          │                         │ → spectrum[1024]│           │
  │          │                         │   (mutex)       │           │
  │          │                         └────────┬────────┘           │
  │          │                                  ▼                    │
  │          │                         ┌─────────────────┐           │
  │          │                         │ render task     │           │
  │          │                         │ (core 0, prio 3)│           │
  │          │                         │                 │           │
  │          │                         │ get_spectrum →  │           │
  │          │                         │ canvas pixels   │           │
  │          │                         └────────┬────────┘           │
  │          │                                  │                    │
  │          ▼                                  ▼                    │
  │   ┌─────────────────────────────────────────────────────┐        │
  │   │              LVGL UI (core 0, prio 4)               │        │
  │   │  ┌──────────────────────────────────────────────┐   │        │
  │   │  │ frequency  mode  S-meter        [≡] menu     │   │ 60px   │
  │   │  ├──────────────────────────────────────────────┤   │        │
  │   │  │ spectrum canvas (green line, RGB565)         │   │ 200px  │
  │   │  ├──────────────────────────────────────────────┤   │        │
  │   │  │ waterfall canvas (Phase 5.2)                 │   │ 990px  │
  │   │  ├──────────────────────────────────────────────┤   │        │
  │   │  │ status bar: span / ref / avg / FPS           │   │ 30px   │
  │   │  └──────────────────────────────────────────────┘   │        │
  │   └─────────────────────────────────────────────────────┘        │
  │                            │                                     │
  │                            ▼                                     │
  │                       MIPI-DSI (720×1280 portrait, ST7123)       │
  └──────────────────────────────────────────────────────────────────┘
```

## Process model

All real work lives in FreeRTOS tasks. None of the producer tasks block; the consumer tasks block waiting on shared buffers or mutexes. The pipeline is one-directional and lossy by design — if the renderer is busy when a new FFT result lands, the renderer just sees the latest one on its next tick. No backpressure, no queues except where strictly needed.

| Task         | Core | Prio | Period           | What it does                              |
|--------------|------|------|------------------|-------------------------------------------|
| LVGL         | 0    | 4    | 33 ms (port tick)| Flushes canvas to display via MIPI-DSI    |
| USB host     | 0    | 5    | event-driven     | USB transfers, enumeration, ISR fanout    |
| audio        | 1    | 5    | event-driven     | UAC RX_DONE → 24-bit decode → ring buffer |
| fft          | 1    | 4    | when 1024 pairs available | Window + complex FFT + magnitude → dB |
| cat          | 0    | (low)| 200 ms           | Send `FA;`, parse 14-byte response        |
| render       | 0    | 3    | 33 ms            | Read spectrum snapshot, draw to canvas    |

Core 1 owns audio + DSP. Core 0 owns USB + UI + rendering. This keeps the FFT off the UI core so a slow draw cycle can't drop samples.

## Module breakdown

### `display/`
Thin wrapper over the M5Stack UserDemo BSP. Exposes `display_init()`, `display_lock()`, `display_unlock()`. The lock is LVGL's mutex; anything that touches LVGL state must hold it.

### `ui/`
LVGL widgets: top bar (frequency, mode, S-meter), spectrum canvas, waterfall canvas, status bar. Public API is intentionally small: `ui_update_frequency()`, `ui_push_spectrum()`, `ui_push_waterfall_row()`. Producers don't touch LVGL directly — they call these.

### `cat/`
USB CDC-ACM client. On QMX connect, opens the CDC interface at 38400 baud 8N1, dumps the device descriptors (informational), then enters a polling loop sending `FA;` every 200 ms. The response is parsed into Hz and pushed via `ui_update_frequency()`. Future work: mode tracking (`MD;`), S-meter readback (`SM;`).

### `audio/`
USB UAC client. Discovers the audio interface dynamically via `uac_host_get_device_alt_param` — we do *not* hardcode format because different QMX firmware versions may negotiate differently. On RX_DONE events:

1. Read up to 4 KB raw bytes from the UAC driver.
2. Decode packed 24-bit little-endian signed I/Q pairs (3+3 bytes per stereo pair).
3. Shift right by 8 to scale into int16 range, track peak L/R.
4. Push interleaved L/R int16 into a 64 KB FreeRTOS ring buffer.
5. Drop on overflow (never block USB).

The 64 KB ring buffer gives ~341 ms of headroom at 48 kHz stereo int16 — more than enough margin for the FFT consumer.

Public API: `audio_init()` and `audio_read_samples(int16_t *dst, size_t max_pairs, uint32_t timeout_ms)`.

### `dsp/`
The FFT engine. On init:
- Allocates window, workbuf, and spectrum arrays in **internal RAM** (not PSRAM) for speed.
- Precomputes a Blackman-Harris window via esp-dsp.
- Initializes esp-dsp's radix-2 complex FFT for N=1024.
- Runs a self-test: synthetic complex tone at bin 100 should produce a peak at bin 100 after FFT.

The runtime task loop:
1. Block until 1024 stereo pairs are available in the audio ring buffer.
2. For each pair, multiply I and Q by the window coefficient → workbuf as interleaved (re, im).
3. Run `dsps_fft2r_fc32` + `dsps_bit_rev_fc32` in place.
4. Compute `mag² = re² + im²` per bin (skip sqrt; absorb into log).
5. Convert to dB: `10 * log10(mag²)`. Floor at `1e-12` to avoid `log(0)`.
6. Publish under mutex into `s_spectrum[1024]`.

Spectrum frames produced: ~47/s (= 48000 / 1024). FFT itself takes <1 ms on the P4 at 360 MHz.

Public API: `dsp_init()` and `dsp_get_spectrum(float *dst)` — the consumer copies out the latest snapshot.

### `render/`
30 Hz task that pulls the latest spectrum and pushes it to the UI. Naturally downsamples (FFT runs at 47 Hz, render at 30 Hz — every render gets the most recent FFT result). Lives on core 0 so it shares cache and the LVGL lock with the UI thread.

### `util/`
Bits and pieces — currently just an FPS counter that walks LVGL's display structure and reports actual frames per second to the status bar.

## Key design decisions

### Complex FFT, not real FFT

A panadapter must distinguish signals above the LO from signals below it. The QMX delivers I and Q channels that together form a complex baseband signal. A real-input FFT would give a mirror-symmetric spectrum — fine for a single-channel SSB receiver, but wrong for a panadapter, which by definition shows asymmetry around the tuned frequency.

We do a complex FFT with `I → real`, `Q → imag`. The output of N=1024 has:
- Bin 0 = DC (tuned center)
- Bins 1..511 = positive frequencies (above the LO, up to +Fs/2)
- Bins 512..1023 = negative frequencies (below the LO, wrapped: bin 1023 is the most negative)

For display we "fftshift": map screen column x=0 to the most negative bin, x=W/2 to DC, x=W-1 to the most positive. The shift is just an index translation, no data movement.

### Window choice: Blackman-Harris

Three common options:
- **Hann/Hanning** — easy, -31 dB sidelobes, modest main lobe width
- **Hamming** — -42 dB sidelobes, similar main lobe
- **Blackman-Harris (4-term)** — -92 dB sidelobes, wider main lobe

For a panadapter the priority is **sidelobe suppression** — a single strong signal should not bleed into adjacent bins and bury weaker nearby signals. We give up some main lobe sharpness for that. Blackman-Harris is the standard SDR choice and esp-dsp provides it precomputed.

### FFT size: 1024

The tradeoff is bin resolution vs. update rate vs. CPU.

| N    | Δf (Hz) | Frame ms | Frames/s | Notes                                |
|------|---------|----------|----------|--------------------------------------|
| 512  | 93.75   | 10.7     | 94       | Coarse but very responsive           |
| 1024 | 46.88   | 21.3     | 47       | Sweet spot                           |
| 2048 | 23.44   | 42.7     | 23       | Fine resolution, harder to see signals come and go |
| 4096 | 11.72   | 85.3     | 12       | Sluggish, only useful for narrow modes |

1024 gives ~47 Hz/bin — fine enough to separate CW signals (which are typically spaced 100s of Hz apart) at a refresh rate well above the eye's smoothness threshold.

### No overlap (yet)

50%-overlapped FFTs (Welch's method) give smoother visual updates and slightly better spectral estimates at the cost of 2× CPU. We start without it; if the waterfall in Phase 5.2 looks stuttery we'll add it.

### Sample format: int16 in the ring buffer

The QMX delivers 24-bit packed I/Q (~140 dB dynamic range). We decode to int16 (~96 dB) for the ring buffer. Why throw away 48 dB?

- Display range is ~120 dB end to end, and that's *with* generous headroom
- Eye can resolve maybe 6 dB steps, screen has 200 px vertically → ~50 dB usable
- int16 → smaller ring buffer (4 bytes/pair vs 8) → less PSRAM pressure
- FFT is `float32` anyway, so we cast on the way in

If we ever need the extra range (e.g., for weak-signal modes like FT8), we can switch the ring buffer to int32 packed-24 and pay the memory cost.

### One spectrum, one renderer, no queue

The FFT publishes `s_spectrum[1024]` under a mutex. The renderer copies it under the same mutex. If two FFT results land before one render, the older one is silently discarded — which is exactly what we want. A queue would buffer stale spectra; lossy snapshots are correct.

### LVGL canvas, not chart

LVGL's `lv_chart` was tried mentally and rejected: it auto-scales axes, has fixed point styles, and refreshes by re-evaluating its entire data array on every redraw. We want full pixel control and predictable performance.

The canvas is 1280×200 RGB565 in PSRAM (~512 KB). Each render frame:
1. Memset the canvas to black
2. For each of 1280 columns: map column → fftshifted bin → dB → pixel y, draw a vertical green line from y to bottom
3. Call `lv_obj_invalidate()`

LVGL marks the canvas dirty and the next display tick (33 ms cycle, MIPI-DSI) pushes the new pixels.

## Memory map

Total allocations after all subsystems are running:

| Region          | Use                            | Size      |
|-----------------|--------------------------------|-----------|
| Internal RAM    | FFT window / workbuf / spectrum| ~16 KB    |
| Internal RAM    | All stacks + LVGL working      | ~75 KB    |
| Internal RAM    | Free                           | ~250 KB   |
| PSRAM           | LVGL display draw buffers      | ~280 KB   |
| PSRAM           | Spectrum canvas (RGB565)       | 512 KB    |
| PSRAM           | Waterfall canvas (RGB565)      | ~1.1 MB   |
| PSRAM           | Sample ring buffer             | 64 KB     |
| PSRAM           | Renderer scratch               | 4 KB      |
| PSRAM           | Free                           | ~28 MB    |
| Flash           | App image                      | 776 KB    |
| Flash           | Free (in 8 MB factory part.)   | ~7.3 MB   |

We're nowhere near memory limits. The bottleneck is and will remain CPU time on core 1 during FFT bursts.

## Failure modes worth knowing about

### USB enumeration fails with `CHECK_SHORT_DEV_DESC FAILED`
Usually means either:
- `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` is below 2048 (need 268+ for QMX composite descriptor)
- QMX needs a power cycle (its internal USB stack occasionally gets confused)

### UAC stream never starts
`uac_host_install()` must have `create_background_task = true`. Without it the UAC client is registered but its event handler never runs, so `NEW_DEV` events sit in a queue forever and `RX_CONNECTED` never fires.

### Sample rate appears as 72000 pairs/s instead of 48000
Decoder is treating data as 16-bit when it's actually 24-bit packed (3 bytes/sample → 6 bytes/pair). Divide bytes_read by 6, not 4.

### `peak L=32767 R=32767` constantly even with no signal
This is real. With no antenna, the QMX's LO leakage and ADC noise floor pin the I/Q ADCs. Connect an antenna and tune to a quiet frequency to see actual noise floor (~hundreds to low thousands).

### Illegal instruction at `call_start_cpu0` immediately after flash
Chip revision mismatch in menuconfig. ESP32-P4 v1.3 silicon needs `CONFIG_ESP32P4_REV_MIN_0=y`. `idf.py fullclean` and rebuild after changing.

### Spectrum looks frozen or stale
Check that the LVGL lock isn't being held by a slow operation. The status bar update (`ui_set_fps_text`) and frequency update (`ui_update_frequency`) both take it; if either blocks for more than ~30 ms the render skips a frame.

## Comparison with the reference codebase

A separate, related codebase (`qrp_companion`) was evaluated as an accelerator. It has:
- A Platform Abstraction Layer cleanly separating ESP32, STM32, and PC builds
- A WOLA (weighted overlap-add) filter bank for DSP
- A Qt5/PortAudio desktop emulator for PC-based development
- Mongoose web UI

Why we didn't fork it:
- Its DSP is filter-bank-oriented (good for demodulation), not optimized for panadapter spectrum display
- The PAL abstraction adds complexity we don't need on a single-target build
- The composite USB device handling would need significant rework anyway

We do borrow ideas: the PAL pattern is a good North Star if we ever port to another MCU, and the WOLA approach may be useful for Phase 5.4 (smoothing).

## What's next

Phase 5.2–5.4 in order of priority:

1. **Waterfall**: replace the orange/blue gradient with a scrolling 2D spectrogram. Each spectrum frame shifts the existing rows down by 1 px and writes the new row at the top, mapped through a viridis-style color LUT.
2. **Frequency axis labels**: tick marks every 5 kHz, labels every 10 kHz, anchored to the live QMX frequency from CAT.
3. **Smoothing / averaging**: 4-frame moving average, optional. Should be a toggle in the bottom bar.
4. **Autoscale**: track 5th and 95th percentile of recent spectra, map those to display bottom/top.
5. **Slip-tune**: touch the spectrum, the QMX retunes via `FA;` CAT command.
6. **Mode tracking**: poll `MD;`, update the mode label, switch span/filter assumptions accordingly.

Beyond that: memory channels, NVS persistence, FT8 decode, audio output to Tab5 speaker, optional TX support.
