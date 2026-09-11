# Spot Map

The **Live Spots** lane answers "who can I work?". The **spot map** answers the
other question: **who is hearing me?**

It is a full-screen map of the stations that have reported *your* signal — the CW
skimmer that copied your CQ, the FT8 station whose decode of you reached PSK
Reporter, the WSPR receiver that logged your beacon — each drawn as a line from
your grid square to theirs.

This is Uwe DL8UG's work, contributed to the project.

!!! note "Off by default"
    The spot map is **opt-in**. Switch it on in
    **Settings → Spot map (who is hearing me)**. Until you do, nothing connects
    and nothing runs — see [Why it is off until you ask](#5-why-it-is-off-until-you-ask).

---

### 1. Opening it

**Swipe down from the very top edge of the screen.** The map opens over whatever
page you were on, and **Exit** (top right) closes it. A Bluetooth keyboard's
`Esc` closes it too.

The gesture works from the panadapter, FT8 and WSPR pages alike.

If the map is switched off, the swipe tells you so and points you at the setting
rather than opening an empty world.

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

The **Source** checkboxes in the left sidebar show or hide each one. **Flush**
empties the map immediately so you can start a fresh picture — it does not stop
any of the feeds, they simply begin filling it again.

---

### 3. The three tabs

**MAP** draws a line from your position to each station that reported you, over a
world outline. Line colour matches the source: **blue** for CW, **amber** for
digital, **green** for WSPR — the same colours the band-plan strip uses for those
modes. Your own position is a dot, drawn on top.

- **Drag** with one finger to pan.
- **Pinch** with two fingers to zoom, up to 8×. Zoom is anchored on your own
  station, so your QTH stays put while the world grows around it.

**LIST** is the same data as a table — receiver, mode, band, frequency, SNR,
distance and age — when you want the numbers rather than the picture.

**CONDITIONS** is HF propagation from hamqsl.com: day and night ratings per band
group, plus solar flux, A and K index, sunspot number, geomagnetic field and
signal noise. It refreshes about hourly, which is as often as the source updates.

---

### 4. What you must set first

**Your grid square.** The map draws lines *from* you, so without it there is
nothing to draw from — the sidebar says so in red, and the map stays empty.
Set it in **Settings → Callsign & Grid**.

**Your callsign**, for the same reason: it is what the three feeds are asked
about.

**QRZ.com callbook credentials are optional.** RBN reports name the skimmer that
heard you, not where it is, so the map looks that up on QRZ to place it. Without
credentials an RBN line still gets drawn, using the centre of the skimmer's DXCC
entity — right country, wrong town. PSK Reporter and WSPR carry a grid square in
the report itself and never need the lookup. Set them in
**Settings → QRZ callbook**.

---

### 5. Why it is off until you ask

Two reasons, both worth knowing rather than just working around:

**It opens a connection on your behalf.** While the map is on, the Tab5 holds a
live session to PSK Reporter's broker subscribed to your callsign, and polls
wsprnet and hamqsl. That is a reasonable thing to do when you have asked for it,
and not something to inherit from a firmware update.

**It costs memory that other things need.** Measured on the bench, running the
feeds costs about 6.6 KB of internal RAM and 2.6 KB of the DMA pool — and that
DMA pool is the one that microSD mounts, USB and encrypted uploads draw on when
they need it. Charging that to everyone, including operators who never open the
map, is not a good trade.

Switching it on and off takes effect immediately, no reboot. Switching it off
gives the memory back.

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
