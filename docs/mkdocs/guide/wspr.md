# WSPR

**WSPR** (Weak Signal Propagation Reporter, pronounced "whisper") answers one question: *where does my signal actually go?* You transmit a very short, very slow beacon carrying only your callsign, grid and power — and stations all over the world that hear it report the fact. Over a few hours you get a map of what your antenna and your band are really doing, at power levels where nothing else would be heard at all.

It is not a contact mode. Nobody replies, there is no exchange, and nothing goes in your log. That is the point: it measures propagation instead of working people.

Swipe → from the left edge to cycle **Panadapter -> FT8/FT4 -> WSPR** and back.

---

### 1. The two-minute rhythm

Everything in WSPR is built on a **two-minute cycle**, aligned to UTC. A transmission starts a second or two after each even minute and lasts about 110 seconds, so one cycle carries exactly one beacon.

That slow rhythm is why the page looks calm compared with the FT8 screen. There is no countdown urging you on and no QSO furniture, because there is nothing to answer. The countdown at the top left simply tells you where you are in the current window.

**Your clock has to be right.** WSPR's decoder searches only a narrow slice of time either side of the cycle boundary, so a clock that is out by more than a couple of seconds will hear nothing and be heard by nobody. The Tab5 keeps itself right from SNTP over WiFi, from the QMX's own clock, or from its internal RTC — see [Time Sync](time-sync.md). If you are running with no network and no GPS, check the clock before you trust an empty screen.

---

### 2. Reading the page

The left pane is a log, not a live list of who is on frequency. Spots stay where they are and new cycles are added — an opening band looks different from a closing one only over time, so the page is arranged to let you see that.

| Column | Meaning |
|---|---|
| **S** | The letter this station is marked with on the waterfall for that cycle — see [Which trace is which](#which-trace-is-which) below. Blank once the cycle it belongs to has scrolled out of the picture |
| **UTC** | The cycle this spot came from |
| **CALL** | The station heard |
| **GRID** | Their Maidenhead locator, as transmitted |
| **COUNTRY** | Country from the callsign prefix, spelled out where it fits and shortened where it does not - never a 3-letter code |
| **BND** | The band it was heard on, in metres. Blank for spots recorded before v1.10.5, and worth having the moment band hopping is on |
| **PWR** | The power **they declared**, not a measurement |
| **SNR** | Signal-to-noise, in the WSPR convention (a 2500 Hz reference — figures around −25 dB are entirely normal and perfectly decodable) |
| **TONE** | Where in the 200 Hz sub-band they were heard |
| **DR** | Drift: how far the sender moved during the transmission. A stable, oven- or GPS-referenced transmitter reads +0; a crystal warming through the burst reads +1 or -2. Whole Hz, which is what wsprnet publishes, and a coarser measurement than TONE beside it - it is the difference between two half-window estimates, so read a trend across several spots rather than one |
| **DT** | How far into the two-minute cycle that transmission actually started, in seconds. **Nominal is +1.0**, because a WSPR transmission begins one second into its even minute — so a value far from that is the other station's clock rather than anything at your end |
| **KM** | Great-circle distance from your grid. The heading reads **MI** if you have chosen miles — see [Settings](settings.md). A leading `~` means the distance came from the station's country rather than its grid |

Below the list:

- **DX** — the furthest station of the session, which is usually the number you actually want.
- **WSPRNET** — whether spots are being published, and how many can be. **"N of M publishable"** is the publication gate: a station is only sent to wsprnet once it has been heard **more than once**, so N is how many of the M calls heard so far are eligible. This is also why a site like wspr.rocks can show fewer unique calls than this page reports hearing — it only ever received the publishable ones.
- **Flush** — empties the decode list. It asks first, because the list is also the upload queue: anything not yet sent to wsprnet goes with it, and the publishable count resets.

!!! tip "With a mouse, point at a trace to see whose it is"

    Hovering a trace on the waterfall names the station, its tone and its signal
    report, beside the pointer. It matches within about half a WSPR signal's
    width, so you have to be on the trace rather than near it. There is no hover
    on a touchscreen, so this adds to the list rather than replacing it —
    everything it shows is in the table underneath.

The right pane shows the captured 200 Hz window for the cycle just decoded. WSPR's whole sub-band is narrower than a single FT8 signal, so this is a very close-up view: individual beacons appear as near-horizontal lines, and a sloping line is a drifting transmitter.

**On a cycle you transmit, the waterfall does not advance** — the receiver is stood down for the whole two minutes, so there is nothing to draw. Rather than leave the previous cycle's picture sitting there looking frozen, the display lays down its cycle-boundary marker and keeps the last received image below it, so you can still see what was there before you transmitted. The status line reads **transmitting** throughout.

**The panadapter spectrum is not available while WSPR runs.** The receiver takes the IQ stream for the whole cycle, so there is nothing left to draw a live spectrum from. This is expected, not a fault.

#### Which trace is which

Each time a cycle finishes, the traces the decoder examined are lettered on the
waterfall — **A, B, C… from left to right by tone** — and the cycle's UTC time is
printed at the left end of the row. The same letter appears in the list's **S**
column, so a line of text and a mark on the carpet are the same station, and you
can tell at a glance which of five traces produced the report you are reading.

The letters describe the cycle **below** the boundary line they sit under, since
that is the cycle that just finished. They scroll down the carpet with it and are
gone once it leaves the picture, which is why an older row's **S** column is
blank.

- **A `?` is a trace the decoder tried and could not read.** That is worth
  seeing: it is the difference between "nothing was there" and "something was
  there and was too weak", which is the whole question when you are judging an
  antenna or a band. It usually means a signal a few dB below the decode
  threshold, but a birdie or a carrier sitting in the sub-band will also collect
  one.
- **Not everything gets a mark.** The candidate finder always returns a full
  quota of possibilities, most of which are its own noise floor rather than
  signals, so a `?` is only drawn for a candidate well above the middle of that
  cycle's crop. Expect a handful, not a row of punctuation.
- **A mark that cannot be placed honestly is left out.** Where several traces are
  too close together to letter separately, decoded stations are placed first and
  the rest are dropped rather than nudged aside — a marker pointing at the wrong
  trace is worse than no marker.
- The letter is drawn a couple of Hz above the tone in the table. A WSPR
  transmission is four tones about 4.4 Hz wide and the reported frequency is the
  lowest of them, so the mark sits on the middle of the trace rather than its
  left-hand edge.

---

**Changing band** — tap the button at the top of the left pane and a list drops
below it. Touch a line and it lights up; if it is the wrong one, keep your finger
down and drag until you are on the one you want, then lift to choose it.
Releasing outside the list chooses nothing. The band you are on is named in
amber.

!!! warning "Use *this* button, not the top bar"
    On the WSPR page the top bar's **Band**, **Mode**, **BW** and **Zoom** are
    greyed out and do nothing: they belong to the panadapter's idea of the radio,
    while this page owns the dial. Only **Freq** stays live, so a beacon found off
    the standard dial frequency can still be chased by typing it in.

    On **v1.12.0 only**, the top bar's Band control overlapped this panel and took
    the touch — picking a band from it wrote an FT8 frequency to the radio while
    WSPR carried on capturing and labelling its spots with the band it thought it
    was on. If you used the WSPR page on that release, check which band your radio
    is actually on.

---

### 3. Settings

All of WSPR's settings live in the **settings drawer** (swipe ← from the right edge) under **WSPR**, and appear only while the WSPR page is up. The page itself keeps just one control — the **TX** button — because that is the only one you reach while a session is running.

#### Transmitting is the TX button, and nothing else

There is no separate "allow transmitting" setting. The **TX** button on the page is the only control, so there is nothing that can disagree with it.

**It is off every time you open the WSPR page**, whatever you did last session. A beacon that resumes on its own because of how you left it a week ago is not a decision you made, so the page always starts as a pure receiver — which is a perfectly good way to use WSPR — and transmitting is something you switch on deliberately each time.

**Your callsign and grid must be set**, in **Station -> Callsign & Grid square**. Without them there is no transmission at all — the same rule FT8 follows. WSPR sends your callsign to every station that hears you and publishes it to a public database, so it uses the identity you entered and nothing else.

#### Calibrate Power

Before Declared power can offer anything real, the current band needs calibrating. **Calibrate Power** sweeps the QMX's *Max. PA voltage* on a **dummy load** — not the antenna — and records the real RF output at each step with the radio's own `PC;` readback. The result is saved for that band and used from then on.

*(v1.14.4)* The sweep **stops at your own Max. PA voltage**, and the window shows that figure before you start — so a 9 V QMX, or one you have deliberately limited, is never driven past the setting you chose. It also **stops early once the measured power stops rising**, which is what happens above your supply voltage: on an 8 V supply every step above ~8 V would otherwise re-measure the same watts for minutes of pointless key-down. Bruce N9JCV found both.

Because the sweep can end early, a shorter results table is normal rather than a failure — the window says which of the two limits ended it.

You reach it two ways: the **"Calibrate this band"** button that appears in place of Declared power (or Output power, on other modes) whenever the current band has no data yet, or **"Recalibrate this band"** in the same place once it does — for redoing a band after a change to your antenna or feedline.

#### Declared power

This is **a claim, not a measurement** — every station that hears you publishes it worldwide, where other operators use it to reason about propagation. But once the band is calibrated, the dropdown only offers the standard WSPR dBm steps your radio actually reaches, and each one is labelled with the **real measured wattage**, not the textbook figure the step's name implies. Picking "33 dBm" no longer means finding out afterwards that the radio actually did something else.

**This is also where you protect the finals now.** WSPR transmits for about **110 seconds out of every 120** — nothing else this radio does comes close, and QRP Labs warn about exactly this kind of sustained duty cycle in the QMX manual. There is no separate switch for it any more: picking a low declared level (a few hundred milliwatts is plenty to be heard) keeps the finals cool for the length of a beacon run, the same way choosing any other setting on this dropdown does. Anything above 1 W gets a plain warning label, rather than a guard acting on your behalf underneath you.

**During each transmission the Tab5 asks the radio what it actually put out**, and shows the answer under the dropdown — *"radio measured 1.6 W last burst = 32 dBm"* — so you can see whether a burst matched what you declared.

**Switching transmit off stops a burst that is already on the air.** The **TX** button keys the radio down at once rather than waiting for the two-minute cycle to end.

#### Output power

A separate slider, filed next to Calibrate Power, sets the radio's output for every mode **other** than WSPR — FT8, CW, SSB, and so on. It shares the same per-band calibration but is a genuinely different control: WSPR's Declared power always owns the radio while WSPR is running, so the Output power slider doesn't appear on this page at all. Leaving WSPR hands control back to it immediately.

#### Running WSPR from a lower supply voltage

**A low declared power reduces the heat in the finals. It does not remove it from the radio.** The QMX limits PA voltage with a pass transistor, so the difference between your supply and the PA voltage in force is dropped inside the radio as heat instead — measured on a bench QMX, holding the PA at 6 V against a 12 V supply cut the finals' own heat by 76% but total heat by only 18%. That trade is worth making — the PA transistors are the fragile, hard-to-replace part — but it is not the whole answer.

**If you intend to beacon on WSPR for hours, feed the QMX from a lower supply.** The QMX accepts **6.0 to 12.0 V**, and running it at around 9 V means less voltage to throw away as heat anywhere in the radio. This is the one thing that helps which no firmware setting can do for you.

#### Transmit schedule *(changed in v1.14.5)*

Two numbers describe one repeating group: **Transmit cycles**, then **Receive cycles**.

Set transmit to 2 and receive to 8, and the beacon transmits for two cycles, listens for eight, and repeats for ever. The period is simply the two added together — ten cycles, twenty minutes — and the Tab5 spells it out under the two controls as you change them:

> Transmit 2, then listen 8 — repeating every 20 min. Transmitting 20% of the time.

| Transmit | Receive | Pattern, repeating | Period |
|---|---|---|---|
| 1 | 4 | Tx Rx Rx Rx Rx | 10 min |
| 1 | 9 | Tx Rx ×9 | 20 min |
| 2 | 8 | Tx Tx Rx ×8 | 20 min |
| 2 | 3 | Tx Tx Rx Rx Rx | 10 min |

Setting transmit to **Receive only** never keys the radio, and greys out the receive count — with no transmission there is no group to space out.

**Why two numbers and not a duty cycle.** This used to be "1 in N" plus a separate "bursts per transmission", and that pair had no agreed meaning. "1 in 5" plainly means one cycle in five, and with a single burst that is exactly what it did — but ask for two bursts and the phrase stops saying anything. Does the group grow into the listening time, or is the listening time kept? Two people can read it two ways, and did. Two counts cannot be read two ways.

**Your existing setting is carried over exactly.** A Tab5 upgrading from v1.14.4 or earlier keeps the schedule it was already running, cycle for cycle — the old "1 in 5" becomes 1 transmit and 4 receive, which is the same thing said plainly.

**Transmitting more means keying the finals more.** Two cycles in a row give a distant receiver a second chance at you when the first falls in a fade, which is why the QMX's own beacon offers it — and it doubles your share of key-down time. Pick the declared power to match; see **Declared power** above.

#### Band hopping

Tap **Choose bands…** and tick the bands you want. **Ticking two or more turns hopping on**; leaving one ticked keeps you on that band. There is no separate on/off switch, because the list of bands already says what you want.

Only the bands your radio can actually reach are offered. A QMX is built with a fixed set of filters, so the list is asked of the radio rather than assumed — the bench QMX offers 60/40/30/20/17/15 while a QMX+ covers 160–6 m.

Hopping changes band **between** cycles, never during one, and the waterfall's noise floor is reset on each hop so a quiet band is not painted against the last one's noise.

#### Publish spots to wsprnet

Off by default. When on, the stations you hear are uploaded to **wsprnet.org**, where they join the public database. This is how WSPR is useful to anyone other than you: your receiver becomes one of the reporting stations that lets other operators see where *their* signals went.

It needs WiFi and your callsign and grid.

---

### 4. Transmitting

With a callsign and grid set and a transmit count other than **Receive only**, the **TX** button on the page arms the station. Which cycles actually transmit is decided by the transmit schedule, and the button shows what is happening.

**The countdown on the button is the time until a real transmission.** The schedule is worked out in advance, so `TX ON next 6:14` means a burst is coming in six minutes and fourteen seconds — not that a cycle boundary is due and might or might not be used.

A few things worth knowing before you leave it running:

- **Your radio is keyed for real,** for about 110 seconds at a time. Make sure it is connected to an antenna or a dummy load, and that the power it is producing matches what you declared.
- **SWR protection still applies.** If the SWR limit in **Radio -> SWR protection** is exceeded, transmitting stops.
- **The Tab5 wakes up on the page you left it on, but not transmitting.** It returns to the WSPR page after a power cycle and starts receiving; the **TX** button is off, so it will not resume beaconing on its own. Switch it on again when you are ready.
- **Split stops it, and says so.** WSPR is transmitted on the dial frequency, and
  the spot you publish names that frequency. If your radio is in split it keys VFO
  B while still reporting VFO A, so every spot would name somewhere the signal
  never was. Before each transmission the Tab5 asks the radio, and if split is on
  it holds the burst and shows **TX held - radio is in SPLIT, clear VFO B**. It
  will not clear it for you: that is your setting, and on the QMX split cannot be
  cleared over the cable anyway — use the radio's own menu or power-cycle it.
- **Simulation mode blocks every byte.** If you want to watch the mechanics without keying anything, turn on **FT8 Simulation Mode** in the drawer; it interlocks WSPR TX as well.

---

### 5. From the browser

The web UI mirrors whatever page the Tab5 is on. Switch it to WSPR with the **Switch to WSPR** link in the bottom bar, and the browser shows the same page the Tab5 does: a **MODE: WSPR** heading, the band selection, the **TX** button with its countdown, the finals-guard voltage, best DX, stations heard per cycle, the wsprnet publishing status — and the table of spots.

The table is shown only while the Tab5 is on the WSPR page, because that is the only time the receiver is running — on any other page it would be a frozen list that looked live.

Transmitting from the browser uses your stored callsign, grid and declared power. They cannot be overridden per request: what goes on the air is the identity you set on the device.

---

### 6. If nothing is decoded

WSPR is quiet by nature — an empty screen for one cycle means very little. Before assuming a fault:

- **Check the clock.** The bottom bar shows the time source. A clock more than a couple of seconds out will decode nothing.
- **Check the band and the hour.** WSPR follows propagation; 20 m at midnight will be emptier than 40 m.
- **Check the dial.** Each band has exactly one WSPR sub-band, and the picker only offers those — but if you tuned the radio by hand afterwards, you may not be on it.
- **Give it several cycles.** At two minutes each, four cycles is eight minutes. That is a normal amount of patience for this mode.

See also [Troubleshooting](../reference/troubleshooting.md).
