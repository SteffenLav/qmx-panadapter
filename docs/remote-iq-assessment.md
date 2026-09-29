# Remote operation: the Tab5 as an I/Q server

Status: **assessment only.** Nothing here is built, nothing is measured on the
bench, and one number that decides the whole idea is still unknown. Written
2026-09-27 from a discussion on the Waveshare port bench; the subject is the
Tab5, so it is handed to this tree rather than kept there.

## The idea, in its final form

The operator works the QMX remotely. The PC running the browser does **all**
signal processing — panadapter FFT and waterfall, FT8 and WSPR decode, audio
demodulation, AGC, binaural. The Tab5 shuts down its screen and becomes a pipe:
USB host for I/Q and CAT, WiFi, and a websocket.

The web interface keeps doing what it does today, plus audio.

## Why this is not a separate firmware image

It has to be ONE app with a mode, not a second product. A dedicated streaming
image makes the Tab5 an expensive streamer that a Raspberry Pi does better and
cheaper. The value is that the same device is a standalone panadapter at the
desk and an I/Q server from the road, with a real front panel, CAT control, a
QSO log that keeps itself, and `gpio_relay.c` to power-cycle the radio.

## ⭐ A BOOT MODE, NOT A RUNTIME SWITCH

This is the difference between a tractable change and a rewrite.

`app_main` is a flat sequence of `*_init()` calls. A branch that skips the
display/LVGL/DSP/decoder block and starts a streamer instead is a small,
readable change to a structure that already exists.

A **runtime** switch is not, for two reasons this codebase already states:

- **Nothing has a teardown path.** Tasks are created at boot and never stopped.
  LVGL, `render`, `dsp`, the decoders — all one-way.
- **The early allocations are deliberately early.** `dsp_rxaudio_ring_preinit()`
  and the RX-audio I2S preopen claim their memory BEFORE WiFi and BLE fragment
  internal RAM, with comments saying that taking it and releasing it again
  "would be no help at all". Freeing them mid-session and re-claiming later is
  exactly what those comments warn against. You would win core 0 and lose the
  heap.

So: persist the mode to NVS and reboot into it. Five seconds, no teardown, no
fragmentation question. OTA already reboots the device routinely.

## ⛔ THE ONE NUMBER THAT DECIDES IT

**Can esp_hosted → C6 → lwIP → httpd websocket sustain ~1.5 Mbit/s with low
enough jitter?** Nobody has measured it.

Full I/Q is 48 kHz x 2 x 16-bit = **1.54 Mbit/s** sustained, and in this design
it has to be all of it — the PC cannot be given only a passband when it must
compute the panadapter and decode across the whole span.

| | rate |
|---|---|
| raw 16-bit, 48 kHz | 1.54 Mbit/s |
| 12-bit block-float packing | ~1.15 Mbit/s |
| decimate to +/-12 kHz span | 768 kbit/s |
| both | ~580 kbit/s |

On a LAN this is nothing. Over typical home upstream it is real but usually
fine. A dropout does not merely glitch the audio — it loses a decode.

⚠ **The only nearby data point does not transfer.** `webserver_ws.c` records
that 10 fps of spectrum frames was "the last straw" that audibly broke RX audio
(operator A/B: closing the browser moved idle0 from 0.1% to 2.9%). That was
measured with LVGL, `render` and the software rotation all running, which is
precisely what streaming mode deletes. Do not read it either way without
re-measuring.

**The experiment:** a streaming-mode boot path that starts nothing but USB
host, CAT, WiFi and a websocket sink, then see what megabits come out and with
what jitter. It is a branch in `app_main` and a socket. Everything else waits on
the answer.

## What cannot move: TX timing

RX offloads cleanly. Transmit does not.

- **CW keying** — ~60 ms per element at 20 WPM. Not remotely schedulable.
  Stays on the Tab5.
- **FT8** — 15 s slots, sub-second tolerance. The *decision* can come from the
  browser; the slot-accurate burst must be armed and run locally.
- **WSPR** — 110 s on a 2-minute boundary. Network jitter is irrelevant; this
  one could be browser-driven.

So the Tab5 keeps CAT, the TX engines and their schedulers. It is a pure pipe
for RX and a command target for TX.

## The decoders port more easily than expected

`ft8_lib`, `wspr_decode`/`wspr_fano`, `ansi_term`, `db_gridlines`, `net_guard`
and `cw_decode` are already portable C with no ESP dependencies **and host test
harnesses** (`tools/run_harnesses.py`). That is most of the way to WASM, and the
harnesses become the conformance tests for the browser port — the same inputs
must give the same decodes.

CW is free: the QMX decodes it itself and the Tab5 only reads `TB;` over CAT.

## Decide early: who is the system of record?

"The web interface keeps doing what it does today" quietly includes server-side
state — the ADIF log, the microSD archive, Cloudlog/QRZ/LoTW/eQSL upload, PSK
Reporter, the file browser, settings, OTA. If FT8 decoding happens in the
browser, then what gets logged and reported originates there too.

Two options:

1. **The browser posts decodes and QSOs back to the Tab5.** Keeps every upload
   path and the SD archive working unchanged. Adds an API surface. Much less
   work.
2. **All of it moves to the PC.** The Tab5 stops being the system of record.

Option 1, and not only because it is lighter: WSPR TX, PSK Reporter and Cloudlog
upload are supposed to run unattended. If they only work while a browser is
open, streaming mode is a downgrade rather than a second personality.

## Two safety points

- **The web server must run in both modes.** It does today. That is what makes
  the mode switch safe — there is never no control path, and the device can
  always be sent back to standalone from the browser.
- **Switching to standalone while remote must be hard to do by accident.**
  Dropping the I/Q stream is recoverable from the web UI; a mode that also parks
  WiFi is not. Note that `gpio_relay.c` exists for exactly this operator —
  Randy N4OPI's remote power-cycle after a firmware upgrade. Same failure mode,
  same audience.

## Suggested order

1. Measure sustainable websocket throughput and jitter with everything else
   stopped. Everything depends on this.
2. Decide the system-of-record question, because it shapes the API.
3. Port one decoder to WASM against its existing harness as a proof.

## What was considered and rejected

- **Streaming I/Q while the normal UI still runs.** The device is already at
  0.0-0.2% idle on core 0 with the radio streaming, and an open browser already
  breaks the audio. Adding a continuous stream to that path makes it worse.
  Moving DSP to the browser does not move the *transport* off core 0.
- **A hybrid: keep the FFT on device, stream only a downconverted passband**
  (NCO shift, reuse the existing decimate-by-8, ~192 kbit/s). Technically the
  cheapest option and it preserves the full panadapter view. Rejected as a
  *destination* because it leaves the decoders on the Tab5 and so does not
  deliver the remote-operation product — but it is a sound fallback if the
  throughput measurement comes back bad, and it is worth keeping in mind for
  that reason.
