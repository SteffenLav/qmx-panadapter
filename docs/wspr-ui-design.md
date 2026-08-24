# WSPR UI — design

Phase 3 of `wspr-scope.md`. This is the reasoning **before** the code, written
because the operator asked for a thought-through UI rather than an FT8 screen
with the words changed. Where a decision looks arbitrary, the reason is given;
where something is deliberately absent, that is stated too, because on this
project a missing control is otherwise read as an oversight and re-added later.

## The five facts every decision below comes from

1. **The rhythm is two minutes, not fifteen seconds.** A WSPR transmission is
   110.6 s inside a 120 s window, on even UTC minutes only. Nothing at all
   happens for most of that time.
2. **It is a one-way beacon.** No QSO, no reply, no exchange, no state machine.
   Nothing in `ft8_qso.c` has an analogue here.
3. **Most stations only listen.** Transmitting is a duty-cycle choice - a
   percentage of slots - not a response to anything.
4. **The message is fixed**: callsign + 4-character grid + power in dBm. All
   three are already settings. There is nothing to compose and no editor.
5. **The result the operator most wants is not on this device.** "Who heard me"
   lives on wsprnet.org. The screen can show who *we* heard; the other
   direction needs the network (see "The missing half", below).

## Tab5 screen

A **new screen**, sibling to Panadapter / FT8 / (CW, on its own branch) - not a
mode bolted onto the FT8 page. It gets `UI_MODE_WSPR = 3`, deliberately taking
3 rather than the 2 the CW branch already uses, so the two feature branches do
not collide when they meet.

Layout reuses the FT8 screen's proven split, because operators already know it
and because it is the shape that works at 1280×720: a ~305 px control pane on
the left, the list filling the rest.

### Left pane, top to bottom

```
MODE: WSPR                 gold, large - matches "MODE: FT8"
+-------------------------+
| Dial: 14.095600 MHz     |  boxed, tappable -> standard-dial picker
+-------------------------+
  RX  1:23                   the 2-minute cycle, in words
  [========------------]     one bar, 120 s, RX blue / TX orange
+-----------+-------------+
| TX   OFF  | Duty   20%  |  two controls, side by side
+-----------+-------------+
  Heard 12 stations
```

**The dial is a picker of standard WSPR frequencies, not a free-entry field.**
Every band has one canonical WSPR dial (20 m is 14.095600, 30 m 10.138700, and
so on). A station on any other frequency is simply not in the sub-band and will
be heard by nobody. A free-entry keypad here would offer the operator a way to
be silently wrong, which is exactly the class of error CLAUDE.md keeps
recording; a list of the ten real ones cannot be wrong.

**The cycle bar replaces FT8's slot bar and means something different.** FT8's
bar counts a 15 s slot and the operator glances at it constantly. WSPR's counts
120 s, so its job is not urgency but *orientation*: am I receiving, is a
transmission coming, how long. One bar, coloured by what the current slot is
doing, with the plain-language line above it (`RX 1:23`, `TX in 3:47`,
`TRANSMITTING 0:42`).

**TX is off by default and says so in one word.** A WSPR transmission keys the
radio for 110 seconds - eight times an FT8 burst. This project already has a
rule for controls that key the radio, written for SWR Tune: they must be
impossible to trigger by accident and visibly ACTIVE while engaged. So TX is a
labelled toggle rather than a one-tap action, and while a burst is running the
whole block turns `UI_COLOR_TX_ACTIVE` orange and counts down. The operator
should never have to wonder whether their radio is transmitting.

**Duty cycle, not a transmit button.** WSPR's convention is "transmit in this
fraction of slots, at random", so the control is a cycling value
(off / 10 / 20 / 33 / 50 %) rather than a per-slot decision. Randomised slot
choice matters and is not decoration: if every station transmitted on a fixed
schedule they would collide with the same neighbours forever.

### Right pane: the spot list

```
CALL      GRID  CTY   SNR  DRIFT   FREQ   PWR    KM   BRG  AGE
```

Every column earns its place, and the reasoning is mostly about what is *gone*:

- **No MESSAGE column.** There is no message. Dropping FT8's widest column is
  what makes room for the rest without crowding.
- **DRIFT** is WSPR-specific and genuinely diagnostic - it is how you catch a
  drifting transmitter, and it is the number that told us our own TX was clean
  when fifty stations reported 0.
- **PWR** (the station's reported dBm) is not trivia: with SNR and KM it is the
  whole propagation story, and it is the input to km/W, the figure of merit
  WSPR operators actually care about.
- **FREQ** is the offset in Hz inside the 200 Hz window, not the absolute MHz.
  The MHz part is the same for everyone in the sub-band, so printing it would
  spend the widest column in the table on a constant.
- **CTY / KM / BRG** stay, from `util/dxcc.c` and `util/maidenhead.c` exactly as
  FT8 uses them. WSPR is a DX-and-propagation mode; these are the point.

**The list groups by cycle, unlike FT8's.** FT8's decode list is a live picture
of who is on frequency *now*, aged out after 60 s. WSPR spots arrive in a burst
every two minutes and then nothing, so a flat live list would look broken for
110 seconds out of every 120. Grouping under a light `21:28` cycle header makes
the two-minute rhythm legible instead of confusing, and turns the pane into
what it should be - a short log - rather than a status display that is stale
most of the time.

### Deliberately NOT on this screen

Named so they are not re-added as oversights: no worked-before, no ADIF logging,
no QSO columns (no QSO exists); no tone picker (WSPR has no callable frequency
choice); no message editor (call + grid + power are settings); no EVEN/ODD
parity control (every WSPR slot is even - there is no parity to choose).

## Web

The parity rule says the spot list ships on both in the same pass, and it
should. But parity means *the same information*, not the same pixels, and the
project's own test is "would this be needed at a POTA site with no laptop?".

- **Tab5 gets** what you need standing in a field: what am I hearing, is TX on,
  which band, when is the next transmission.
- **The web gets** what a laptop is better at and a 1280×720 touch screen is
  not: the full history rather than the recent window, sortable columns, CSV
  export, and - the one genuinely new thing - a **distance/bearing plot**,
  because "where am I being heard" is a shape, not a table.

`GET /api/wspr` mirrors `/api/decodes`' shape:

```json
{ "state":"rx", "dial_hz":14095600, "cycle_utc":"21:28",
  "next_tx_s":227, "tx_enabled":false, "duty_pct":20,
  "spots":[{"call":"W3HH","grid":"EL89","cty":"USA","snr":-9,
            "drift":0,"hz":1497.3,"pwr":30,"km":7012,"brg":291,"age":41}] }
```

## The missing half, and why it is not in this design

The emotional core of WSPR is **who heard me** - that is why the operator sat
up tonight watching fifty callsigns arrive. That data is on wsprnet.org and
this device has WiFi, so fetching our own spots and showing them beside the
ones we heard is clearly the best feature in this whole document.

It is deliberately not in Phase 3. It needs its own research pass (wsprnet's
query interface, rate limits, and a courteous polling interval - the same care
`reference_spothole_api` records for spothole), it is useless without a network
so it cannot be the primary screen, and Phase 4 of the scope doc already parks
reporting for exactly these reasons. Designed around, not designed in: the
cycle header and the left pane both leave room for a "heard by N" line to
appear later without moving anything.

## Update — the page as built, and the one thing the design got wrong

Built on 2026-08-24 and revised live against the operator's screenshots. The
side-panel shape survived contact; one thing did not.

**The design said nothing about a spectrum, and the operator wanted one.** Their
reasoning was sound - WSPR is narrow, so the panel leaves room and the rest of
the pane is spare. Two facts then shaped what could actually be built:

- A **live** spectrum is impossible here. While a capture is armed the DSP
  diverts the IQ into the capture pre-ring instead of the panadapter FFT, and a
  capture fills 120 s of every 120 s cycle - so it would be frozen for exactly
  the time it matters.
- A **full-span** one would be nearly useless anyway: the panadapter shows
  48 kHz and WSPR occupies 200 Hz, so the whole sub-band is about SIX PIXELS.

So the page shows the **captured window as a waterfall**, built after each
capture and before the decode - which is what WSJT-X shows for WSPR, and costs
almost nothing because the data is already in hand. 176 rows (one per symbol
period) x 205 columns at 1.4648 Hz per bin, which is exactly one bin per WSPR
tone spacing. Updates once per cycle; for a mode where nothing happens faster
than that, once per cycle is live.

The frequency scale under it is the operator's requirement, verbatim: *"need
numbers to judge the horizontal placement"*. A waterfall without them cannot
answer the only question it exists to answer.

**Layout as built**: panel left (info, and buttons when there are any),
waterfall top-right, decode log underneath.

### Four faults that only a photograph of the screen found

Recorded together because the pattern is the lesson: every one of these was
invisible in the source, in the logs, and in the rendered DOM. The operator
photographed the Tab5 and all four fell out.

1. **"MODE: WSPR" overran the 320 px panel** into the decode list's CALL column.
   "MODE: FT8" fits at montserrat_48; one more character does not.
2. **The waterfall was drowned in speckle.** Black was mapped to the median, so
   half the noise showed - and a single FFT bin's noise power is exponentially
   distributed, standard deviation equal to its mean, ~5.6 dB. Against that, a
   weak WSPR signal is only ~7 dB above the floor in a 1.4648 Hz bin. Black is
   now +5 dB and two FFTs are averaged per row.
3. **The frequency scale was compressed**, ending at x~730 of a 944 px
   waterfall, because it was a space-padded string and I had guessed the
   proportional font's space width at roughly double its real value. Now one
   absolutely-positioned label per tick, placed by the same arithmetic that maps
   a frequency to a column.
4. **The decode list header did not line up with its rows.** The header string
   and the row printf format were maintained separately and had drifted - PWR's
   data ended in the column its header started in. There is now ONE format
   string for both, every field passed as a string so the header goes through
   the same specifiers.

A fifth, found by measurement rather than eye: the waterfall was filled with
188,800 `lv_canvas_set_px()` calls, each going through LVGL's draw layer, which
blocked taskLVGL long enough to starve the HTTP server - the browser
disconnected every cycle and `/ss.bmp` truncated at 135 KB of 1.84 MB. Writing
RGB565 straight into the canvas buffer plus one invalidate does the same work
without holding the task.

## Build order

1. `wspr_spots.c` - the spot store (mutex-protected, aged, sortable), modelled
   on `ft8_screen.c`'s call table.
2. `UI_MODE_WSPR` + screen skeleton + the RX slot loop that fills the store.
3. `GET /api/wspr` and the web panel.
4. Help topic, so the drawer's User Manual button lands somewhere real.

Step 2 has a feasibility gate in front of it: the host decoder takes ~15 s for
8 candidates on a laptop, and it must fit inside a 120 s cycle on a 360 MHz
RISC-V core that is also running the panadapter. **Measure that before building
the loop around it** - if it does not fit, the answer is fewer candidates or a
narrower search, and it is much cheaper to learn that from a timing harness
than from a screen that mysteriously shows nothing.
