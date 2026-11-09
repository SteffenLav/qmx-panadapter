# CW *(beta)*

The CW page is the third screen in the carousel: swipe left from the Panadapter to reach FT8, and left again for CW. The spectrum and waterfall keep running behind it, so you still see the band while you operate. The page ships **on** - it is this release's own beta. Turn it off with `cw_page_enabled = no` in a config file, or `{"action":"cw_enable","on":false}` over the API, if you would rather not have it yet.

!!! warning "There is no abort for a message in progress"
    `KY` has no abort in the QMX's CAT manual. Tested on the bench: neither sending `RX;` nor bouncing the mode away from CW and back stops an in-flight send - both were tried and both simply had no effect, the keyer ran to completion either way. Once you tap a memory, it sends to the end. Keying the radio itself **is** confirmed working, off air, by the operator.

The page is built for one-handed, gloved, field operation — the kind of CW you do standing at a picnic table with a paddle you have not touched in a month.

*(v1.25.0)* **The CW page is now also on the web** — see
[Web UI → CW Control](web-ui.md#cw-control) for sending macros, editing them,
switching macro sets, entering a callsign by hand, and setting a contest name,
all from a browser.

---

## The screen

**Left pane, top to bottom:**

| Control | What it does |
|---|---|
| **MODE: CW** | Header. The page forces the radio into CW mode whenever you enter it. |
| **WPM** with `-` / `+` | Keyer speed, read back from the radio so the number always matches what the QMX itself thinks. **Tap the number itself** *(v1.24.2)* for a numeric keypad instead of stepping one WPM at a time. |
| **Park** | Your POTA park reference, used by the `{PARK}` macro. Tap to edit. |
| **TX offset** *(v1.24.2)* | Opens the CW transmit offset popup — see [TX offset](#tx-offset) below. |
| **NEXT** | The one-tap QSO ladder — see [Running a QSO](#running-a-qso). |
| **Six memory buttons** | Your keyer messages. |
| **Status line** | What the radio is doing: receiving, keying, or how many characters are left to send. |
| **General / Contest toggle** (bottom of the panel) | Switches between two named sets of six macros — see [Macro sets](#macro-sets). Placed at the very bottom, separated from the memory buttons by the status line, so it cannot be mis-hit while working the macros above it. |

**Right of the pane:** the live spectrum and waterfall, the **decode ticker** along the bottom, the **copilot hint** line just above it, and the **station card** when a callsign has been heard.

---

## Band discipline

The CW page lets you operate the CW **and Digi** portions of the band — not the
phone portion. That is deliberate: CW is legal inside the digital sub-band, but
digital modes are not legal inside the CW one, so clamping to CW alone would
deny you spectrum you are actually allowed to use.

- Entering the page moves you into the band's CW segment if you were somewhere else.
- Changing band lands you in the **middle of that band's CW segment**.
- Tuning — by tap, drag, band-plan strip or keypad, **on the Tab5 or from the
  web** *(v1.25.0 — the web's own tuning paths now honour this too)* — is
  **clamped** to CW + Digi. You cannot tune into the phone portion while on
  this page.
- The band-plan strip below the spectrum draws the CW and Digi segments only,
  each in its own colour, stretched to fill the whole strip — never Phone —
  and the filter is set to **300 Hz** on entry.
- *(v1.25.0)* **Spots on this page are CW only**, even though Digi is in
  reach — see [Live Spots](spots.md#2-tapping-a-spot). Tapping an FT8/FT4
  activity spot would switch the radio's mode away from CW, which this page
  has no business offering.

The segment boundaries come from the band plan for your region, which you can set in **Settings → Band-plan region**.

---

## TX offset

*(v1.24.2)* Tap the **TX offset** button, beside Park, to open a popup with four nudge buttons (`-50`, `-10`, `+10`, `+50` Hz) and the current value.

This shifts where you **transmit** relative to where you are **listening** — without moving your dial. It exists so you can avoid zero-beating the exact frequency of the station you are calling: many operators prefer to be heard a little off to the side of a pileup rather than dead on top of it. The radio achieves this with its own split (VFO A/B) mechanism under the hood; you do not need to think about VFOs, only about the offset in Hz.

- **Clear** resets the offset to zero (transmit where you listen — "zero-beat"). This is a different thing from the **long-press on the top bar's BW indicator**, which retunes your *receive* dial onto a signal's peak — the two happen to share the word "zero-beat" in amateur radio but do opposite jobs: one moves your RX frequency onto a signal, the other clears a TX/RX offset back to none.
- Range is ±300 Hz, in 10 Hz steps from the nudge buttons.
- The setting is stored on the Tab5 and survives a reboot; it has no effect outside CW/CW-R.
- This same control also exists in the settings drawer (**Settings → Radio → CW TX offset**) if you prefer a slider.

## Filter bandwidth

The top bar's **BW: ---** indicator is live in CW mode as well as FT8/SSB/AM: tap it for the same filter-width popup you use everywhere else, showing only the widths your radio's own **CW → Choose filters** menu has enabled. There is no separate bandwidth control on the CW page itself — the top bar one already works here, and reaches the radio's real hardware filter.

## Break-in delay (Semi QSK)

*(v1.24.2)* How long the receiver stays muted after you stop keying — useful if you run an amplifier or relay that needs the receiver quiet for the whole transmission rather than switching between every dit and dah. This lives in the settings drawer, not on the CW page itself: see [Semi QSK](settings.md#radio) under Settings → Radio for the full set of modes and the delay control.

---

## Memory messages

Six slots, each with macros expanded when you send:

| Macro | Becomes |
|---|---|
| `{MYCALL}` | Your callsign from Settings |
| `{PARK}` | The park reference from the Park button |
| `{CALL}` | The station you are working |
| `{RST}` | A pro-forma signal report: `599`, or `5NN` if you long-press the `{RST}` button when inserting it (toggles a saved cut-numbers preference) |
| `{SERIAL}` | Your contest serial, incrementing with each exchange sent, with cut numbers (0 sent as T) — see [Contest logging](#contest-logging) |

**Sending and editing:**

- **Tap** a memory to send it. The button lights **light grey** while your finger is down so you can see which one will fire.
- **Drag up or down** without lifting to move to a different button — useful because the buttons are narrow and a fingertip covers more than one. Lift on the one you want.
- **Hold for 800 ms** and the editor opens immediately, with your finger still down. The hold timer restarts whenever you slide to a different button, so taking your time to aim never opens the editor by accident.
- An **empty slot** opens the editor when tapped.

The editor shows all six messages at once, so you can set up a whole activation in one visit.

---

## Macro sets

The toggle at the bottom of the panel switches between two independent sets of
six macros, each with its own editor:

- **General** — everyday CQ/exchange messages. The default macros use `599`
  for the signal report.
- **Contest** — the same six slots, with `5NN` in place of `599`. Whichever
  signal report you actually sent is scanned from the text at send time, so
  the log always records what went out, not a hard-coded assumption.

Edit either set by holding a memory button as above — the editor's title
names which set you are editing, and a toggle tap elsewhere mid-edit cannot
redirect where Save writes to.

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

**With nothing heard, the card shows "tap to enter a callsign"** *(v1.24.2)* — tap
it to type one on a popup keyboard instead of waiting for it to be decoded.
That is how you adopt a station you can copy by ear but the decoder has not
caught, or one you already know from another source.

**The pencil icon beside the card** *(v1.24.2)* always opens the same
manual-entry keyboard, whether or not a callsign is already showing — so a
wrongly auto-adopted call can be corrected without first dropping the contact.

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

The [Audio](audio.md) chapter covers the rest of the audio path — the AGC, the
pan controls, and listening in a browser. What follows here is only what a CW
operator has to decide.

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

*(v1.25.0)* **The Tab5 now does this for you.** Left at the factory setting of
*Relative*, the QMX's main volume control moves the received audio and your
sidetone together by the same amount, so you cannot bring the receive level
down without taking your own monitor with it. Entering the CW page now checks
the radio's reported `Sidetone abs/rel` setting, and if it reads **Relative**,
switches it to **Absolute** and starts mirroring the **QMX volume** slider into
**Sidetone volume** itself (with a 15% floor, so it can never go fully silent).
Leaving the page restores **Relative**, so a standalone QMX is never left
needing to be repaired by hand. If the radio is already **Absolute** — Michael
KZ4LY's own deliberate binaural setup, described above — this does nothing at
all.

The **CW monitor level** row in the settings drawer still exists for setting
the level and the abs/rel mode directly, and reads them back from the radio
rather than keeping its own copy — useful outside the CW page, or if you want
a level other than what the automatic mirror would pick.

---

## Running a QSO

The **NEXT** button always shows exactly what it will send. One tap sends it and advances:

1. **CQ** — sends your CQ memory. Stays here, listening.
2. **Report** — once a station is adopted, sends `<their call>` plus your report memory.
3. **TU + log** — sends your closing memory **and writes the QSO to the ADIF log**.

A station is adopted automatically when the decoder hears your call followed by `DE <their call>` shortly after your CQ. You can also adopt anyone by tapping the station card, which is how you answer someone else's CQ. **Long-press NEXT** to drop the contact and go back to CQ.

The logged contact records mode CW, the report you sent, and the report you received **only if it was actually decoded** — a report that was never exchanged is left out of the log rather than invented.

---

## Contest logging

*(v1.25.0)* **Set contest…**, from the web CW panel (see
[Web UI → CW Control](web-ui.md#cw-control)) or `[cw] contest =` in a config
file, names the contest you are currently running. While it is set, every CW
QSO logged here carries that name as ADIF `CONTEST_ID`, alongside a genuine
contest exchange:

- **The serial you actually sent** — read back from the expanded `{SERIAL}`
  macro text, never a number assumed from a counter — as ADIF `STX_STRING`.
- **The serial the other station sent back** — captured from the decoder right
  after their report, the same way the report itself is captured — as ADIF
  `SRX_STRING`.

The station card shows the received serial next to the report once it has been
decoded. This is mutually exclusive with ARRL Field Day logging, which keeps
writing its own `CONTEST_ID` the way it always has.

---

## Practice mode (no radio needed)

**Settings → CW Simulation Mode** (or a three-finger hold on this page) turns it into a practice bench. A red border around the screen shows it is active, and **the radio is never keyed** while it is on — the status line says "Simulated:" rather than "Keying:" so a glance always tells you which is happening.

The simulated band behaves like a real one:

- **Stations call CQ on their own** every half-minute or so, some of them park activations, so you can practise *answering* rather than only being answered.
- **The copy is imperfect.** Characters are dropped and mis-timed into neighbouring letters — `E` becomes `I`, `T` becomes `M` — exactly the way a real decoder fails. Callsigns are usually still readable because real CW repeats them, which is the skill the page is teaching.
- **The band has noise** between transmissions.
- Whoever you are working answers you through the whole exchange, and the QSO logs to ADIF like any other.

Practice contacts are logged with no frequency, which marks them as test records — the ADIF viewer's **"Del N test"** button removes them in two taps when you are done.

Turning simulation off clears the ticker, the hint, the station card and the ladder, so no phantom contact is left looking like real copy.

---

## Gestures

The spectrum and waterfall behind the pane take the same touch gestures as the
Panadapter page: pinch with two fingers to zoom and pan together (pan comes
from how far the midpoint between your fingers moves, so a drag with roughly
steady finger spacing pans without zooming much), and a one-finger swipe to
pan without retuning. A three-finger hold toggles **CW Simulation Mode** on
or off, the same gesture FT8 uses for its own simulator.

---

## Known limitations

- **No abort for a message in progress** (see the warning at the top) — both
  tested candidates (`RX;`, a mode bounce) were tried and neither works.
- **Zero-beat tuning aid** and **speed matching** are designed but not yet built; both need real signals to develop against.
