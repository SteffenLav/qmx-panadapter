# Web Radio

An internet radio player on the Tab5, for when the bands are quiet. New in
this release. It has nothing to do with the QMX — the radio and its I/Q are
untouched while you listen.

---

## Opening it

Swipe ← from the right edge for the settings drawer, then tap **Web Radio**.
Closing the player (tap outside, or the **Close** button) does **not** stop
playback — it is a background task, so you go back to the panadapter and the
music keeps going. There is a separate **Stop** button for that.

---

## Finding a station

The directory comes from [radio-browser.info](https://www.radio-browser.info),
a community-run list of internet radio streams. Two filters compose together:

- **Search** — station name, free text.
- **Genre** — a fixed list of the most common genres in the directory
  (pop, rock, chillout, house, dance, electronic, 80s, 90s, and more),
  fetched once and filtered locally, so picking a genre costs no extra
  network round trip and composes instantly with whatever you have typed
  in search.

Tap a station in the list to play it. The station name and the **ICY**
programme title (what's actually playing right now) are shown, and the title
updates on its own as the stream changes track.

---

## Memory slots

Six slots, the same tap/long-press pattern as other memory features in this
app:

- **Tap** a slot to play the station stored there.
- **Long-press** a slot to store whatever is currently playing.

Stored in NVS, so they survive a reboot — this board reboots on every flash,
and a memory that didn't would not be much of one.

---

## What it costs

- Runs over the Tab5's own WiFi — no extra port, no extra setup, no PC.
- Audio is decoded on the Tab5 (an MP3 decoder running on its own core);
  measured at roughly 20% of that core once its hot buffers were moved out
  of slow internal RAM — low enough to leave the panadapter and FT8/CW
  decoding alongside it undisturbed.
- Playback is local to the Tab5 — it comes out of the Tab5's own speaker or
  headphone jack, the same output the rest of the audio features use (see
  [Audio](audio.md)), not streamed to a browser.

---

## Known limitations

- Latin-1 station names and programme text (Danish, Swedish, German — æøå
  and friends) render correctly; this is new and worth a report if you see
  a station name come through as boxes instead.
- No favourites/sort beyond the six memory slots and the search/genre filter
  yet.
