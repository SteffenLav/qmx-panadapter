# CW

The CW page is the third screen in the carousel: swipe left from the Panadapter to reach FT8, and left again for CW. The spectrum and waterfall keep running behind it, so you still see the band while you operate.

!!! warning "Transmit is not yet hardware-verified"
    Everything on this page has been developed and tested against the built-in simulator. Keying the radio over CAT has **not** been confirmed on real hardware yet. Treat CW transmit as untested until this note disappears from the documentation.

The page is built for one-handed, gloved, field operation — the kind of CW you do standing at a picnic table with a paddle you have not touched in a month.

---

## The screen

**Left pane, top to bottom:**

| Control | What it does |
|---|---|
| **MODE: CW** | Header. The page forces the radio into CW mode whenever you enter it. |
| **WPM** with `-` / `+` | Keyer speed, read back from the radio so the number always matches what the QMX itself thinks. |
| **Park** | Your POTA park reference, used by the `{PARK}` macro. Tap to edit. |
| **NEXT** | The one-tap QSO ladder — see [Running a QSO](#running-a-qso). |
| **Six memory buttons** | Your keyer messages. |
| **Status line** | What the radio is doing: receiving, keying, or how many characters are left to send. |

**Right of the pane:** the live spectrum and waterfall, the **decode ticker** along the bottom, the **copilot hint** line just above it, and the **station card** when a callsign has been heard.

---

## Band discipline

The CW page only lets you operate the CW portion of the band. That is deliberate:

- Entering the page moves you into the band's CW segment if you were somewhere else.
- Changing band lands you in the **middle of that band's CW segment**.
- Tuning — by tap, drag, band-plan strip or keypad — is **clamped** to the CW segment. You cannot accidentally tune into the phone portion while on this page.
- The band-plan strip below the spectrum shows only the CW segment, and the filter is set to **300 Hz** on entry.

The segment boundaries come from the band plan for your region, which you can set in **Settings → Band-plan region**.

---

## Memory messages

Six slots, each with macros expanded when you send:

| Macro | Becomes |
|---|---|
| `{MYCALL}` | Your callsign from Settings |
| `{PARK}` | The park reference from the Park button |
| `{CALL}` | The station you are working |

**Sending and editing:**

- **Tap** a memory to send it. The button lights **light grey** while your finger is down so you can see which one will fire.
- **Drag up or down** without lifting to move to a different button — useful because the buttons are narrow and a fingertip covers more than one. Lift on the one you want.
- **Hold for two seconds** and the editor opens immediately, with your finger still down. The hold timer restarts whenever you slide to a different button, so taking your time to aim never opens the editor by accident.
- An **empty slot** opens the editor when tapped.

The editor shows all six messages at once, so you can set up a whole activation in one visit.

---

## Reading the band

**The decode ticker** shows what the QMX's own CW decoder hears, letter by letter as it arrives.

With no signal, a CW decoder produces a steady dribble of single characters — `T T T T T`. The ticker **squelches** these: when the same lone letter repeats, it is shown twice and then suppressed until something else arrives. A lone `R`, `K` or `N`, and `EE`, are never filtered, because those are real. The filter only affects what is displayed — callsign capture and the hints always see the raw stream, so nothing can be lost to it.

**The copilot hint line** sits just above the ticker and says what the copy *meant*, in plain language:

| You see | It means |
|---|---|
| **W1AW is calling CQ — tap the card to answer** | Someone is calling and you may answer |
| **W1AW is activating a park — tap the card to work them** | A POTA activation (`CQ POTA`) |
| **W1AW is answering YOU — tap the card to work them** | They came back to your call |
| **That was YOUR callsign** | Your call went by |
| **That was a signal report: 599** | A report was sent |
| **That was their park reference: US-1211** | A POTA reference was sent |
| **They are signing off (73 / TU)** | The contact is ending |
| **They want a repeat — send it again** | `AGN` / `PSE` |
| **QRZ — they are asking who called** | |

Hints are **green when there is something to act on** and amber otherwise, and they clear after about 25 seconds so a stale reading never sits beside newer copy.

This is what makes the page usable if you cannot yet copy CW by ear: the ticker gives you the letters, the hint tells you what just happened, and NEXT tells you what to send back.

**The station card** appears when a callsign is heard, showing the country, whether you have worked them before (and on this band), the report they sent you, and their **park reference** if they gave one. Tap the card to start working them; tap again to drop them.

---

## Listening: the two audio paths, and what each one can do

The Tab5 and the QMX produce **separate** audio from the same receiver, and
knowing which is which is the difference between a setup that works first time
and an evening of fighting it.

| | QMX headphone output | Tab5 output (speaker or headphones) |
|---|---|---|
| Source | the QMX's own demodulator | the raw I/Q stream over USB |
| Carries your keying sidetone | **yes** | **no, and it never will** |
| Stereo separation of two stations | no | **yes** — Binaural CW |
| Set its level with | the radio's volume (or **QMX volume** in the drawer) | **RX Volume** in Resource Management |

**The two volumes are completely independent.** Measured on the bench
2026-11-02: moving the QMX's AF gain across its whole useful range — 0 dB, then
40 dB, a factor of 100 in amplitude, each confirmed by the radio's own
read-back — changed the level arriving over USB by nothing at all. I/Q mode
carries the samples from *before* the QMX's audio chain, so the radio's volume
and its RF gain both stay on the radio's side of the glass. Turn the QMX right
down and the Tab5 keeps playing at exactly the same level.

### Why the Tab5 cannot give you a sidetone

Its playback path is about **100 ms** deep — ~80 ms of I2S buffering, up to
48.5 ms inside the codec write, plus a ~21 ms processing frame. A dot at
20 WPM lasts 60 ms, so the tone would begin after the element that produced it
had already ended, and under QSK you would hear the previous character while
sending the next. This is measured, it is the Tab5's own latency rather than
the radio's, and no setting changes it. FlexRadio reach the same conclusion
about their own hardware: the sidetone is delivered by whatever device is doing
the keying.

So **your keying monitor always comes from the QMX.**

### Binaural CW needs both ears

**Binaural CW (stereo separation)** in Resource Management splits the CW
passband the radio has already chosen into its lower and upper halves and pans
them left and right, with a little cross-feed so a dead-centre station still
sits in the middle. Two stations either side of your pitch then separate
spatially instead of blurring into one note.

Three things to know:

- It needs **RX Audio** switched on first — the effect is produced on the Tab5.
- It applies in **CW and CW-R only**. Every other mode is plain mono whatever
  this is set to.
- ⚠ **It is not remembered across a reboot.** Switch it back on after a power
  cycle.
- ⛔ **Do not split your headphones between the two radios while it is on.**
  One ear then gives you one half of the passband and only a trace of the
  other, so you would lose stations on the far side of your own pitch. If you
  want the QMX in one ear and the Tab5 in the other, turn Binaural CW **off**
  and accept mono on the Tab5 side.

### The recommended setup for working stations by paddle

Both ears on the Tab5 with Binaural CW on, and the QMX's sidetone switched into
those same headphones on key-down — an analog audio switch driven from PTT.
That is the only arrangement that gives you spatial separation while receiving
*and* a monitor with correct QSK timing, because the switch moves the audio in
hardware with no buffer in the way. The design is Michael KZ4LY's.

Without a switch you have to choose one:

- **Both ears on the Tab5** — best receiving, no keying monitor.
- **QMX in one ear, Tab5 in the other, Binaural CW off** — a monitor in one
  ear and mono receive in the other.

On the radio's side, whichever you pick, set the monitor so it stops moving
when you change receive volume:

1. In the QMX's **CW** menu set **Sidetone abs/rel** to **Absolute**.
2. Set **Sidetone volume** to taste.

Left at the factory setting of *Relative*, the main volume control moves the
received audio and your sidetone together by the same amount, so you cannot
bring the receive level down without taking your own monitor with it. The
**CW monitor level** row in the settings drawer sets both of these from the
Tab5, and reads them back from the radio rather than keeping its own copy.

---

## Running a QSO

The **NEXT** button always shows exactly what it will send. One tap sends it and advances:

1. **CQ** — sends your CQ memory. Stays here, listening.
2. **Report** — once a station is adopted, sends `<their call>` plus your report memory.
3. **TU + log** — sends your closing memory **and writes the QSO to the ADIF log**.

A station is adopted automatically when the decoder hears your call followed by `DE <their call>` shortly after your CQ. You can also adopt anyone by tapping the station card, which is how you answer someone else's CQ. **Long-press NEXT** to drop the contact and go back to CQ.

The logged contact records mode CW, the report you sent, and the report you received **only if it was actually decoded** — a report that was never exchanged is left out of the log rather than invented.

---

## Practice mode (no radio needed)

**Settings → CW Simulation Mode** turns the page into a practice bench. A red border around the screen shows it is active, and **the radio is never keyed** while it is on.

The simulated band behaves like a real one:

- **Stations call CQ on their own** every half-minute or so, some of them park activations, so you can practise *answering* rather than only being answered.
- **The copy is imperfect.** Characters are dropped and mis-timed into neighbouring letters — `E` becomes `I`, `T` becomes `M` — exactly the way a real decoder fails. Callsigns are usually still readable because real CW repeats them, which is the skill the page is teaching.
- **The band has noise** between transmissions.
- Whoever you are working answers you through the whole exchange, and the QSO logs to ADIF like any other.

Practice contacts are logged with no frequency, which marks them as test records — the ADIF viewer's **"Del N test"** button removes them in two taps when you are done.

Turning simulation off clears the ticker, the hint, the station card and the ladder, so no phantom contact is left looking like real copy.

---

## Known limitations

- **Transmit is unverified on hardware** (see the warning at the top).
- There is **no abort button** for a message being sent. Adding one has to wait until it is known what actually cancels a transmission on the radio.
- **Zero-beat tuning aid** and **speed matching** are designed but not yet built; both need real signals to develop against.
