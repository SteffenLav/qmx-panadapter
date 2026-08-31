# WSPR

**WSPR** (Weak Signal Propagation Reporter, pronounced "whisper") answers one question: *where does my signal actually go?* You transmit a very short, very slow beacon carrying only your callsign, grid and power — and stations all over the world that hear it report the fact. Over a few hours you get a map of what your antenna and your band are really doing, at power levels where nothing else would be heard at all.

It is not a contact mode. Nobody replies, there is no exchange, and nothing goes in your log. That is the point: it measures propagation instead of working people.

Swipe → from the left edge to cycle **Panadapter → FT8/FT4 → WSPR** and back.

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
| **UTC** | The cycle this spot came from |
| **M** | The band it was heard on, in metres. Blank for spots recorded before v1.10.5, and worth having the moment band hopping is on |
| **CALL** | The station heard |
| **GRID** | Their Maidenhead locator, as transmitted |
| **COUNTRY** | Country from the callsign prefix |
| **SNR** | Signal-to-noise, in the WSPR convention (a 2500 Hz reference — figures around −25 dB are entirely normal and perfectly decodable) |
| **DRF** | Drift, in Hz per minute. A stable transmitter reads 0 |
| **TONE** | Where in the 200 Hz sub-band they were heard |
| **PWR** | The power **they declared**, not a measurement |
| **KM / BRG** | Great-circle distance and bearing from your grid |

Below the list:

- **DX** — the furthest station of the session, which is usually the number you actually want.
- **HISTORY** — stations per cycle, oldest on the left. A single snapshot cannot tell an opening band from a closing one; a row of bars can.
- **WSPRNET** — whether spots are being published, and whether they can be.

The right pane shows the captured 200 Hz window for the cycle just decoded. WSPR's whole sub-band is narrower than a single FT8 signal, so this is a very close-up view: individual beacons appear as near-horizontal lines, and a sloping line is a drifting transmitter.

**On a cycle you transmit, the waterfall does not advance** — the receiver is stood down for the whole two minutes, so there is nothing to draw. Rather than leave the previous cycle's picture sitting there looking frozen, the display lays down its cycle-boundary marker and keeps the last received image below it, so you can still see what was there before you transmitted. The status line reads **transmitting** throughout.

**The panadapter spectrum is not available while WSPR runs.** The receiver takes the IQ stream for the whole cycle, so there is nothing left to draw a live spectrum from. This is expected, not a fault.

---

### 3. Settings

All of WSPR's settings live in the **settings drawer** (swipe ← from the right edge) under **WSPR**, and appear only while the WSPR page is up. The page itself keeps just one control — the **TX** button — because that is the only one you reach while a session is running.

#### Allow transmitting

Off by default. Turning it on lets the station beacon; turning it off makes the Tab5 a pure receiver, which is a perfectly good way to use WSPR.

**Your callsign and grid must be set**, in **Station → Callsign & Grid square**. Without them there is no transmission at all — the same rule FT8 follows. WSPR sends your callsign to every station that hears you and publishes it to a public database, so it uses the identity you entered and nothing else.

#### Declared power

This is **a claim, not a measurement.** The Tab5 has no way to know what your radio is actually delivering into your antenna, so it transmits the number you choose here — and every station that hears you publishes it worldwide, where other operators use it to reason about propagation.

Set it to what your transmitter really produces. A QMX running 200 mW that declares 5 W does not look like a better station; it puts wrong data into everybody else's analysis.

**The Tab5 helps you get it right.** During each transmission it asks the radio what it is actually putting out, and shows the answer under the dropdown — *"radio measured 1.6 W last burst = 32 dBm"*. Switching **Protect finals** on or off also moves the declared figure to the value that setting normally produces, as a starting point. Both are suggestions: the number is a statement about your station and stays yours to choose.

#### Protect finals

**This matters more on WSPR than anywhere else, and it is on by default.**

WSPR transmits for about **110 seconds out of every 120**. Nothing else this radio does comes close to that — an FT8 transmission lasts about 12 seconds. Running a QMX flat out on that cycle puts real, sustained heat through the PA transistors, and QRP Labs warn about exactly this in the QMX manual: *"High supply voltages can stress the PA transistors, particularly when you are using Digi Modes with high duty cycle."*

With **Protect finals** on, the Tab5 turns the radio down for as long as WSPR transmit is enabled — it sets the radio's own **Max. PA voltage** to about 6 V, and puts your setting back afterwards. This is the same precaution the QMX applies to its own built-in WSPR beacon.

Measured on a QMX at 12 V:

| | Output | Heat in the finals |
|---|---|---|
| Protection off | 5.4 W | 4.7 W |
| **Protection on** | **1.6 W** | **1.1 W** |

**The finals run 76% cooler.** The button says which state you are in — green *"ON - about 1 W"*, or red *"OFF - FULL POWER, finals at risk"* — and turning protection off takes two deliberate taps, while turning it back on takes one. When it is off, the TX block on the WSPR page reads **FULL PWR** in red so you cannot leave the station running unprotected without knowing.

#### Running WSPR from a lower supply voltage

**Protection reduces the heat in the finals. It does not remove it from the radio.**

The QMX limits PA voltage with a pass transistor, so when your supply is 12 V and the PA is held at 6 V, the difference is dropped inside the radio as heat instead. In the measurements above, total heat fell only 18% even though the finals' share fell 76%. That trade is worth making — the PA transistors are the fragile, hard-to-replace part — but it is not the whole answer.

**If you intend to beacon on WSPR for hours, feed the QMX from a lower supply.** The QMX accepts **6.0 to 12.0 V**, and running it at around 9 V means less voltage to throw away as heat anywhere in the radio. This is the one thing that helps which no firmware setting can do for you.

#### Duty cycle

How much of the time you are willing to transmit, as a fraction of cycles: **0%** (receive only), **10%**, **20%**, **33%** or **50%**. Each cycle is decided independently at random, which is deliberate — a fixed pattern would have you transmitting in step with everyone else who chose the same setting.

WSPR convention is to transmit a minority of the time and listen the rest. 20% is a reasonable default; 50% is a lot on a shared, very quiet sub-band.

#### Band hopping

Tap **Choose bands…** and tick the bands you want. **Ticking two or more turns hopping on**; leaving one ticked keeps you on that band. There is no separate on/off switch, because the list of bands already says what you want.

Only the bands your radio can actually reach are offered. A QMX is built with a fixed set of filters, so the list is asked of the radio rather than assumed — the bench QMX offers 60/40/30/20/17/15 while a QMX+ covers 160–6 m.

Hopping changes band **between** cycles, never during one, and the waterfall's noise floor is reset on each hop so a quiet band is not painted against the last one's noise.

#### Publish spots to wsprnet

Off by default. When on, the stations you hear are uploaded to **wsprnet.org**, where they join the public database. This is how WSPR is useful to anyone other than you: your receiver becomes one of the reporting stations that lets other operators see where *their* signals went.

It needs WiFi and your callsign and grid.

---

### 4. Transmitting

With **Allow transmitting** on, a callsign and grid set, and a duty cycle above 0%, the **TX** button on the page arms the station. Each cycle is then decided by the duty cycle, and the button shows what is happening.

A few things worth knowing before you leave it running:

- **Your radio is keyed for real,** for about 110 seconds at a time. Make sure it is connected to an antenna or a dummy load, and that the power it is producing matches what you declared.
- **SWR protection still applies.** If the SWR limit in **Radio → SWR protection** is exceeded, transmitting stops.
- **The Tab5 wakes up on the page you left it on.** If you leave it on WSPR with transmitting enabled, it resumes beaconing after a power cycle — including after an unexpected one, with nobody present. That is what a beacon is for, but it is worth knowing before you leave the shack.
- **Simulation mode blocks every byte.** If you want to watch the mechanics without keying anything, turn on **FT8 Simulation Mode** in the drawer; it interlocks WSPR TX as well.

---

### 5. From the browser

The web UI mirrors whatever page the Tab5 is on. Switch it to WSPR with the **Switch to WSPR** link in the bottom bar, and the spot table appears in the browser as well.

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
