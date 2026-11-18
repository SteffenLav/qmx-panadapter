# RTTY

The Tab5 receives and sends RTTY itself. The decoder runs on the radio's I/Q, and the transmitter keys the QMX with the same tone command FT8 uses. No PC and no sound card are in the loop.

To open the page, open the **page picker** (swipe right from the left edge, or tap the left grip) and tap **MODE: RTTY**. The spectrum and waterfall stay on screen, with the decoded text below them. In the browser, use **Switch to RTTY** in the menu: the same page and the same controls appear there, including sending (see [In the browser](#in-the-browser)).

---

## Tuning to a station

**Tap the signal on the waterfall.** The dial moves so the signal sits at 1500 Hz audio, and the decoder starts on it. Two orange lines on the spectrum show the decoder's mark and space tones. Taps snap to a 10 Hz grid.

- The decoder finds the tone pair within ±150 Hz of the tap by itself, so a tap does not have to be exact.
- After a station is decoded, the decoder holds on it. It does not jump to a neighbour in the gaps between overs. A new tap releases the hold.
- **Frequency readout:** on this page the top bar shows the **mark** frequency, not the dial. The keypad works the same way: the number you type is the mark.
- The page keeps one RTTY dial per band. A band change on this page goes back to the last RTTY dial you used on that band, or to the middle of the band's digital segment the first time.
- The band-plan strip shows the digital segment with a margin on each side, and always includes the part of the band on screen, so you can drag to stations just outside the band plan's digital segment. The spot labels show RTTY spots only.
- **Opening the page puts the radio in DiGi mode** *(v2.0.2)*. In another mode the radio's audio sits away from the decoder's lines. The radio's previous mode comes back when you leave the page.
- The Tab5 speaker plays the signal under the orange lines (±250 Hz around them), not the radio's whole passband. The QMX headphone jack still plays the radio's own passband.

## The screen

**Left pane, top to bottom:**

| Control | What it does |
|---|---|
| **M / S bars** | Mark and space tone levels. Both bars about equal = tuned on the pair. |
| **Sig** bar | Signal level. The squelch opens at the white tick. |
| **AFC** readout | How far the decoder has followed the station from the tap, in Hz. |
| **Speed / shift** button (`45.45/170`) | Opens the picker: 45.45, 50, 75 or 100 baud; 170, 200, 425, 450 or 850 Hz shift; and **Auto**. Green = Auto is on. |
| **Reverse** | Swaps mark and space. |
| **AFC** | Follows a drifting or mistuned station. |
| **Squelch** | Prints nothing while no RTTY signal is present. It opens only when **both** tones stand at least 6 dB above the noise. |
| **Clear text** | Empties the text box, on the Tab5 and in the browser. |
| **TX** block | Sending - see [Sending](#sending). |
| **Status line** | Listening, decoding, or sending; and why a send was refused. |

**Text box:** six rows. Earlier lines are green, the current line is cyan, and what **you** sent is red, in the place it went.

## Auto speed and shift

With **Auto** on (the default), the page measures the station's speed, shift and polarity. When a station is present (both tones clear of the noise) and the reading differs from the current setting for 3 s, the page tries it for 5 s. A setting under trial is not saved; only a kept one is. The new setting stays only if the decoded characters get clearly better (a mean character confidence at least 0.08 higher). If they do not, the page goes back to the old setting and does not try again for 30 s.

A bad signal can therefore cost one 5-second trial every 30 seconds, never more. Choosing a speed or shift by hand turns Auto off; tap **Auto** in the picker to turn it on again.

## Sending

!!! note "Before the first send"
    Set your callsign under **Identity**. Without it the Tab5 refuses to send. Your grid (four characters) goes into **Exch** when it is set.

1. **Pick the station.** Tap their callsign in the text box. It gets a blue box, and **To: CALL** shows above the buttons. **Answer**, **Exch** and **73** stay dimmed until a station is picked.
2. **Press a message.** Each one is a complete over:

    | Button | Sends |
    |---|---|
    | **CQ** | `CQ CQ CQ DE MYCALL MYCALL MYCALL` twice, then `PSE K` |
    | **Answer** | `CALL CALL DE MYCALL MYCALL MYCALL K` |
    | **Exch** | `CALL DE MYCALL TNX FER CALL UR RST 599 599 QTH GRID GRID` / `CALL DE MYCALL K` |
    | **73** | `CALL DE MYCALL TNX FER QSO 73 GL` / `CALL DE MYCALL SK` |

3. **Or type.** **Type...** opens a line; each key is sent as you type it. A Bluetooth or snap-on keyboard types into it directly; without one, the on-screen keyboard opens. Backspace edits the line but cannot unsend. Closing the line ends the over.
4. **STOP** turns the tone off at once and drops whatever is still queued. Leaving the page does the same.

**Where it transmits.** On the decoder's tones: you transmit where you decode. If the decoder followed a signal (AFC) that it never decoded, the send goes out at your tap instead, not on whatever the AFC found.

**What the Tab5 does before it keys:**

- If the radio is not in **DiGi** mode, the Tab5 switches it and waits for the radio to confirm. The radio stays in DiGi afterwards.
- If the tones would fall outside 400-2700 Hz audio, the dial moves first so they sit around 1500 Hz.
- The **band guard** checks both tones on the dial actually in force, right before key-down. Outside your licence for the IARU region, nothing is sent.
- It refuses while WSPR runs, while another transmission holds the radio, or in simulation mode. The status line says which.

**When an over ends:** 3 s after the queue is empty, on **STOP**, or after **180 s** at most, whatever is still queued. Every over starts with a press or a key: nothing on the RTTY page transmits by itself.

## Logging

When you pick a station, a card shows above the text box, at the right:

- the callsign
- **New**, **Worked on band** (in your log on this band before), or **DUPE** (contest mode: already logged on this band in this contest)
- **Rcvd**: the report (and, in contest mode, the exchange) the decoder found in their text
- a **Log** button

**73** (or **TU** in contest mode) and **Log** open **Log this QSO?**. Nothing is logged until you press **Log** in it. The fields come filled in: RST sent 599, RST rcvd and the received exchange only when the decoder found them. Check them: RTTY errors damage numbers. A lost figures shift prints `599 001` as `TOO QPP`; the Tab5 reads letters on the number keys back as digits, but cannot know every error.

**To correct a field, tap the word in the text.** While the dialog is open, a tapped word goes where it belongs: a report to **RST rcvd**, a callsign to **Callsign**, a number to **Exch rcvd**. The line under the fields says what went where. Tap a field to type instead.

The record gets mode **RTTY**, the dial frequency and the time from the device. **Log** clears the station, ready for the next one. **STOP** stays in reach while the dialog is open.

The drawer's **Manual log** on this page also logs mode RTTY with RST 599.

### Contest mode

Tap **TX** above the send buttons (or **Contest...** in the speed picker) to set it:

- **Contest mode** on or off
- **Contest name** - written to every QSO as `CONTEST_ID`, for example `WAE-DX-RTTY`
- **Next serial**
- **Sent exch** - leave empty to send the serial, or enter a fixed exchange (a zone, a state)

While it is on, **TX #013** above the buttons shows what you send next, and the messages are the short contest forms:

| Button | Sends |
|---|---|
| **CQ** | `CQ TEST MYCALL MYCALL CQ` |
| **Answer** | `MYCALL MYCALL` |
| **Exch** | `CALL 599 013 013` |
| **73** | `TU MYCALL`, then asks to log |

**The serial moves on when a QSO is logged**, not when it is sent, so a repeated **Exch** sends the same number. Logged contest QSOs also carry `STX_STRING` and `SRX_STRING`.

**Dupes** are this contest's RTTY QSOs on the band - contacts from other days and other contests do not count. The list comes from the log on the Tab5 and, at each start, from the archive on the microSD card (older records move there). Without a card at start, QSOs moved at an earlier start are not seen.

## In the browser

The browser's RTTY page has the same controls as the Tab5 pane, and the same text box over the waterfall: received text green and cyan, your own text red.

- **Tune:** click the waterfall. As on the Tab5, the dial moves and the decoder starts on the click.
- **Pick the station:** click a callsign in the text, or type it in **To:** and press Enter. **The station is shared:** a callsign picked in the browser shows on the Tab5, and the other way round.
- **Send:** **CQ**, **Answer**, **Exch**, **73** and **STOP** under the controls. The device builds the messages, so the browser and the Tab5 send exactly the same text.
- **Type:** the **Type** line sends each key as you type (in small batches). **Enter** ends the over.
- While sending, the line beside **To:** shows `TX n s` and the last characters sent, in red. A refusal shows there in orange for 8 s.
- **Log:** the line under the buttons shows what the Tab5 card shows (New / Worked on band / DUPE, Rcvd). **73** and **Log...** open the same form as on the Tab5; while it is open, click a word in the text to fill its field. **Contest...** sets contest mode, shared with the Tab5.

## Settings and API

Speed, shift, Reverse, AFC, Squelch, Auto and the tap offset survive a reboot. In a config file they are `rtty_offset_hz`, `rtty_baud`, `rtty_shift_hz`, `rtty_reverse`, `rtty_afc`, `rtty_squelch` and `rtty_auto`.

Scripts use `POST /api/cmd`:

| Action | Fields | What it does |
|---|---|---|
| `set_screen` | `screen: "rtty"` | Opens the RTTY page. |
| `rtty` | any of `baud`, `shift`, `reverse`, `afc`, `squelch`, `auto`, `offset_hz`, `tune_hz`, `mark_hz`, `clear`, `call` | Changes settings and returns the decoder status, the last decoded text and the send state (`tx`). `call` sets the station being worked (`""` clears). Receive only - it never keys. |
| `rtty_send` | `text` (and `end`), or `macro: "cq"/"answer"/"exch"/"73"` | **Keys the radio.** Starts an over, or adds to the one that runs. |
| `rtty_stop` | *(none)* | Tone off and back to receive, now. |
| `rtty_log` | `call`, `rst_sent`, `rst_rcvd`, `stx`, `srx` | Logs the QSO (mode RTTY, dial frequency, now) and clears the station. `stx`/`srx` only in contest mode. |
| `rtty_contest` | any of `on`, `name`, `next`, `exch` | Contest mode settings. |
| `rtty_word` | `word` | Which log field a word belongs in: `{"kind":"rst"/"call"/"exch"/"none","value":...}`. |

The `rtty` reply also carries `qso` (what the Tab5 card shows) and `contest`. In a config file, contest mode is `rtty_contest`, `rtty_contest_name`, `rtty_contest_next` and `rtty_contest_exch`.

## Limits

- Baudot (ITA2) only, with US-TTY figures and unshift-on-space. Speeds 45.45 / 50 / 75 / 100 baud.
- Auto has been proven on air against a clean 75-baud station and a damaged one. It has not yet met a live contest pile-up or a real 50- or 100-baud station.
