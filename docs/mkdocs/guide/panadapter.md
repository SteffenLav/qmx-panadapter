# Panadapter

The panadapter is your primary view — a real-time spectrum analyser and waterfall for the QMX.

## Layout

```
┌─────────────────────────────────────────────────────────┐
│ 14.074 | USB | 2.5 kHz | S7 | 1.0x        [⚙️ settings] │  Top bar (60px)
├─────────────────────────────────────────────────────────┤
│                    ╱╲                                    │  Spectrum (200px)
│           ____╱╲_╱  ╲____                                │  Green trace,
│        ╱╲╱                ╲                               │  amber VFO,
│ ┼─────┼─────────────────────────────┼                     │  grey passband
├─ 14.0  14.05  14.10  14.15  14.20 ─┤  Frequency axis    │
│                                     │  (18px)            │
│ ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │  Waterfall        │
│ ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │  (412px)          │
│ ░░░░░▓▓▓░░░░░░░░░░░░░░▓▓░░░░░░░░░░ │  Newest at top    │
│ ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │  SDR gradient     │
├─────────────────────────────────────────────────────────┤
│ Battery 95%  FPS: 30              Waterfall            │  Status bar (30px)
└─────────────────────────────────────────────────────────┘
```

## Touch Controls

### Tap to Tune

Tap anywhere on the spectrum or waterfall to jump to that frequency. The panadapter snaps to a frequency grid (resolution depends on zoom level) for easy tuning.

- **Zoom 1.0x** — 10 kHz snap
- **Zoom 2.0x** — 5 kHz snap
- **Zoom 4.0x** — 2.5 kHz snap
- **Zoom 8.0x** — 1 kHz snap

### Pinch to Zoom

Use two fingers to pinch in (zoom out) or pinch out (zoom in). The display centers on the passband width — in USB/LSB modes, the passband center stays on screen even when the VFO is off to the side.

### Pan (One-Finger Drag)

Drag horizontally with one finger to scroll the spectrum left/right. Release to tune to the new center frequency.

### Memory Channels

Swipe ↑ from the bottom edge to open the memory picker. Each of your 10 per-band memory channels stores:

- Frequency
- Mode (USB, LSB, CW, DiGi)
- Bandwidth
- Last tuned time

Tap a channel to recall it. Long-press to **edit** (change name, frequency, or mode). Tap **Save** to store the current frequency/mode to a channel.

## Top Bar

Tap any item to open its selector:

| Item | Purpose |
|---|---|
| **14.074** | Frequency. Tap to open the frequency keypad. |
| **USB** | Mode. Tap to cycle USB, LSB, CW, DiGi. |
| **2.5 kHz** | Bandwidth (SSB only). Tap to choose 2.5, 2.7, 2.9, or 3.2 kHz. |
| **S7** | S-meter. Shows received signal strength. Tap to reset peak hold. |
| **1.0x** | Zoom level. Tap to choose preset (1x, 2x, 4x, 8x) or custom. |

## Spectrum & Waterfall

The **spectrum** shows real-time signal strength (green curve) across the tuned band. The **waterfall** shows a rolling history of that spectrum, with colour indicating signal strength (SDR gradient: blue → green → yellow → red).

**Flat Spectrum Mode** (in settings) shows the dB scale relative to a per-bin noise floor, so even weak signals pop above the baseline. Normal mode shows absolute dBm (referenced to a -73 dBm S9 mark).

**Waterfall Controls** (in settings):

- **Black level** — how far above noise-floor to go black (default 9 dB)
- **Contrast** — dB span of the colour ramp (default 45 dB)
- **Adaptive floor** — blend between per-bin and global noise floor (default 100%)
- **FFT window** — Blackman-Harris, Hann, or Nuttall (default Blackman-Harris)

## Frequency Keypad

Tap the **frequency** on the top bar to open the keypad. Enter frequency in MHz format:

- **14.074** for 14.074 MHz
- **1.832** for 1.832 MHz
- **.100** for relative tune (±100 Hz from current)

Layout switches between **10-Key** (phone dial) and **Phone** (QWERTY) via a toggle. Choose whichever is faster for you — the preference persists.

## Memory Channels

10 memory channels per band. Each stores frequency, mode, and name. Swipe up from the bottom edge to open the memory picker, or tap a memory channel name to recall it.

**To save the current frequency:**
1. Tune to the desired frequency
2. Swipe ↑ to open memory picker
3. Tap an empty or unwanted channel
4. Tap **Save** to overwrite it

**To edit a channel name:**
1. Swipe ↑ to open memory picker
2. Long-press a channel
3. Type a new name
4. Tap **Save**

## Band Presets

Tap the **band name** on the top bar to switch between configured bands. The band selector shows:

- **160m, 80m, 40m, 20m, 17m, 15m, 12m, 10m** — standard bands
- **Custom** — user-added bands (via settings)

Switching bands **remembers the last frequency you visited on each band**, so you can flip between 20m and 40m without losing your place.

## Zoom & Pan

The zoom level is displayed on the top bar (e.g., **2.0x**). Tap it to choose a preset:

- **1.0x** — full 4 MHz span (standard panadapter view)
- **2.0x** — 2 MHz span (half-band view)
- **4.0x** — 1 MHz span (detailed search)
- **8.0x** — 500 kHz span (CW pile-up detail)
- **Custom** — (coming in a future release)

At zoom levels above 1.0x, the display **pans to keep the passband centered** when you change mode or bandwidth, so the active receive area stays on screen.

## S-Meter

The S-meter (top-right of the top bar) shows received signal strength on a 0–68 scale:

- **S1 to S9** — standard S-units (-130 to -73 dBm)
- **+10 to +20** — above S9 in 10 dB steps

Tap the S-meter to toggle peak-hold mode (shows the strongest signal heard in the last ~5 seconds). Tap again to reset.

## Settings Drawer

Swipe ← from the right edge to open the settings drawer. Common panadapter controls:

- **IQ Balance** — adaptive I/Q correction (usually on)
- **Flat Spectrum** — normalise to per-bin noise floor
- **Waterfall** — black level, contrast, adaptive floor, FFT window
- **WiFi** — on/off, SSID, password
- **Time Sync** — SNTP, manual time set, FT8-derived sync
- **Display** — 180° flip, brightness
- **About** — firmware version, reset to defaults

---

**Next:** Learn about [FT8 Receive](ft8-rx.md) or [Web UI](web-ui.md).
