# Web-UI audio streaming + server mode — feasibility & design

Scoping pass for Sam W7STF's request (email, 2026-07-15): demodulated receiver audio at
the **web UI** (SWL from the deck, radio + Tab5 stay in the office), plus a "server mode"
where the Tab5 screen is off and the device just serves. Written against the actual code
at `a4d8564` (post-v0.21.0). No code written yet — this is the "is it possible now, and
how" answer, superseding the earlier "no headroom" verdict given when CW audio was shelved.

## TL;DR

**Yes-with-caveats, and much cheaper than it sounds — the demodulator already exists.**
The FT8 capture front-end in `dsp.c` (`fft_task`'s FT8 branch) is, verbatim, a USB
demodulator: fs/4 real-mix of the +12 kHz IF to baseband (sign-flip trick, zero
multiplies), 31-tap 0–3 kHz FIR, /4 decimation to 12 kHz real audio samples, running
continuously into a PSRAM ring. It has been field-proven for a month as the thing FT8
decodes from. Web audio for USB/DiGi is: run that same branch in panadapter mode, µ-law
encode 1200 samples per 100 ms tick, and send them as a second binary frame type on the
**existing** `/ws` WebSocket from the **existing** `ws_push_task` — no new task, no I2S,
no codec, none of the hardware half of the shelved CW-audio problem.

- **Bandwidth**: µ-law @ 12 kHz = 96 kbps, on top of the ~82 kbps spectrum stream.
  Modest, but the SDIO→C6 WiFi link is the scarce resource on this device, not CPU —
  the release gate is a multi-hour streaming soak, not more DSP work.
- **The browser does the luxury features**: playback via a tiny AudioWorklet; Sam's
  Digital Noise Reduction wish runs client-side (RNNoise-class WASM) on the demodulated
  audio regardless of where demod happens — it never needs to fit on the P4.
- **Server mode** (screen off, keep serving) is display sleep (#34, **not yet built** —
  only the backlight primitives exist in `display.c`) plus extending the Tier 1 render
  gate (#39) to gate on "backlight off", both straightforward. It rides along; it is not
  a prerequisite for audio.
- **A compile-time server-only firmware fork is rejected** — permanent maintenance tax,
  and runtime server mode delivers the same thing.

**Estimate: 2–3 sessions** for Phase 1 (USB/DiGi audio at the web UI, panadapter mode
only) + a soak gate. Phases 2+ (LSB/CW/AM, opposite-sideband rejection, server mode,
client-side DNR) are each small and independent.

---

## What Sam actually asked for (three separable things)

1. **Demodulated audio at the web UI** — the core wish. SWL/CW copy anywhere on the
   home WiFi without relocating radio, PSU, speaker, cables.
2. **Server mode** — Tab5 screen off, device serves the web UI only.
3. Failing those, **a compile-time server-only firmware variant**.

Plus a parenthetical: a diagnostics view of CPU/heap — which already exists (dev-only
resource-monitor overlay + `POST /api/cmd {resmon}` / Ctrl+Alt+R in the web UI, and
per-core idle% in the diag log every 10 s since #39).

## Why the earlier "no" no longer holds

The refusal was calibrated against the **local playback** problem (shelved CW audio, #6):
I2S/ES8388 codec bring-up, DMA contention with the USB host, and a priority-6 ghost task
that trampled `fft_task` for four releases. Web audio shares none of that anatomy:

| CW audio (shelved #6) | Web audio (this design) |
|---|---|
| I2S + ES8388 codec bring-up | none — bytes over the existing WebSocket |
| DMA channel contention w/ USB host | no new DMA users |
| New always-on high-priority task | no new task at all (rides `ws_push_task`, prio 3) |
| Demod DSP written from scratch | demod front-end already shipped (FT8 pre-ring) |

And the platform itself moved:

- **Tier 1 render gate (#39)** proved mode-gated rendering frees the CPU wholesale
  (core-0 idle ~14–35% → ~70–80% in FT8 mode). The machinery for "don't render what
  isn't visible" exists and is hardware-verified.
- **Internal-RAM Tetris substantially won** (task stacks / LVGL pool / USB-DWC / mbedtls
  all moved to PSRAM); idle internal heap went from ~14 KB to 30–39 KB free.
- **The esp_hosted permanent WiFi wedge is fixed and self-healing** (v0.20.0 SDIO
  drain-and-resync recovery, hardware-verified) — the thing that would have made a
  continuous stream a support nightmare.

**Correction to an earlier internal note**: display sleep (#34) has *not* shipped —
`display.c` has `display_set_backlight()` / `display_fade_in_backlight()` primitives
only. Server mode therefore includes building #34, which was already an open request
(Samuel, groups.io #173236) with its own battery-life justification.

---

## Existing machinery inventory (what we reuse)

### 1. The FT8 pre-ring IS a USB demodulator — `dsp.c`

`fft_task`'s FT8 branch ([dsp.c:830-880](../main/dsp/dsp.c)) does, every 1024-sample
window, unconditionally while in FT8 mode:

```c
// fs/4 real mix: Re{ x[n] · e^{-jπn/2} } — the +12 kHz IF lands at DC.
// Interleave trick: +I, +Q, -I, -Q — zero multiplies.
s_ft8_mix_buf[i+0] =  I[i+0];   s_ft8_mix_buf[i+1] =  Q[i+1];
s_ft8_mix_buf[i+2] = -I[i+2];   s_ft8_mix_buf[i+3] = -Q[i+3];
// 31-tap FIR (0-3 kHz flat, -65 dB at the /4 aliasing edge), decimate 1024→256
dsps_fird_f32(&s_ft8_fir, s_ft8_mix_buf, s_ft8_dec_buf, DSP_FFT_SIZE / 4);
// append to s_ft8_pre — 15 s PSRAM float ring @ 12 kHz
```

Taking the real part of the downshifted complex signal *is* SSB (USB) demodulation of
whatever sits in the 0–3 kHz audio passband above the VFO. This is exactly what ft8_lib
decodes from — a month of field QSOs is the proof it's clean audio.

**Known limitation carried into Phase 1**: a real mix folds the spectrum — signals in
the 3 kHz *below* the VFO (the opposite sideband) alias onto the audio. FT8 doesn't
care; an SWL listener on a crowded band will occasionally hear the mirror. Phase 2 fixes
this properly (see below). It is NOT the 48 kHz sample-rate ghost documented in the
v0.18.3 image-rejection dead end — that one is QMX-side and stays out of scope.

### 2. The WS push pipeline — `webserver_ws.c`

- Single client, last-connection-wins, binary frames, 2-byte header
  (`WS_FRAME_TYPE_SPECTRUM 0x01` + decim factor), 1026 B @ 10 fps ≈ **82 kbps** today.
- `ws_push_task` at **priority 3** (deliberately below `fft_task`'s 4 — the hazard class
  is documented), 100 ms `vTaskDelayUntil` cadence — the same cadence an audio stream
  needs (1200 samples per tick @ 12 kHz).
- Transfer-pause plumbing (`webserver_ws_set_paused`) and fail-streak/backpressure
  handling (EAGAIN tolerance, wall-clock backstop, explicit stale-socket close) already
  battle-tested. Audio inherits all of it by riding the same task.

### 3. Zoom-FFT mix/decimate chain — `dsp.c`

A second, arbitrary-center complex mix + LPF + decimate path already exists for zoom.
If the audio passband ever needs to follow an off-VFO center (CW pitch, passband tuning),
the pattern is proven in-tree.

### 4. Render gate (#39) + backlight primitives

`render_task` already skips all canvas work when the FT8 screen covers the panadapter.
Server mode = the same skip, keyed on "backlight off", plus #34's idle timer and wake
gesture. LVGL provides `lv_display_get_inactive_time()` for the idle side.

---

## Design

### Producer (dsp.c) — Phase 1

Run the existing FT8 mix/FIR/decimate branch in **panadapter mode too**, writing the
same 12 kHz PSRAM ring (rename `s_ft8_pre` → audio pre-ring; it already has exactly the
right shape). Gate it on "web audio session active" so the panadapter path pays zero
cost when nobody listens. The branch's measured cost is known-affordable: it already
runs alongside capture+decode+render in FT8 mode, and panadapter-mode core-0 has far
more slack post-#39.

Mode handling at Phase 1: **USB and DiGi only** (the fs/4 mix is sideband-correct for
both). LSB/CW/AM are Phase 2.

### Encoding + framing — Phase 1

New frame type on the existing socket, emitted from the existing task loop:

```
byte 0: 0x02 (WS_FRAME_TYPE_AUDIO)
byte 1: flags (b0-b2: mode enum USB/LSB/CW/AM/DIGI, b3: squelched)
byte 2-3: uint16 LE sequence number (gap detection → browser inserts silence)
byte 4..: 1200 × µ-law bytes (100 ms @ 12 kHz)
```

µ-law encode is a 256-entry table lookup per sample — negligible. 96 kbps payload.
No Opus, no ADPCM in Phase 1: µ-law is zero-state (a lost frame corrupts nothing),
trivially decoded in JS, and the LAN can afford 96 kbps. If bandwidth ever matters
(remote access), IMA-ADPCM halves it as a Phase-3 option.

Frames are only built/sent when the client has sent a small "audio on" WS text message
(`{"audio":1}`) — the spectrum-only default keeps today's exact behaviour and bandwidth.

### Browser player — Phase 1

`index.html`: µ-law → Float32 decode table + an **AudioWorklet** ring buffer (~300 ms
jitter depth, sequence-gap → zeroes), volume slider, mute button. WebAudio requires a
user gesture to start — the mute/unmute button doubles as that gesture. ~150 lines of JS.

Client-side is also where Sam's **DNR** belongs, in every variant: RNNoise-class WASM on
the demodulated stream, a later drop-in that never costs the P4 anything.

### Phase 2 — modes + opposite-sideband rejection

- **LSB**: conjugate the baseband before the mix (negate Q in the interleave — still
  zero multiplies), same filter.
- **CW**: mix center = IF + `cat_get_cw_offset_hz()` (the WS spectrum path already does
  this for display centering); narrower FIR optional.
- **AM** (QMX 1_04+): envelope `sqrtf(I²+Q²)` on the decimated complex — needs the
  complex (not real-part) variant of the mix, see next point.
- **Opposite-sideband rejection**: switch the mix to complex (keep I' and Q' after the
  fs/4 shift — still sign-flips only), run the same 31-tap FIR on both rails, and do a
  phasing (Hilbert) combine at 12 kHz. Cost ≈ 2× the current FIR + a short Hilbert FIR —
  still far below the FFT's cost. This removes the Phase-1 mirror caveat and gives real
  SSB quality (~40+ dB rejection with a 31–63-tap Hilbert).

### Phase 2/3 — server mode (screen off)

1. Build **#34 display sleep** as specced (idle timeout → backlight off, double-tap
   wake, two-finger double-tap immediate off) — independently requested, independently
   shippable.
2. Extend the #39 render gate: when the backlight is off, `render_task` skips canvas
   work in panadapter mode too (spectrum data for the WS stream comes from `dsp.c`, not
   from the render pipeline, so the web UI is unaffected — same separation the FT8-mode
   gate already proved).
3. Keep USB/CAT/audio/WS fully alive. That's the whole feature.

### Explicitly rejected: compile-time server-only firmware

Two firmwares = two test matrices, two release artifacts, drift forever. Runtime server
mode gives the same power/CPU win with one binary. Also rejected: streaming raw/decimated
IQ to the browser as the *primary* path (client-side demod) — it's elegant and stays on
the table as a Phase-3 option for passband-tuning UX, but device-side µ-law is simpler,
works on any browser/CPU, and client-side DNR doesn't need it.

---

## Resource budget & house rules

| Resource | Cost | Verdict |
|---|---|---|
| CPU (core 0/1) | mix (sign flips) + 31-tap FIR/4 + µ-law table — same branch FT8 mode runs today | affordable, measured in the field |
| Internal RAM | ~0 — ring in PSRAM, µ-law table const, frame buffer is static like `s_payload` | follows the sub-16 KB-malloc rule by having no mallocs |
| PSRAM | 720 KB ring already budgeted in FT8 mode; audio can shrink it (2 s is plenty) | fine |
| WiFi | +96 kbps sustained on the SDIO→C6 link (~2.2× today's spectrum stream) | **the actual risk — soak-gated** |

House rules this design already conforms to (all learned the hard way, see CLAUDE.md):

- No new task; the one task touched stays priority 3 < `fft_task`'s 4.
- No `malloc()` on any hot/periodic path; buffers static or PSRAM-at-init.
- No full heap/stack walks anywhere periodic (cyan-flash rule) — nothing here walks.
- Audio frames obey the existing transfer-pause (`webserver_ws_set_paused`) so
  QRZ/eQSL uploads still get the SDIO link to themselves.
- FT8/FT4 mode keeps the web stream paused (v0.20.0 gate) — **web audio is
  panadapter-mode only**, which matches the use case (SWL/CW/SSB listening).

## Risks & release gates

1. **WiFi link duty cycle (the real gate).** The SDIO wedge is fixed *and self-healing*,
   but a sustained ~180 kbps combined stream is a duty cycle it has never soaked under.
   Gate: multi-hour (target: overnight) streaming soak with the diag log watching for
   `SDIO RX oversize ... draining to recover` frequency — recoveries must stay rare and
   isolated, not accelerate (the drift-vs-coalesce discriminator documented in
   CLAUDE.md).
2. **`TCP_SND_BUF` was deliberately shrunk** 11520→2880 (v0.20.0) for internal-DMA-heap
   headroom. 1204-byte audio frames at 10 Hz may want it back up a notch; any change
   re-opens the heap-headroom trade-off and must be re-soaked. Try 2880 first — frames
   fit under it.
3. **Half-open-socket stalls**: `httpd_ws_send_frame_async` can block seconds on a
   stale socket (documented in `webserver_ws.c`); with audio riding the same task, a
   stall now also mutes audio. Acceptable (same session is dead either way); the
   wall-clock backstop already bounds it.
4. **Browser autoplay policy**: audio cannot start without a user gesture — design the
   UI so unmute is the gesture, not a surprise.

## Phased plan

| Phase | Contents | Effort | Gate |
|---|---|---|---|
| 1 | USB/DiGi audio: pre-ring in panadapter mode, µ-law WS frame 0x02, AudioWorklet player, on/off in web UI | 1–2 sessions | overnight streaming soak |
| 2a | LSB/CW mix variants; CW offset centering | small | on-air listen check |
| 2b | Phasing demod (complex mix + Hilbert) → opposite-sideband rejection; AM envelope (1_04) | 1 session | A/B vs Phase-1 audio |
| 2c | Server mode: #34 display sleep + render-gate-on-backlight-off | 1 session | battery + soak |
| 3 | Optional: client-side DNR (WASM), IMA-ADPCM low-bandwidth mode, IQ-slice streaming + browser demod/passband tuning | as demanded | — |

Phase 1 and #34 are independent and can ship in either order.

## Relationship to existing TODO items

- **#6 (CW audio, shelved) / #7 (speaker/headphone audio)**: unchanged and NOT
  unblocked by this — local playback still needs the I2S/DMA root-cause work. But
  Phase 1 here delivers most of #7's *user value* (hearing the radio) via the browser
  without touching I2S. If #6 is ever revived, the Phase-2 phasing demod is shared work.
- **#34 (display sleep)**: becomes a sub-part of server mode; unchanged as its own item.
- **#39 (render gate)**: Tier 2 (native blit) trigger list gains nothing here — server
  mode *skips* rendering rather than needing it faster.
- **#11 (DSP polish / noise reduction)**: the NR half is better served client-side per
  this design; auto-notch remains a device-side idea.
