# Panadapter

The panadapter is your primary view — a real-time spectrum analyser and waterfall for the QMX.

### 1. Layout

```qmxdiagram
type: stack
title: The panadapter screen, top to bottom
row: 60 Top bar | frequency, mode, filter width, S-meter, zoom
row: 200 Spectrum | green trace, amber VFO cursor, tinted passband
row: 32 Frequency axis | absolute MHz labels
row: 370 Waterfall | newest row at the top, SDR gradient
row: 22 Band-plan strip | CW / Digi / Phone segments, with the visible span marked
row: 36 Status bar | battery, UTC clock, WiFi
note: amber | the VFO cursor sits at the dial frequency; in USB the passband tint runs upward from it, in LSB downward
note: dim | in a digital mode the tint spans 150-3200 Hz, the QMX's one fixed digital filter (corrected in v1.8.3)
note: dim | the top-right 200 x 120 of the spectrum is a deadzone, so a tap near the drawer grip cannot retune you
```


**Status bar indicators:**

Left to right: battery, the SD dot, the firmware version, the UTC clock, and the
WiFi indicator with the network name and IP address.

| Item | Meaning |
|------|---------|
| Battery % and voltage | Charge level, with the measured pack voltage beside it (e.g. `88% (8.1V)`) |
| **SD** (green dot) | A microSD card is mounted and being mirrored **continuously** — the case with WiFi off (see [Settings](../guide/settings.md)) |
| **SD** (yellow dot) | A card is present and your start-up backup is written, but continuous mirroring has stopped — the case with WiFi on. Your log, config and LoTW certificate are safe on the card; later QSOs are written at the next start-up |
| `UTC(GPS)` / `UTC(NTP)` / `UTC(FT8)` / `UTC(FT4)` | The time source **currently** in charge — GPS-disciplined QMX, WiFi/SNTP, or FT8/FT4 offline sync (RTC / manual show as `UTC(RTC)` / `UTC(MAN)`) |
| Firmware version | Currently flashed firmware version |
| WiFi fan icon | Link strength, at the right-hand end with the network name and IP address. The dot alone means a weak link (above 25 %), plus the first bow above 50 %, plus the second above 80 %; all three faint means connected but very weak, or `off`. From v1.3.4 this replaces the old `-NN dBm` figure — the width went to the network name, which is far more often too long to fit |

### 2. Touch Controls

#### Tap to Tune

Tap anywhere on the spectrum or waterfall to jump to that frequency. The tap snaps to a frequency grid so you land on a sensible frequency rather than wherever your fingertip happened to be.

With a mouse the rule is simply the colour of the pointer: **white means clicking tunes,
green means you are over something that will act instead** — a callsign, a top-bar control,
an edge handle. Press and hold, or click and hold, and drag if you want to place the cursor
before committing; a finger and a pointer behave identically.

!!! note "Fixed in v1.8.1"
    Before v1.8.1 a band across the upper part of the spectrum quietly refused to tune —
    the touch areas behind the top bar had been made shallower without the tune code being
    told, so that strip belonged to nobody. It looked as though tuning only worked near the
    centre frequency, while the waterfall was fine.

> Callsigns from [Live Spots](spots.md) are drawn over the spectrum and are the
> one exception: tapping a **callsign** tunes to that station and sets the mode,
> rather than tuning to the point you touched.

The snap step follows the **mode**, not the zoom level — a CW signal needs to be found to the hertz, an FT8 dial frequency does not:

| Mode | Snap step |
|------|-----------|
| CW / CW-R | 10 Hz |
| USB / LSB | 250 Hz |
| DiGi / FT8 / FT4 / RTTY | 500 Hz |

!!! note "In CW the display allows for your CW offset — including when you change it"
    A CW signal you can hear at 700 Hz is not on the dial frequency, so the
    spectrum, the waterfall and tap-to-tune all allow for the radio's **CW
    offset**. That figure is read from the radio, and since v1.8.5 it is
    **re-read every few seconds while you are in CW**, so changing CW offset or
    CW centre on the radio itself is followed within a few seconds.

    Before that it was read only once when the Tab5 connected, so if you changed
    it afterwards the display kept compensating by the old value and tapping a
    signal tuned you slightly off frequency — you would then transmit off
    frequency as though XIT were on. Reported by Roy KI0ER.
| AM / FM | 1 kHz |

#### RIT — receiving off your transmit frequency

Someone answering your call a couple of hundred hertz off your frequency is the ordinary
case this is for: you want to hear them without moving your own transmit frequency, which
everyone else is listening on.

1. Tap **RIT** at the top right of the spectrum. It turns amber and reads *click a signal*
   (in the browser) or waits for a tap.
2. Tap the caller on the spectrum or waterfall. The dial does **not** move. A dashed
   magenta line appears where you are now listening, in the spectrum and down the
   waterfall, and the filter window moves onto them.
3. It stays armed, so the next caller is another tap.
4. Tap **RIT** again to clear the offset and switch it off.

The offset is also printed beside its own marker over the waterfall, as `+250 Hz`,
so you can read how far off you are listening without looking at the corner.

**Long-press RIT to park the offset instead of clearing it.** The offset is
remembered and RIT switches off; long-press again and it comes back unchanged. The
button reads `RIT (+250)` in brackets while an offset is parked. This is for a round
robin or a net where one station is off frequency: you drop the offset when the turn
passes back to the stations who are on frequency, and pick it up again without
re-dialling anything (suggested by Roy KI0ER). A parked offset is discarded when you
retune — it belonged to that one station.

If you never use RIT, the button can be hidden: **Settings → Radio → Show RIT button**
(also in the web settings). It is shown by default. If an offset is actually engaged the
button appears regardless of that setting — the radio listening away from your dial is not
something to leave unsaid on screen.

The gold line goes on marking the **dial** — which is where you transmit — so seeing both
lines at once tells you transmit has not followed your receiver. Tuning anywhere by any
means clears RIT, on the assumption that you have moved on.

While RIT is armed, taps use a 10 Hz grid whatever the mode. The normal grid above exists
to land the dial on a tidy frequency; RIT is a fine adjustment onto one caller's tone, and
SSB's 250 Hz step would leave only a handful of usable offsets. The range is ±500 Hz —
tapping further away clamps to the limit and says so.

Available in the browser too, where a click does the same thing. The offset itself is
shared, so setting it on either screen shows on both; the *armed* state is per-screen,
because arming changes what a click does and a click in a browser is not a finger on the
Tab5.

The grid is anchored to absolute frequency, not to where your finger first touched, so the cursor always lands on the same set of points no matter where the drag began.

#### Pinch to Zoom

Use two fingers to pinch in (zoom out) or pinch out (zoom in). The display centers on the passband width — in USB/LSB modes, the passband center stays on screen even when the VFO is off to the side.

!!! note "The darker band at each end of a zoomed view (improved in v1.8.3)"

    Zooming filters the signal before it is re-analysed, and that filter starts
    rolling off just inside the edge of what is drawn — so the outer part of a
    zoomed view is slightly attenuated. It is a property of the filter, not a
    fault, and it has behaved this way since zoom was added.

    v1.8.3 doubles the length of that filter, which roughly **halves the width of
    the darkened band** at each side. The alternative — widening the filter
    instead — was rejected deliberately: it would trade the dark edge for false
    signals appearing where nothing is transmitting. Measured and reported by
    Samuel W7STF, whose own estimate of the affected width was accurate.

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

**Outside a band**, the strip reads **"Out of band — drag to tune"** in one flat
colour instead of the CW/Digi/Phone zones, and returns to normal as soon as you are
back inside a band. The frequency marker is hidden while you are out, because there is
no band plan to position it against.

**Out of band the strip becomes a coarse tuner** (v1.8.3). Inside a band the strip is
a *map* — where you touch is the frequency you get. Outside one there is no map to
touch, so it works the other way round: a handle sits in the middle of the strip,
and you **drag it off centre to move the dial**. Let go and it springs back to the
middle, ready for the next pull.

- Dragging all the way to either edge moves by **half of what is currently on
  screen**, so two drags in the same direction overlap rather than skipping a gap.
  Nothing can scroll past unseen.
- It follows the zoom: zoomed in, the same drag is a finer step.
- A plain **tap** out of band does nothing on purpose — with no band plan behind it,
  a tapped position has no frequency to mean.

Suggested by Samuel W7STF, who pointed out that a row saying only "out of band"
earns nothing when `Band: ---` in the top-left already tells you that.

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
| **USB** | Mode. Tap to open the picker: USB, LSB, CW, DiGi — plus **AM** once the connected QMX reports 1.04 or newer firmware. |
| **2.5 kHz** | Bandwidth (SSB only). Tap to choose 2.5, 2.7, 2.9, or 3.2 kHz. |
| **S7** | S-meter. Shows received signal strength. Display only — there is nothing to tap. |
| **1.0x** | Zoom level. Tap to choose x1, x2, x4, x8, x16 or x24. Pinching sets any value in between. |

### 4. Spectrum & Waterfall

The **spectrum** shows signal power in dBm (default range −130 to −30 dBm) as a green curve with a dim fill. Each frame is smoothed per-bin with an exponential moving average — α = 0.4 by default, adjustable in the drawer — which balances a stable picture against a snappy response to CW keying and SSB attack transients.

The scale labels down the right-hand edge are **worked out from the dB range you set**
(v1.8.3): the firmware picks round values that fit inside your Min and Max, choosing a
finer step for a narrow range. Before this they were fixed at −40 to −120 regardless,
so any range other than the default was described by labels that did not belong to it
(found by Samuel W7STF running −118/−13). At the default range the labels are unchanged.

The **frequency axis** shows absolute MHz labels centred on the QMX VFO, refreshed on every CAT frequency update. At high zoom the labels resolve to kHz or Hz precision.

The **waterfall** runs newest row at the top, in a thermal SDR palette (black → dark blue → teal → green → yellow → red). Four colour maps are available in the drawer: **Thermal, Viridis, Turbo** and **Grayscale**.

**The waterfall floor tracks the band automatically.** Its black level follows a running median sampled only from bins *inside the passband*, EMA-smoothed, so the background colour follows conditions instead of sitting at a fixed anchor. Bins outside the passband are drawn darker and excluded from that calculation, so they cannot wash out dim in-band signals.

**Flat-spectrum mode** (drawer toggle, persisted) switches both spectrum and waterfall to a per-bin adaptive display: every bin renders as dB *above its own running noise floor*. Real signals — weak CW tones especially — stand out sharply against a calm baseline, which is why it is the mode to reach for on a noisy band. The dB-range sliders have no effect while it is on, and the axis shows relative dB above floor rather than absolute dBm.

**The picture is cleared when it would otherwise mislead.** Switching between Panadapter, FT8 and FT4, or changing band, clears the waterfall and resets the spectrum baseline, so stale signals from the old band cannot linger in the new one. The noise floor recalibrates and a new baseline settles in about a second.

**Waterfall controls** (in settings):

- **Black level** — how far above noise-floor to go black (default 9 dB)
- **Contrast** — dB span of the colour ramp (default 45 dB)
- **FFT window** — Blackman-Harris, Hann, or Nuttall (default Blackman-Harris)

!!! note "Adaptive floor was removed in v1.8.3"

    That slider could not change anything: the per-bin noise floor it blended
    towards is re-seeded many times a second, so both ends of it produced the
    same picture. It has been taken out of the drawer, as it already had been
    from the browser. The stored value is still kept and still exported with
    your configuration, so the control can come back the day the underlying
    floor tracking runs.

### 5. Frequency Keypad

Tap the **frequency** on the top bar to open the keypad. Enter frequency in MHz format:

- **14.074** for 14.074 MHz
- **1.832** for 1.832 MHz
- **14.074.250** for 14,074,250 Hz — a third group is hertz within the kilohertz

Each group after a `.` is padded to three digits, so `14.07` is 14.070 MHz, not 14.007. Type a number with **no `.` at all** and it is taken as plain hertz.

The digit layout toggles between **10 Key** (calculator order, 7-8-9 on the top row) and **Phone** (1-2-3 on the top row). Pick whichever your fingers already know — the choice persists across reboots.

**Resize the keypad:** Pinch it or swipe up/down on it to toggle between the normal and a compact layout. Your choice is remembered across reboots. The compact layout reveals more of the spectrum behind it.

**Reposition the keypad:** Drag it by the **"Enter freq"** title label (the only non-button area at the top of the panel) to move it anywhere on screen, clamped to stay fully visible. The position is remembered so it reopens exactly where you left it.

The background behind the keypad is intentionally semi-transparent (40% dim) so the spectrum and waterfall stay visible while you're entering a frequency.

**Cancel and Enter are the only exits** — tapping outside the keypad does nothing, so an accidental background touch cannot silently discard what you were typing.

### 6. Memory Channels

32 memory channels (4×8 grid), free to hold any frequency/mode — not tied to a band. Each stores frequency, mode, and name. Swipe up from the bottom edge to open the memory picker.

Each slot shows its **label in large text**, with the mode and frequency dimmed beneath. Mode is also colour-coded — CW (green), DiGi (teal), USB (brick red), LSB (purple) — so the grid is scannable at a glance without reading every label.

**To recall a channel:** Tap it. The QMX retunes immediately.

**To create a new channel (empty slot):** Tap the empty slot — the frequency/mode picker opens directly, pre-filled with the current VFO so you can adjust both before naming it. No long-press needed.

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

### 7. Band Presets

Tap the **band name** on the top bar to switch between configured bands. The band selector shows:

- **160m, 80m, 60m, 40m, 30m, 20m, 17m, 15m, 12m, 10m, 6m** — standard bands (whichever your radio has configured)
- **11m** — the CB segment (26.9–27.5 MHz), shown on QMX+ radios that expose it
- **Custom** — user-added bands (via settings)

On radios with many configured bands (QMX+), the picker lays the bands out in **two side-by-side columns** so every band is visible without scrolling. Selecting a band jumps to the **nearest** band centre, so closely-spaced bands like 10 m and 11 m are no longer confused for one another.

Switching bands **remembers the last frequency you visited on each band**, so you can flip between 20m and 40m without losing your place.

### 8. Zoom & Pan

The QMX sends a 48 kHz-wide slice of I/Q, so **×1 shows 48 kHz** centred on the dial. Zooming narrows that window with a true zoom-FFT — you get finer resolution, not a stretched picture.

The zoom level is displayed on the top bar (e.g. **2.0x**). Tap it to choose a preset:

| Zoom | Visible span |
|---|---|
| ×1 | 48 kHz — the whole slice |
| ×2 | 24 kHz |
| ×4 | 12 kHz |
| ×8 | 6 kHz — CW pile-up detail |
| ×16 | 3 kHz |
| ×24 | 2 kHz — individual CW signals well separated |

Pinching sets any value in between; double-tap returns to ×1 and re-centres.

| Gesture | Effect |
|---------|--------|
| One-finger fast horizontal swipe | Pan ("stroll") the view — retunes to the new centre on release |
| Pinch (two fingers) | Zoom ×1.0 – ×24.0 |
| Two-finger drag | Pan the zoomed window |
| Double-tap | Reset zoom and pan to ×1.0, centred |
| Top-bar **Zoom** → tap | Pick a preset |

**One-finger pan (stroll).** A fast horizontal swipe — more than about 70 px of movement within the first 250 ms of touching down — slides the spectrum and waterfall under your finger in real time, with a live frequency tooltip, and retunes to wherever you release. It works at any zoom level, alongside the two-finger pinch and pan, and it is the quickest way to move along a band without dropping into a deliberate tune-drag.

At zoom levels above ×1 the display **centres on the passband**, not the VFO dial — which matters in USB and LSB, where the passband sits to one side of the carrier. It re-centres automatically when you change mode or filter width, so the active receive area stays on screen, and the passband lines and frequency axis track correctly at every zoom level.

The zoom level is **persisted**, and comes back at full zoom-FFT resolution on the next boot.

### 9. S-Meter

The **Signal** field in the top bar is a tick-scale bar labelled S1, S3, S5, S7, S9, +10, +20, with a moving green bar beneath it.

- **S1 to S9** — standard S-units, −130 to −73 dBm
- **+10 / +20** — above S9, in 10 dB steps

The scale is **S9 = −73 dBm**, 6 dB per S-unit below S9 and 1 dB per unit above it.

The reading is the peak level in a ±64-bin window centred on the **IF-shifted VFO bin** — corrected for the QMX's +12 kHz IF offset, so it measures the signal actually under your cursor rather than the DC/local-oscillator spike sitting at the centre of the spectrum. It stays live during FT8 capture.

It is a readout, not a control: there is nothing to tap, and there is no peak-hold mode.

### 10. Settings Drawer

Swipe ← from the right edge to open the settings drawer, or tap the right grip handle.

It is grouped — **Station, Device, Radio, Network, Display, FT8, Spectrum** — with a **Basic / Expert** toggle at the top. Basic shows what a normal session needs; Expert reveals the tuning and calibration controls, so the list stays short until you need it to be long.

**Every control, group by group, is documented once in [Settings](settings.md)** — deliberately in one place rather than summarised here as well.

---

**Next:** Learn about [FT8 Receive](ft8-rx.md) or [Web UI](web-ui.md).
