# Spot Map

The **Live Spots** lane answers "who can I work?". The **spot map** answers the
other question: **who is hearing me?**

It is a full-screen map of the stations that have reported *your* signal — the CW
skimmer that copied your CQ, the FT8 station whose decode of you reached PSK
Reporter, the WSPR receiver that logged your beacon — each drawn as a line from
your grid square to theirs.

This is Uwe DL8UG's work, contributed to the project.

!!! note "Always on, no setting to find"
    The three feeds below connect from the moment the Tab5 boots and stay
    connected — there is nothing to switch on. Earlier versions asked you to
    enable it first and only ran the feeds while the map was open; both of
    those were tried and dropped, in that order, on the operator's own
    instruction, once he found the list was still empty the first few
    minutes after opening it either way. See
    [What it costs, running all the time](#5-what-it-costs-running-all-the-time).

---

### 1. Opening it

**Open the settings drawer and tap SelfSpotter** — the button right below
**Need guidance?**, near the top of the drawer. The map opens over whatever
page you were on, and **Exit** (top right) closes it. A Bluetooth keyboard's
`Esc` closes it too.

!!! note "Not a swipe any more"
    Earlier versions opened this with a swipe down from the top edge of the
    screen. That gesture collided with the top bar's own Band/Mode/BW/Zoom
    taps and was easy to trigger by accident, so it was replaced with the
    drawer button above.

The button works from the panadapter, FT8 and WSPR pages alike — the drawer
itself opens the same way from any of them (swipe in from the right edge).

The header shows your **callsign, dial frequency, and the current UTC date and
time** — so a screenshot of the map stands on its own without needing anything
else to say when or where it was taken.

There is nothing to switch on first — the feeds have been running since boot
(see below), so the map is never starting from a cold, empty world.

---

### 2. What feeds it

Three sources, all of them reports of **your own** transmissions:

| Source | Where it comes from | How fresh |
|--------|--------------------|-----------|
| **CW (RBN)** | A Reverse Beacon Network skimmer copying your CQ | as it happens |
| **Digi (PSKR)** | PSK Reporter's live feed, over MQTT | as it happens |
| **WSPR** | wsprnet.org, polled every 3 minutes | trails a cycle |

All three need WiFi. **None of them transmits anything** — they are lookups of
what other people have already published about you.

A report stays on the map for **half an hour**, then drops off. The map is a
picture of who is hearing you *now*.

The **Source** checkboxes in the left sidebar show or hide each one. **Flush**,
in the header beside **Exit**, empties the map immediately so you can start a
fresh picture — useful when you change band. It does not stop any of the feeds;
they simply begin filling it again. *(Contributed by Uwe DL8UG. It was in the
sidebar until v1.14.4.)*

---

### 3. The three tabs

**MAP** draws a line from your position to each station that reported you, over a
world map — filled land, lighter than the sea, with a coastline outline on top.
Line colour matches the source: **blue** for CW, **amber** for digital, **green**
for WSPR. A line older than 30 minutes fades rather than disappearing, so a
quiet stretch still shows what was heard recently. Your own position is a dot,
drawn on top of everything.

- **Drag** with one finger to pan.
- **Pinch** with two fingers to zoom, up to 50×. Zoom is anchored on your own
  station, so your QTH stays put while the world grows around it.

**LIST** is the same data as a table — **RECEIVER, GRID, COUNTRY, MODE, BAND,
FREQUENCY, SNR, KM** and **AGE** — when you want the numbers rather than the
picture. Every column is sortable; tap a header to sort by it.

**GRID** is the locator the receiving station itself sent, not one worked
backwards from a position, so it is blank for CW skimmers: they report a
callsign and not a location. **COUNTRY** names the *country*, which can differ
from the DXCC entity the map plots — a Hawaiian station's marker sits on
Hawaii, while its country reads United States.

**CONDITIONS** is HF propagation from hamqsl.com: day and night ratings per band
group, plus solar flux, A and K index, sunspot number, geomagnetic field and
signal noise. It refreshes about hourly, which is as often as the source updates.

---

### 4. What you must set first

**Your grid square.** The map draws lines *from* you, so without it there is
nothing to draw from — the sidebar says so in red, and the map stays empty.
Set it in **Settings -> Station -> Callsign & Grid square**.

**Your callsign**, for the same reason: it is what the three feeds are asked
about.

**QRZ.com callbook credentials are optional.** RBN reports name the skimmer that
heard you, not where it is, so the map looks that up on QRZ to place it. Without
credentials an RBN line still gets drawn, using the centre of the skimmer's DXCC
entity — right country, wrong town. PSK Reporter and WSPR carry a grid square in
the report itself and never need the lookup.

These are set **in the web UI, not on the Tab5** — open `http://<tab5-ip>` in a
browser and use **Miscellaneous -> Set QRZ Callbook login**. They are your QRZ.com
username and password for the Callsign Lookup service, which is a different thing
from the QRZ Logbook API key used for uploading contacts.

---

### 5. What it costs, running all the time

Worth knowing, since there is no longer a setting to weigh this against:

**It holds a connection on your behalf, permanently.** From boot, the Tab5
holds a live session to PSK Reporter's broker subscribed to your callsign,
and polls RBN and wsprnet. That used to be something you asked for first;
now it starts with everything else.

**It costs memory that other things need.** Measured on the bench, running
the feeds costs about 6.6 KB of internal RAM and 2.6 KB of the DMA pool —
and that DMA pool is the one that microSD mounts, USB and encrypted uploads
draw on when they need it. Every unit pays this now, whether or not the map
is ever opened.

The feeds run whether the map is on screen or not — opening it doesn't start
them and closing it doesn't stop them — so the picture is already populated
with whatever has come in since boot the first time you look, rather than
starting from nothing.

---

### 6. If the map stays empty

**Nobody has heard you yet.** The commonest answer, and not a fault: the map only
fills once you have actually transmitted and somebody has reported it. Call CQ,
or let WSPR or FT8 run for a few cycles, then look again.

**No grid square set** — the sidebar says so in red. See section 4.

**WiFi is down.** All three feeds need it.

**Only some sources are ticked.** Check the **Source** boxes in the sidebar.

**You are running CW but RBN spots are absent** — a skimmer has to actually copy
you. RBN coverage is thinner on some bands and at some hours than others.
