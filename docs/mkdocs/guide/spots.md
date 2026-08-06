# Live Spots

Live spots put other stations **on your spectrum**, at the frequency they are
actually operating on, so you can see who is where without leaving the radio.

Two sources feed the same display:

| Source | What it tells you | Default |
|--------|-------------------|---------|
| **POTA** (Parks On The Air) | Park activations currently spotted, any mode | **On** |
| **RBN** (Reverse Beacon Network) | CW stations the skimmer network is hearing right now | **Off** (opt-in) |

Both need WiFi. Nothing here transmits, and nothing here touches the radio until
you tap a spot.

---

### 1. What you see

Spots are drawn **over the spectrum**, the way a FlexRadio or similar SDR shows
them — the callsign sits around the middle of the spectrum with a thin vertical
line dropping from it down to the frequency axis, so the line points at the
frequency the station is on. The line is only 2 px wide, so the trace stays
readable underneath.

**Colour tells you what a spot means:**

| Colour | Meaning |
|--------|---------|
| **Amber** | A POTA activation |
| **Bright green** | An RBN (CW skimmer) spot |
| **Grey** | You have **already worked this station on this band** |

Grey is the useful one: it answers "do I need this station?" at a glance. It is
band-aware, so the same operator on a different band is *not* greyed out — that
is a new band-slot.

**Spots fade as they age.** A spot is a claim about *now*, and an old one pointing
at an empty frequency is worse than no spot at all:

- Under 5 minutes old — full brightness
- Around 15 minutes — about half faded
- 30 minutes — gone

The callsign and its line always fade together.

**Off-screen counts.** When there are spots on your band that fall outside the
window you are looking at, a small count appears in the bottom corner of the
spectrum — `< spots (3)` on the low side, `spots (5) >` on the high side. It says
"spots" in words rather than a bare `<3`, because a lone number against the edge
of the spectrum tells you neither what it counts nor that you can tap it. These
are **scoped to the band you are on**, so the arrow never points at something on
40 m while you are on 20 m. Each is coloured like the spot it will take you to.

**Crowded bands.** Only so many callsigns fit legibly side by side, so on a busy
segment the closest-spaced spots lose their name — and a spot with no name is not
drawn at all, rather than leaving a line pointing at nothing. Stations you have
**not** worked are labelled in preference to ones you have, and fresher spots in
preference to older ones.

---

### 2. Tapping a spot

**Tap a callsign** and the radio tunes to it *and* switches mode:

| Spot mode | Radio goes to |
|-----------|---------------|
| CW | CW |
| FT8, FT4, other data | DiGi |
| SSB | USB above 10 MHz, LSB below |
| Unknown | Frequency only — the mode is left alone |

Bandwidth is deliberately **not** forced. The QMX keeps a filter per mode and
loads it when the mode changes, so the right width follows by itself.

**Tap an off-screen count** (`< spots (3)` / `spots (5) >`) and you jump to the nearest spot on
that side, which brings it into view. Those counts have a deliberately large
touch area — the visible text is small, but the target around it is not.

Tapping the spectrum anywhere else still tunes normally, and pinch-zoom and the
one-finger pan are unaffected. Only the callsigns themselves and the two counts
are spot targets.

---

### 3. Turning it on and off

**Settings drawer** (swipe in from the right edge) — **Live spots** section:

| Control | What it does |
|---------|--------------|
| **Live spots (POTA)** | The whole feature. Off leaves the spectrum completely clean |
| **Add RBN (CW skimmers)** | Adds RBN as a second source. Needs your callsign set |

Both settings are saved, and both appear in the configuration file you can
download and restore from the web UI (`spots` and `spots_rbn` under
`[settings]`).

The spots overlay belongs to the **Panadapter** page. It is not drawn on the FT8
screen.

---

### 4. About RBN

RBN is **off by default, on purpose.** It is a continuous global feed rather than
an occasional fetch, and it arrives over a persistent connection on the part of
this board that has historically been the most delicate. Off by default means it
can never affect anyone who has not asked for it.

If you do switch it on:

- **Your callsign is required.** The feed asks for one when connecting; it is how
  RBN attributes load, not a password. Set it under **Callsign & Grid**.
- **Only your current band is kept.** RBN reports the whole world, and the
  display can only show one band at a time, so spots outside the band you are on
  are discarded as they arrive. Changing band clears the picture and it refills
  within a few seconds.
- **Duplicates are merged.** The same CQ is typically reported by ten or more
  skimmers; you see one entry per station, at the best reported signal.
- Spots are held for up to **10 minutes** after they were last heard.
- It is the **CW** feed. FT8/FT4 activity comes from POTA, not RBN.

---

### 5. Refresh and timing

- POTA is fetched about **once a minute** — spot rates on that service change on
  that sort of timescale, and the fetch briefly pauses the web UI's spectrum
  stream so the two never compete for the link.
- RBN arrives continuously and is merged into the display every **10 seconds**.
- Ageing is re-evaluated every second, so spots dim and disappear on their own
  even when nothing new arrives.

If spots never appear at all, check in this order: WiFi connected (bottom bar),
**Live spots** switched on, and — for RBN only — a callsign set.
