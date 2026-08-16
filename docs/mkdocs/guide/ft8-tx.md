# FT8 and FT4 Transmit

The panadapter **keys the QMX and transmits full FT8/FT4 QSOs** — reply to CQ, run your own CQ, auto-answer (robot mode), or conduct a full exchange.


### 1. FT8 vs FT4 — Which to Use?

Both modes transmit via the same CAT interface; the difference is **slot length and symbol rate**:

| Feature | FT8 | FT4 |
|---------|-----|-----|
| **Slot length** | 15 seconds | 7.5 seconds |
| **Symbols transmitted** | 79 | 105 |
| **Symbol duration** | 160 ms | 48 ms |
| **QSO time** (full exchange) | ~90 sec (6 slots) | ~45 sec (6 slots) |
| **Use case** | Weak signal, SOTA/POTA | Busy bands, contests |
| **Preset picker** | Panadapter mode → Freq dropdown | FT8 mode → Preset picker |

**Choose FT4 when:**
- The band is crowded (faster exchanges, less QRM)
- You're in a contest (time is critical)
- You want quicker pileup resolution

**Choose FT8 when:**
- Signal is marginal (longer decode window tolerates more QRN)
- You're portable/POTA (less pressure to transmit fast)
- You prefer the relaxed pace

FT4 has full feature parity with FT8, including time-sync from decoded signals (fixed in v0.19.3 — the time-sync modal shows "FT4" when appropriate and the bottom-bar clock correctly reads `UTC(FT4)` while in FT4 mode).

### 2. Switching Between FT8 and FT4

**In FT8 screen (left pane):**
- Tap the **Preset** dropdown (shows current frequency/mode)
- Select an FT4 frequency to switch to FT4
- Select an FT8 frequency to switch back

**In Panadapter screen:**
- Tap the **Band preset dropdown** (top left)
- Some bands have both FT8 and FT4 frequencies listed
- Tap the FT4 variant to switch

Your **settings are sticky** — when you switch back to FT8 or FT4, the panadapter remembers your last frequency, bandwidth, and filter settings for each mode.

The **decode list and pileup are cleared** on the switch (v1.3.3). The two modes use different slot lengths, so stations decoded under the old timing describe a band the new mode cannot hear — and leaving them on screen meant you could tap one and try to work a station that was no longer there.

### 3. Modes of Transmission

#### 1. Reply to a CQ

In FT8 view, tap a CQ row in the decode list. A confirmation modal appears:

```qmxdiagram
type: panel
title: Confirm FT8 Transmission
row: K9ZZ EN52   -07   2138 Hz
row: W1AW FN31   -12   1406 Hz   <- the row you picked
row: SV1ENG JN37 -09   1869 Hz
row: TX1: W1AW OZ1LAV JO45
buttons: Cancel!, Auto Pounce, Transmit+
note: dim | drag up or down the list to move the highlight before you lift your finger
note: amber | Transmit sends this message once; Auto Pounce runs the whole exchange for you
```

- **Transmit** (green) — send this one message on the next correct slot
- **Auto Pounce** (blue) — handle the entire exchange automatically (TX1 → wait for report → TX2 → wait for RR73 → TX3 73)
- **▲ / ▼ Nudge** — move the target to the row above or below, without closing the modal and redoing the selection gesture
- **Cancel** — abort

**Transmit is intelligent (v1.3.0)** — it builds the correct *next* message from what that station last sent, exactly like a WSJT-X double-click: their CQ → your grid (or your report, with Skip TX1 on), their grid → your report, their report → `R`+your report, their `R`-report → `RR73`, their `RR73`/`73` → `73`. You can run a whole QSO one Transmit tap at a time — and sending the closing `RR73`/`73` **logs the QSO to ADIF** just like an automatic contact. Auto Pounce is offered on any first reply; once you're mid-exchange with a station, its rows offer Transmit only.

**A quick tap is enough** — you do not need to hold a row before releasing. Hold-and-drag still works for selecting across rows in a busy list.

The reply follows **correct FT8/FT4 parity** — if you're replying to an even slot, you transmit on the odd slot, and vice versa. For FT4 the countdown timer correctly counts down in 7.5-second slot increments, not 15-second FT8 ones.

**Skip TX1 (faster pounce).** With **Skip TX1** enabled in the Filter modal, pouncing a station opens with your signal report straight away instead of the grid exchange — saving one round trip. If the station has already dropped out of the decode list, it falls back to the normal grid exchange automatically.

**If your target is working someone else**, the panadapter holds instead of keying up over their exchange — the status shows "working *call* - waiting" with a **TAP TO CANCEL** line. Waiting costs nothing (a hold doesn't count toward the timeout), but if you'd rather move on, tap the status to cancel the pounce and pick a different station; the abandoned exchange stays resumable for a few minutes via the resume prompt.

#### Working a pile-up

When you call CQ (or work a busy run), more than one station may answer at once — and a caller who replies while you're mid-QSO with someone else used to vanish from the live decode list as soon as they stopped transmitting. They are now kept in a **Pileup** list so you don't lose them:

- Whenever one or more stations are waiting, the **ADIF-log** button on the FT8 screen becomes a **Pileup** button in a distinct colour, and reverts once the list empties.
- Tap **Pileup** to see everyone who has called you and isn't worked yet.
- **Hold the button to open the ADIF log** — the log stays reachable even while the button reads "Pileup" (a one-time hint appears the first time it flips).
- Tap a station in the list to work them (the same confirmation modal as a decode-list tap), or tap the small **✕** to dismiss them without working them. The reply is the correct **next** message for that station, built from whatever they actually last transmitted — usually a signal report for someone who has just called you, but an `R`+report if they came back later with a report of their own (v1.3.2; the same laddering as tapping a decode-list row).
- A station is removed from the list automatically once you start a QSO with them, and a just-completed contact's trailing 73 can't put them back.
- Stations you've worked before still appear in the pileup unless **Exclude worked-before** is checked — the pileup follows the same rule as the auto-answer.

The pile-up tracker never transmits on its own — it only remembers callers; you choose who to work. If you'd rather it *did*, check **Auto-work pileup** in the Filter modal: the strongest waiting caller is pounced automatically when your current QSO completes — or immediately, if you enable it with callers already waiting and nothing else in progress. It carries the same unattended-TX warning as the robot.

### 2. Call CQ

Tap the **Call CQ** button. A modal opens:

```qmxdiagram
type: panel
title: CQ Messages (check the active)
row: [x] CQ OZ1LAV JO65
row: [ ] CQ DX OZ1LAV JO65
row: [ ] CQ POTA OZ1LAV JO65
buttons: Cancel!, + call grid, Save+
note: dim | CQ stop and Listen sit top-right and apply on the tap - no Save needed
```

Choose a preset (or edit/save a new one), then tap **Transmit**. The QMX starts calling CQ and listening for replies.

**CQ tone selection** — the panadapter automatically picks a quiet frequency (6.25 Hz tone spacing, 200–2800 Hz audio range, no other stations nearby). If your chosen tone gets busy during a QSO, the panadapter **auto-relocates to the nearest clear frequency** to avoid stepping on other stations.

**CQ auto-stop** — by default the panadapter keeps calling until someone answers. If you'd rather call a few times and then pause (a common courtesy on a quiet band), long-press **Call CQ** and tap the **CQ stop** button at the top-right of the preset editor: it cycles through never / 1 / 2 / 3 / 4 / 5 / 10 calls and applies immediately, no Save needed. While calling, the TX status shows the progress ("call 2 of 4"); after the last unanswered call the panadapter listens through one more receive slot (an answer to your final call still starts the QSO normally), then stops and goes idle. Applies to every CQ run, including the automatic resume after a completed or timed-out QSO — each fresh CQ sequence starts the count over.

**Listening slot** - while you are transmitting you are deaf to your own time window, so
the occupancy picture for the window you transmit in is the one that goes stale. The
**Listen** button in the preset editor (under **CQ stop**) can spend one slot receiving
after every 3, 5 or 10 calls. Off by default, because it changes your on-air cadence, and
like CQ stop it applies on the tap with no Save needed.

### 3. Auto-Reply (Robot Mode)

⚠️ **Requires explicit checkbox in the Filter modal** to enable.

When robot mode is on and you're idle, the panadapter scans every FT8 decode for CQ callers matching your filters, picks the highest-priority match (Strongest SNR, Weakest SNR, or Most distant grid — your choice), and starts a full QSO:

1. **TX1** — send your call, grid
2. **RX** — wait for their report
3. **TX2** — send their report + your call back
4. **RX** — wait for RR73 or 73
5. **TX3** — send 73 or RR73 (exchange complete)
6. **Log** — ADIF entry created, move to next CQ

**A busy pick is abandoned, not waited for** (Roy KI0ER's reasoning adopted verbatim): when *you* pounce on a station that turns out to be mid-QSO with someone else, the Tab5 politely holds — a deliberate pounce means you want *that* station. But the robot picked its target off a list, so when its pick engages a third station the robot **moves on to a different CQ caller** instead of idling through someone else's QSO. No grey-list strike: busy is not unresponsive.

Each QSO takes **~90 seconds** (6 FT8 slots × 15 s/slot). If a station doesn't respond in 4 slots, the panadapter times out and resumes scanning CQ.

**Important:** Robot mode **transmits on the air unattended**. Never enable it unless:

- Your station is physically attended
- Your filters are tested and correct
- Your antenna/SWR is known good
- Your QMX power is set appropriately

A permanent on-screen warning appears whenever robot mode is available.

**Four things now switch it off for you**, all of them because the alternative is
transmitting when you did not expect it (Roy KI0ER):

- **It waits before its first call.** Straight after a band change or a restart
  nothing has been heard yet, so both transmit windows are unmapped and a tone
  would be chosen from no information at all. It holds, showing *"Auto-answer:
  listening before first call"*, until it has heard both windows.
- **Cancelling a transmission switches it off.** Tapping the TX indicator to stop a
  transmission disarms it, abandons the QSO **and** turns auto-answer off — so
  halting a transmission to go and check your antenna does what you expect, rather
  than the radio starting again a cycle later.
- **A band change switches it off**, whichever way you changed band — the band
  buttons, the web page, a spot, a memory recall, or the radio's own knob. Few
  stations have an automatic ATU, so the antenna is usually not tuned for the new
  band yet.
- **It is off at every startup**, however the last session ended. Unattended
  transmission is never the state the panadapter powers up in; switching it on is
  a deliberate act, once per session.

Each of these says on screen why it stood down.

### 4. FT8 Simulation Mode (Practice QSOs)

⚠️ **For practice only** — no real stations involved, radio never keys up. Since v1.3.0 the simulator needs **no QMX connected at all** — no radio, no antenna, zero RF.

Enable **FT8 Simulation Mode** in the settings drawer to practice everything: manual step-by-step Transmit, Auto Pounce, CQ-runs, pileups, and Field Day exchanges. Useful for:

- Learning the full FT8 exchange sequence
- Testing your setup without risking interference
- Verifying ADIF logging works correctly
- Practicing the pileup tools on a genuine pileup

**How it works:**
- **Six phantom stations** (three US, three DX) call CQ on their own tones — every message is real FT8, synthesized to actual GFSK audio and decoded through the same receive pipeline real RF uses
- Tap a CQ to pounce (auto or fully manual — they answer either), or **Call CQ** yourself and **four phantoms answer at once**, building a real pileup
- Phantoms are **patient like real operators**: each repeats its message every cycle, up to four times, before giving up and going back to CQing; one you're working stops CQing; one you've worked stops answering your CQs (toggle sim off/on to reset)
- Their replies match **what you actually transmitted** — grid gets a report, a report gets a roger, `RR73` gets a courtesy `73`
- The **Fast pounce** toggle is honoured, so you can watch what it changes: decodes surface just before the slot boundary with it on, just after with it off
- Swiping to the Panadapter and back clears the phantom rows and pileup for a fresh session, and **turning simulation mode off also clears them** (v1.3.3) — phantom stations no longer linger in a list that is supposed to be real, where they were still tappable

!!! note "Fixed in v1.3.3"
    With **Fast pounce** turned *off* — which is the default — the phantoms never answered at all, so every practice pounce simply timed out. If you tried the simulator before v1.3.3 and concluded the phantom stations were ignoring you, that was this, not you.

**Visual indicator:** When simulation mode is active, a **10 px red border pulses around the entire screen** (breathing red frame). This is your visual reminder that you're in practice mode — if the red frame is gone, simulation is off and real stations are in play.

**The interlock is in the firmware, not just the UI.** While simulation is on, every CAT command that would key the radio is skipped and logged instead — so a QMX that happens to be connected is never keyed, regardless of what the screen is showing.

**Important:**
- The QMX is **never keyed** in simulation mode, no matter what
- QSOs are logged as real ADIF entries — deliberately, so the logging/upload paths get exercised too. When you're done, the ADIF viewer shows a **"Del N test"** button (only while practice contacts exist — they're recognized by their missing frequency): two taps deletes them all
- All other features (panadapter, web UI, settings) work normally

### 5. ARRL Field Day Mode

During [ARRL Field Day](https://www.arrl.org/field-day), tick **Field Day mode** in the Filter modal and fill in the two fields beside it:

- **Class** — your number of transmitters plus the category letter, written together: `16A`, `5B`, `1D`. It is not a power rating.
- **Section** — your ARRL/RAC section, e.g. `EMA`, `WCF`, `NNJ`.

This switches the FT8 exchange from grid-and-signal-report to Field Day's class+section format, using the standard FT8 message type WSJT-X uses for FD (`WA9XYZ KA1ABC R 16A EMA`). Both pounce and CQ-run follow the convention automatically: the opening message is unchanged — FT8's CQ format has no room for class and section — and the *report-equivalent* step carries them instead, with the receiving side echoing back `R`-prefixed.

While the mode is on, **Call CQ** tags your CQ as `CQ FD <call> <grid>`, using the same modifier mechanism as `CQ POTA` or `CQ DX`, so other Field Day stations know to expect this exchange. It **replaces** any other modifier for as long as the mode is enabled — FT8 allows exactly one. The CQ preset editor reflects that: the three presets are dimmed and locked while FD mode is on, since their own modifier cannot apply, and a live preview line shows exactly what will go out.

Completed Field Day contacts log the standard ADIF contest fields — `CONTEST_ID=ARRL-FD`, your section and theirs — alongside the usual call, frequency and time, so they import cleanly into contest-logging software.

### 6. Fox/Hound (DXpedition) Mode

Big DXpeditions run FT8 in **Fox/Hound** mode, where one station (the *Fox*) works
five callers at a time from below 1000 Hz while everybody chasing it (the *Hounds*)
calls from above 1000 Hz. It is not the ordinary FT8 exchange: a hound that answers
the Fox has to **move onto the Fox's frequency** to do it, because the Fox listens
only to its own narrow slice.

Set **Fox/Hound (DXpedition)** in the Filter window to one of three positions:

- **Off** — normal FT8. Nothing changes.
- **Guided** — the Tab5 tells you when it can see a Fox, and you tap it to call.
  Every transmission stays your decision.
- **Automatic** — the Tab5 calls a Fox it recognises and runs the whole exchange.

Whichever you choose, the awkward part is handled for you: your first call goes out
**above 1000 Hz** on a clear slot, and the moment the Fox answers you with a report,
your reply is **moved down onto the Fox's own frequency** — which is the step that
makes the contact possible at all. The Fox's `RR73` ends it, and the Tab5 then
returns to its calling tone ready for the next one.

**The Tab5 does not send `73` to a Fox.** The Fox's frequency is the scarcest thing
on the band during a DXpedition and a courtesy `73` there is just clutter. The
contact is complete, and logged, on the Fox's `RR73`.

**How a Fox is recognised — and where it trusts you instead.** For anything it does
*by itself*, the Tab5 is strict: not frequency alone (plenty of ordinary stations
work below 1000 Hz) but a station down there visibly working a **queue** — several
different callsigns inside a few minutes, a pattern nothing else on the band
produces. Until it has seen that, automatic mode will not call anything a Fox.

**A tap from you needs no such evidence.** While Fox/Hound is enabled, tapping any
station in the Fox region runs the Fox/Hound exchange, because enabling the mode is
your declaration that you are chasing a DXpedition — the same way WSJT-X's Hound
tick works. The machine is deliberately more cautious than you are.

The catch is leaving the mode on after the DXpedition has finished: a tap at an
ordinary station low in the passband would then QSY onto their frequency and skip
your `73`. So the transmit-confirmation window **tells you before you commit** —
it adds a line reading *"as HOUND — will QSY onto 500 Hz, no 73 sent"* whenever
Fox/Hound rules are about to apply. If you see that on a station you did not think
was a DXpedition, set Fox/Hound back to **Off**.

Three of the Tab5's usual courtesies stand down while a Fox contact runs, and all
three would otherwise work against you:

- it **keeps calling** while the Fox is busy with other stations — a Fox is always
  busy, and waiting for a free frequency means never calling at all;
- it **does not re-send** a closing message, because the Fox never asks for one;
- it **does not grey-list** the Fox for ignoring you, which in a pileup of hundreds
  is entirely normal.

Automatic mode will not call a Fox already in your log on that band, so it works one
contact and then leaves the DXpedition alone.

!!! note "Hound only — the Tab5 cannot be a Fox"
    A Fox transmits up to five signals simultaneously. The Tab5 keys the QMX one
    tone at a time over CAT, so a multi-signal transmission is not something this
    radio can be asked for. This is a hardware limit, not a missing feature.

### 7. Message Status

The FT8 left pane shows a **status label** indicating what's happening:

| Status | Meaning |
|---|---|
| **ACTIVE** (red) | Transmitting right now |
| **ARMED** (amber) | Ready to transmit next slot |
| **QSO Complete** (green) | Exchange finished, logged to ADIF |
| **QSO Timeout** (orange) | No response after 4 slots, exchange aborted |
| *(dim white)* | Idle, no active QSO |

Tap the status label to **abort** the current QSO (only works if ARMED or ACTIVE; once COMPLETE, it's logged).

**Resume after timeout** — if a QSO times out because the partner faded, and they come back within ~5 minutes calling you, the exchange **resumes where it left off** automatically (or tap their row to resume manually) instead of restarting from scratch.

**If they never heard your final** (v1.3.4) — a partner who does not decode your closing `73`/`RR73` keeps sending you their report, waiting for it. The Tab5 now notices: if the station just worked comes back with a report rather than `RR73`/`73`/`RRR`, the final is sent again, up to three times within four minutes, *before* anything else can start a new contact. The QSO is not logged a second time. Taking over by hand no longer produces a duplicate entry either — the same callsign on the same band inside ten minutes is recognised as the same contact.

### 8. Power & SWR Readout

After each transmit burst, the status bar briefly shows:

```
Last TX: 5.2W SWRx1.25 [N=79]
```

- **5.2W** — average output power measured via QMX `PC;` CAT command
- **SWRx1.25** — SWR measured via QMX `SW;` CAT command (only valid during TX)
- **[N=79]** — number of symbols transmitted
  - FT8: always 79 symbols (~12.7 s at 160 ms/symbol)
  - FT4: always 105 symbols (~5.0 s at 48 ms/symbol)

These readings are **informational only** — the panadapter does not enforce limits. Monitor your antenna system independently.

### 9. CQ Presets

You can save up to 3 custom CQ messages:

1. Tap **Call CQ**
2. Tap **Edit Presets** (or long-press the active preset)
3. Type your message: callsign, modifier (optional), grid
4. Tap **Save**

Presets persist across power cycles. Common modifiers:

- **DX** — calling DX only
- **POTA** — Parks on the Air activation
- **SOTA** — Summits on the Air activation
- *(blank)* — standard CQ

The preset editor's top-right **CQ stop** button sets the CQ auto-stop limit (see [Call CQ](#2-call-cq) above) — it cycles never / 1–5 / 10 and applies on tap, independent of Save/Cancel.

### 10. Frequency & Tone Control

Your transmit tone is chosen for you by default — the nearest clear 50 Hz slot, scanned against the stations currently decoded. You can see it, move it, and from v1.3.4 pin it.

**The TX tone button** is in the FT8 left pane, to the right of the `TXCQ` parity button, and always shows a number (1500 Hz until you change it). Tap it to open the picker.

**The mini occupancy strip** sits directly under the slot countdown in the same pane: the same 50 Hz grid and the same colours as the picker's full-size strip, so where the band is busy — and where you are in it — is answerable at a glance without opening anything.

**Both time windows, always** (asked for by Roy KI0ER). The strip is **two rows — EVEN above, ODD below**, marked `E` and `O` in the same blue/orange the slot countdown uses. Two stations only collide if they transmit in the *same* window, and only you know which window you are about to pounce into — so both pictures are on screen **before** you choose, not after a transmission has fixed your window. Your white marker sits on your own window's row once one is locked (both rows until then), and your partner's pink sits in *their* window. The picker's full-size strip is split the same way, and its verdict says where your pick stands: *"Clear in EVEN — busy in ODD"*.

In the picker:

| Control | What it does |
|---------|--------------|
| **Occupancy strip** | The whole 200-2800 Hz window as 52 slots. **Green** free, **red** occupied, **white** you, **pink** your QSO partner |
| **Touch and drag** | Drag along the strip to pick a slot. The bar turns grey and follows your finger, the readout tracks it live, and it commits when you lift off |
| **-50 / +50** | Nudge one slot at a time |
| **Find clear slot** | Jumps to the nearest free slot, scanning outward from where you are |
| **TX Hold** | Pins the tone: every CQ and every reply goes out on it. See below |
| **Apply nnnn Hz** | Commits the tone and the hold setting |

The readout states in words whether your chosen slot is clear or occupied, and lists the nearest free slots as numbers.

**Apply whenever you like, including in the middle of a transmission.** If a burst is on the air the change is accepted and applied the moment that burst ends — the transmission in progress finishes on the old tone, because stopping it half way would send a corrupted frame, and the very next message carries your new one.

That used to be a refusal, and it mattered more than it sounds (Roy KI0ER). A burst covers about 12.6 s of a 15 s slot and you transmit every other slot, so roughly four attempts in ten landed mid-burst and were rejected — leaving the QSO running on the offset it started with. Your choice depended on your timing, which is not a choice.

Slot parity is untouched, so moving your tone mid-QSO does not disturb the exchange — your partner tracks the slot, not the frequency. Nothing in FT8 requires either station to stay on the offset it began on, only to stay in its time window. This is the same freedom WSJT-X gives you, and it is the way out from under a station that has landed on top of you.

**TX Hold** is WSJT-X's "Hold Tx Freq". With it **off** — the default — each transmission takes the nearest clear slot, and a CQ that gets clashed relocates itself on the next cycle. With it **on**, the tone you picked is the tone used for everything, a clash is reported but never acted on, and nothing moves you off the slot you chose. The line under the checkbox says which of the two you are getting. Both the tone and the hold setting survive a power cycle.

!!! note "The strip shows decoded stations, not raw spectrum"
    A station too weak to decode will not appear as occupied, and an all-grey strip means nothing has been heard yet rather than "the band is clear".

!!! note "Occupancy is per time window"
    Two stations only collide if they transmit in the *same* slot, so a tone busy in the odd window may be completely free for you in the even one. The strip accounts for this whenever your own transmit window is known — that is, once a reply or a CQ is armed, since a reply inherits the opposite of your partner's slot and a CQ carries your `TXCQ EVEN`/`ODD` choice. With `TXCQ ANY` and nothing armed there is nothing to work from, so both windows are shown combined. When the window *is* known, the free-slot line names it.

### Waiting for a busy station

On a crowded band several stations answer the same CQ and the caller works one of them. If that is not you, the Tab5 no longer keeps calling: while their last decoded message is addressed to somebody else, **nothing is transmitted**, and the status line shows `working <call> - waiting`. As soon as they send `73` or `RR73`, or call CQ again, it picks up where it left off.

**You are not committed to the wait** (Roy KI0ER's report). The status line carries a **TAP TO CANCEL** line while the hold is on; tapping it drops the pounce so you are free to work somebody else. The abandoned exchange stays resumable for a few minutes, in case the station frees up and you want back in.

This also protects the grey-list. Giving up on a station used to count against it, so a popular station could end up permanently skipped by the robot and Auto-work-pileup for no reason other than being busy. A wait is not a failed attempt. The wait is capped at about six minutes so a station that vanishes mid-exchange still times out normally.

To tune to a different frequency while CQ is running:

1. Tap the **Freq** label on the top bar
2. Enter a new frequency
3. The CQ continues on the new frequency next slot

### 11. ADIF Logging

Every completed QSO is **automatically logged to ADIF** on the Tab5's internal storage:

- **Callsign**, grid, report (SNR), time (UTC)
- **Frequency**, mode (`FT8`)
- **Duration** (QSO start to finish)
- **Operator** (your callsign)

Download the log via the web UI (**QSO Logs** menu → **ADIF download ↓**) or Settings → ADIF Log.

### 12. Upload to QRZ, eQSL & LoTW

Via the web UI:

1. Open the **QSO Logs** menu in the bottom bar and click **QRZ upload ↑**, **eQSL upload ↑**, or **LoTW setup** (reads **LoTW ↑** once configured)
2. Enter your API key (QRZ), username/password (eQSL), or run the guided certificate setup (LoTW) when first prompted — credentials are saved for future sessions
3. Click again to upload

See [Web UI → LoTW Upload](web-ui.md#lotw-upload) for the LoTW certificate setup details.

Logs are batched — each upload session records which QSOs have been sent, so re-running skips already-submitted entries.

Uploads work **while FT8 or FT4 is actively running** — the panadapter briefly steps the FFT and SD-archive activity aside for the duration of the HTTPS transfer, then resumes automatically. You typically miss one FT8 slot and won't notice. A progress result is shown once the upload completes.

### 13. Troubleshooting

**"QMX not responding to TX command"**

- Check USB cable (must be data cable, not charge-only)
- Verify QMX is in the correct mode (USB/LSB/CW/DiGi, not AM)
- Try a manual `TX;` command via the web UI's CAT tab

**"⚠ FREQ BUSY warning, but no other station is there"**

- The clash detector reserves a ±50 Hz guard band around each decoded station, so it flags a neighbour that is close rather than exactly on you
- It counts only stations **in your own transmit window** (it used to count both windows, so it could warn about a station that could never collide with you; Roy KI0ER caught it)
- The warning names the frequency. Tap **TX nnnn Hz** and either **Find clear slot** or drag to a green slot on the occupancy strip
- Remember the strip only knows about stations it has *decoded* — a very weak one will not show

**"QSO timeout — no response"**

- The called station didn't hear you (too weak, bad timing, etc.)
- Try a different station, or switch to manual mode and check levels first
- See [Troubleshooting](../reference/troubleshooting.md) for more

---

**Next:** Set up [Time Sync](time-sync.md) for accurate FT8 timing, or explore the [Web UI](web-ui.md).
