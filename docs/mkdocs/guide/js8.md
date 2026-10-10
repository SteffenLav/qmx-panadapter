# JS8 (beta)

**JS8 Normal runs on the Tab5 itself** — the decoder, the error correction and the transmitter, with no PC in the loop. JS8 is a keyboard mode built on FT8's modulation: it carries free text rather than the fixed exchange FT8 uses, so stations hold short conversations with each other.

JS8 is **new in v1.17.0 and declared a beta**. What is proven and what is not is spelled out in [section 6](#6-what-is-proven-and-what-is-not). Read that before relying on it.

### 1. Selecting JS8

Swipe → from the left edge to reach the FT8/FT4/JS8 view, then pick **JS8** from the **Preset** dropdown in the left pane. The dropdown is three wide now — FT8, FT4 and JS8.

Only **JS8 Normal** is supported: 15-second slots, the same slot length as FT8. The faster and slower JS8 speeds (Fast, Turbo, Slow) are not implemented, and Turbo cannot be reached over CAT in any case.

⚠ **JS8 and FT4 share two of their calling frequencies.** If you were expecting FT4 activity on a frequency and see JS8 instead, that is why — the modes overlap on the band plan, not a fault.

### 2. The Stations and Conversation tabs

With JS8 selected, the right pane carries two tappable tabs:

- **Stations** — who is on the band. Each station that has identified itself, with its grid and how long ago it was heard.
- **Conversation** — what has been said, newest first, with four columns: **AGE**, **Hz**, **CALL** and **MESSAGE**.

Rows are tappable in both. The same two views appear in the browser, so you can watch a conversation from a laptop while the Tab5 sits with the radio.

### 3. What the decoder reads

Three kinds of JS8 traffic arrive:

- **Directed messages** — a message aimed at a particular station, shown with the sender's callsign.
- **Heartbeats** — the periodic `HB <call> <grid>` that JS8 stations send to say they are listening.
- **Free text** — a plain message, which JS8 splits across as many 15-second slots as it needs. The Tab5 joins the pieces back together and shows the message once it is whole, with a partial line while the rest is still arriving.

A free-text frame carries **no callsign of its own** — that is how JS8 works, not a limitation here. The Tab5 names a free-text message after a station that identified itself on the same audio frequency, and if two stations are close enough together to be equally plausible it shows the **frequency instead of a name**. A wrong callsign on somebody's message is worse than no callsign, so it declines to guess.

### 4. Sending free text

**On the Tab5:** tap **Free text**, the button under Call CQ. It is shown in JS8 only. Type the message, and the window tells you how many frames it takes and how many seconds it holds the frequency *before* anything is keyed, along with the text exactly as it will go out. **Send** starts it; **Stop TX** aborts a run that is already going out, and stays reachable while transmitting.

**In the browser:** the same composer sits under the conversation pane on the FT8/JS8 page, with the same plan line and the same Send and Stop.

The text is upper-cased, runs of spaces are collapsed and anything JS8 cannot carry is dropped. That is the encoder's own rule, and what the plan line shows you is the encoder's answer rather than a second copy of the rule — so what you read is what goes on the air.

⚠ **A long message is a long transmission.** One frame goes out per 15-second slot, so a twelve-frame message holds the frequency for three minutes. Read the plan line before you send.

The same three actions are scriptable, which is how the feature worked before the composer existed:

```
POST /api/cmd   {"action": "js8_text", "text": "HELLO FROM THE PANADAPTER"}
```

- `{"action": "js8_text_plan", "text": "..."}` reports the frame count and the seconds **without transmitting**.
- `{"action": "js8_text_cancel"}` stops a run that is already going out.

Progress is in `/api/status` under `ft8.ftx_sent` and `ft8.ftx_total`.

### 5. Calling CQ

The **Call CQ** control works in JS8 as it does in FT8, and sends `CQ <your call> <your grid>`.

Your grid goes on the air as **four characters**, always. If you have entered a six-character grid it is truncated for transmission — a six-character grid in a JS8 CQ is read by other stations as a callsign, which makes the call unintelligible.

### 6. What is proven, and what is not

Stated plainly, because "beta" on its own tells you nothing:

**Proven on air:**

- Receiving. Real stations decoded off air, including other people's free text arriving intact.
- Sending free text. A real JS8Call at the other end reassembled a three-frame message and displayed it complete, with the end-of-message marker it draws for a finished transmission.
- Calling CQ. A CQ from the Tab5 was decoded by a second station.

**Not proven:**

- **A full automatic two-way JS8 QSO has not been completed.** That is the next piece of work.
- **Compound callsigns** (`G0ABC/P`, `W1ABC/4`) are not handled.
- The fix that stops a message being shown under the wrong callsign (section 3) was built from a real on-air case but has **not** itself been re-tested on air — reproducing it needs a third station on a nearby frequency, which is not something that can be arranged to order.

### 7. Signal reports

JS8 signal reports come from the same measurement as FT8's and are calibrated against **`jt9`**, the reference decoder from the WSJT-X family, using 154 paired decodes of the same recordings. A report here should be within a decibel or two of what WSJT-X or JS8Call would say about the same signal.
