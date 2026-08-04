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
