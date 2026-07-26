# Panadapter

The panadapter is your primary view — a real-time spectrum analyser and waterfall for the QMX.

### 1. Layout

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
│ Battery 95%  FPS: 30   SD  v1.1.0   Waterfall         │  Status bar (30px)
└─────────────────────────────────────────────────────────┘
```

**Status bar indicators:**

| Item | Meaning |
|------|---------|
| Battery % | Current battery charge level |
| FPS | Render frame rate (target 30 fps) |
| **SD** (green dot) | A microSD card is mounted and being mirrored **continuously** — the case with WiFi off (see [Settings](../guide/settings.md)) |
| **SD** (yellow dot) | A card is present and your start-up backup is written, but continuous mirroring has stopped — the case with WiFi on. Your log, config and LoTW certificate are safe on the card; later QSOs are written at the next start-up |
| `UTC(GPS)` / `UTC(NTP)` / `UTC(FT8)` / `UTC(FT4)` | The time source **currently** in charge — GPS-disciplined QMX, WiFi/SNTP, or FT8/FT4 offline sync (RTC / manual show as `UTC(RTC)` / `UTC(MAN)`) |
| Firmware version | Currently flashed firmware version |
| Waterfall label | Active colour map name |

### 2. Touch Controls

#### Tap to Tune

Tap anywhere on the spectrum or waterfall to jump to that frequency. The panadapter snaps to a frequency grid (resolution depends on zoom level) for easy tuning.

- **Zoom 1.0x** — 10 kHz snap
- **Zoom 2.0x** — 5 kHz snap
- **Zoom 4.0x** — 2.5 kHz snap
- **Zoom 8.0x** — 1 kHz snap

#### Pinch to Zoom

Use two fingers to pinch in (zoom out) or pinch out (zoom in). The display centers on the passband width — in USB/LSB modes, the passband center stays on screen even when the VFO is off to the side.

#### Pan (One-Finger Drag)

Drag horizontally with one finger to scroll the spectrum left/right. Release to tune to the new center frequency.

#### Band-Plan Strip

Along the bottom of the screen — just above the status bar, below the waterfall — is a thin coloured strip showing where you are within the band at a glance. Each colour zone marks the conventional segment (CW, Digi, Phone) for your selected region (auto / ITU Region 1/2/3, set in settings).

The **visible-span block** inside the strip shows the exact portion of the band the spectrum and waterfall are currently displaying — it narrows as you zoom in and shifts as you pan.

A **passband sub-block** inside the visible-span block mirrors the current filter width at band scale (grey tint), so you can see how your passband sits within the CW/Digi/Phone zones.

**Tune directly from the strip:**

- **Tap** anywhere on the strip to jump to that frequency
- **Drag** to scrub along the band — the frequency label updates live and the QMX retunes on release

**Drag from the bottom bar too.** The visible-span block acts as a slider handle that reaches *below* the thin strip: touch on or just under it — anywhere along the bottom status bar — and **drag sideways** to scrub the band, exactly like dragging the strip itself. This gives you a much taller grab target. It coexists with the memory-picker gesture on the same row: a **sideways** drag retunes the band-plan, while a **vertical up-swipe** still opens the memory picker.

The strip updates live as you zoom, pan, or change bands.

#### Memory Channels

Swipe ↑ from the bottom edge to open the memory picker — a 4×8 grid of 32 channels, each free to hold any frequency/mode (not tied to a band). Each channel stores:

- Frequency
- Mode (USB, LSB, CW, DiGi)
- Label (your own name for the channel)

Each button shows the mode colour (CW = green, DiGi = teal, USB = brick red, LSB = purple) so you can tell mode at a glance without reading the label.

**Recall:** Tap a filled channel to immediately retune to it.

**Tap an empty slot** to create a new channel there directly — the frequency keypad opens straight away.

**Long-press + drag a filled channel** to move it to a different empty slot — no editing needed, the data follows your finger. Release on an empty slot to drop.

**Long-press in place** to open the editor (change name, frequency, or mode).

Entering a frequency outside the legal amateur band edges is rejected immediately with an error — the pad stays open so you can correct it without starting over.

### 3. Top Bar

Tap any item to open its selector:

| Item | Purpose |
|---|---|
| **14.074** | Frequency. Tap to open the frequency keypad. |
| **USB** | Mode. Tap to cycle USB, LSB, CW, DiGi. |
| **2.5 kHz** | Bandwidth (SSB only). Tap to choose 2.5, 2.7, 2.9, or 3.2 kHz. |
| **S7** | S-meter. Shows received signal strength. Tap to reset peak hold. |
| **1.0x** | Zoom level. Tap to choose preset (1x, 2x, 4x, 8x) or custom. |

### 4. Spectrum & Waterfall

The **spectrum** shows real-time signal strength (green curve) across the tuned band. The **waterfall** shows a rolling history of that spectrum, with colour indicating signal strength (SDR gradient: blue → green → yellow → red).

**Flat Spectrum Mode** (in settings) shows the dB scale relative to a per-bin noise floor, so even weak signals pop above the baseline. Normal mode shows absolute dBm (referenced to a -73 dBm S9 mark).

**Waterfall Controls** (in settings):

- **Black level** — how far above noise-floor to go black (default 9 dB)
- **Contrast** — dB span of the colour ramp (default 45 dB)
- **Adaptive floor** — blend between per-bin and global noise floor (default 100%)
- **FFT window** — Blackman-Harris, Hann, or Nuttall (default Blackman-Harris)

### 5. Display & Buffer Features

#### Spectrum Buffer Clear

When you **switch modes** (Panadapter ↔ FT8 ↔ FT4) or **change bands**, the panadapter automatically **clears the waterfall and resets the spectrum baseline**. This prevents stale signals from interfering with your new band or mode view.


- Waterfall clears completely (starts fresh)
- Noise floor recalibrates
- New baseline takes ~1 second to establish

This happens transparently — you'll notice the waterfall momentarily clear and re-initialize, then populate with real-time data from the new band/mode.

#### Flat Spectrum Mode

Toggle in settings. Shows dB scale relative to a **per-bin noise floor** (not absolute dBm), so weak signals stand out above the baseline even on a noisy band.

- **Normal mode**: absolute dBm referenced to S9 = -73 dBm
- **Flat mode**: relative dB above each bin's noise floor

### 6. Frequency Keypad

Tap the **frequency** on the top bar to open the keypad. Enter frequency in MHz format:

- **14.074** for 14.074 MHz
- **1.832** for 1.832 MHz
- **.100** for relative tune (±100 Hz from current)

Layout switches between **10-Key** (phone dial) and **Phone** (QWERTY) via a toggle. Choose whichever is faster for you — the preference persists.

**Resize the keypad:** Pinch it or swipe up/down on it to toggle between the normal and a compact layout. Your choice is remembered across reboots. The compact layout reveals more of the spectrum behind it.

**Reposition the keypad:** Drag it by the **"Enter freq"** title label (the only non-button area at the top of the panel) to move it anywhere on screen, clamped to stay fully visible. The position is remembered so it reopens exactly where you left it.

The background behind the keypad is intentionally semi-transparent (40% dim) so the spectrum and waterfall stay visible while you're entering a frequency.

**Cancel and Enter are the only exits** — tapping outside the keypad does nothing, so an accidental background touch cannot silently discard what you were typing.

### 7. Memory Channels

32 memory channels (4×8 grid), free to hold any frequency/mode — not tied to a band. Each stores frequency, mode, and name. Swipe up from the bottom edge to open the memory picker.

Mode is shown in colour on each button — CW (green), DiGi (teal), USB (brick red), LSB (purple) — so the grid is scannable at a glance without reading every label.

**To recall a channel:** Tap it. The QMX retunes immediately.

**To create a new channel (empty slot):** Tap the empty slot — the frequency keypad opens directly. No long-press needed.

**To edit an existing channel:** Long-press it in place. Change the name, frequency, or mode, then tap **Save**.

**To move a channel to a different slot:** Long-press and drag it to any empty slot. Release to drop. Only the data moves — the grid positions stay fixed.

**To save the current QMX frequency/mode to a slot:**
1. Swipe ↑ to open the memory picker
2. Long-press the slot you want to overwrite
3. The current frequency/mode pre-fills the editor — adjust the label if you like, then tap **Save**

**To delete a channel — drag it onto the wastebin:** the bottom-right cell (channel 32) is a **wastebin**. Long-press any filled channel and drag it onto the wastebin to delete it (it fades out). This is the way to clear a slot you no longer want.

**Example channels on first use.** A brand-new device ships with a few example channels already filled in (rather than 32 blank cells), so the grid is explorable straight away. These only seed empty slots — they never overwrite anything you've saved.

**First-run tour.** The very first time you open the memory picker, a brief (~10-second) one-time animation shows that channels can be **dragged** to move them and **dropped on the wastebin** to delete them. It plays once and never again.

**Out-of-band frequencies are rejected immediately** — if you enter a frequency outside a recognised amateur band, the keypad stays open so you can correct it rather than silently saving a wrong value.

### 8. Band Presets

Tap the **band name** on the top bar to switch between configured bands. The band selector shows:

- **160m, 80m, 60m, 40m, 30m, 20m, 17m, 15m, 12m, 10m, 6m** — standard bands (whichever your radio has configured)
- **11m** — the CB segment (26.9–27.5 MHz), shown on QMX+ radios that expose it
- **Custom** — user-added bands (via settings)

On radios with many configured bands (QMX+), the picker lays the bands out in **two side-by-side columns** so every band is visible without scrolling. Selecting a band jumps to the **nearest** band centre, so closely-spaced bands like 10 m and 11 m are no longer confused for one another.

Switching bands **remembers the last frequency you visited on each band**, so you can flip between 20m and 40m without losing your place.

### 9. Zoom & Pan

The zoom level is displayed on the top bar (e.g., **2.0x**). Tap it to choose a preset:

- **1.0x** — full 4 MHz span (standard panadapter view)
- **2.0x** — 2 MHz span (half-band view)
- **4.0x** — 1 MHz span (detailed search)
- **8.0x** — 500 kHz span (CW pile-up detail)
- **Custom** — (coming in a future release)

At zoom levels above 1.0x, the display **pans to keep the passband centered** when you change mode or bandwidth, so the active receive area stays on screen.

### 10. S-Meter

The S-meter (top-right of the top bar) shows received signal strength on a 0–68 scale:

- **S1 to S9** — standard S-units (-130 to -73 dBm)
- **+10 to +20** — above S9 in 10 dB steps

Tap the S-meter to toggle peak-hold mode (shows the strongest signal heard in the last ~5 seconds). Tap again to reset.

### 11. Settings Drawer

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
