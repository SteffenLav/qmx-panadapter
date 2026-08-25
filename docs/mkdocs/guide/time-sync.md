# Time Sync

FT8 requires accurate UTC time — within ±1 second of the real thing. The panadapter syncs time from multiple sources in priority order.

### 1. Time Sources (Priority Order)

1. **GPS-disciplined QMX** (auto-detected) — phase-locked to the GPS second, ~10 ms, works offline
2. **WiFi + SNTP** (if available) — ~10 ms, re-syncs every ~1 hour
3. **Tab5 RTC** (if set) — persists across power cycles (±1 min accuracy)
4. **FT8/FT4-derived** — offline fallback only (ignored while GPS/SNTP is up)
5. **Manual set** — enter time manually via the settings drawer

GPS and SNTP are the two accurate sources and are used whenever present; if you're offline with neither, the panadapter falls back to the RTC (or FT8-derived / manual).

### 2. Offline (POTA / Portable)

If you're operating **without WiFi** (POTA, portable, SOTA):

1. **Set the Tab5 RTC before you leave home** (settings → Time)
2. The RTC is powered by a **supercap battery** and holds time for **30–40 hours** without power
3. When you turn the Tab5 on in the field, it reads the RTC immediately
4. **Turn the QMX on whenever you like — you do not have to do anything about its
   clock.** A QMX without GPS comes up at 00:00 and its clock free-runs, so the
   panadapter does not take the time from it while the Tab5's own clock is good.
   It sets the radio's clock instead, and only when the radio is more than a few
   seconds out.
5. Optional: if your QMX has GPS, the panadapter syncs the RTC from QMX GPS every 5 minutes

No internet needed — FT8 timing works offline.

!!! note "Why step 4 says that"

    Earlier versions took the time from a GPS-less QMX when there was no WiFi,
    which meant switching the radio on in the field replaced your accurate RTC
    time with the radio's 00:00 — and FT8 stopped decoding. Reported by
    Don WB0LQW. The Tab5's supercap RTC is the better clock of the two offline,
    so it now wins, and the radio gets set from it.

    If you have no accurate time at all — the RTC was never set, or it has been
    unpowered for more than a day or two — then a QMX reading *is* used, because
    something is better than nothing. Failing that, set the clock by hand
    (**FT8 → Options → Sync Time**), which also accepts seconds.

### 3. WiFi + SNTP

When WiFi is active:

1. The panadapter connects to an SNTP server (default pool.ntp.org)
2. Gets the current UTC time
3. Sets the system clock and writes it to the Tab5 RTC
4. Checks periodically (~hourly) and re-syncs if time has drifted

SNTP sync is **automatic** — you don't need to do anything. The bottom bar shows the current time (updates every second).

### 4. QMX GPS Time Sync (auto-detected)

If your QMX has **internal GPS** (QMX+ models often do), there is **nothing to enable** — the Tab5 detects it automatically:

1. At connect, it catches the QMX's `TM;` seconds *flip* (the true GPS second boundary) and compares it against SNTP.
2. If they agree tightly — **and the Tab5 has not itself set that radio's clock** — the QMX is flagged GPS-disciplined and the clock is **phase-locked to that second edge** (~10 ms, drift-free), not just set to the whole second. Re-locks every 5 minutes.
3. If they don't agree (a non-GPS QMX with a free-running RTC), it's used only as a low-priority offline fallback.

A GPS-disciplined QMX shows **UTC(GPS)** in the bottom bar and is a top-tier source (as good as SNTP); a non-GPS QMX shows **UTC(QMX)** and only helps when offline.

!!! note "Why step 2 asks who set the clock"
    The Tab5 pushes its own time into a non-GPS QMX to set that radio's clock —
    which leaves the radio agreeing with the Tab5 *because the Tab5 put it
    there*. Until v1.8.5 a close-enough agreement was taken as proof of GPS, so a
    radio with no GPS at all could be labelled `UTC(GPS)`, and worse, the Tab5
    then stopped maintaining the clock of the one radio that had no other source.
    A clock the Tab5 has set is no longer accepted as evidence about itself. The
    flag clears when the radio's clock is plainly its own again — a QMX's clock is
    not kept across a power cycle, so switching it off and on is what re-arms
    detection.

!!! note "The label needs the radio to still be there"
    A remembered verdict says *this radio is GPS-disciplined*, not *GPS is
    keeping time right now*. With the QMX unplugged the Tab5 has no GPS of its
    own, so the label falls back to whatever is actually in charge — usually
    `UTC(NTP)`. Reported by Don N2VGU, whose Tab5 read `UTC(GPS)` with the radio
    disconnected; the time was correct throughout, only the label was wrong.

### 5. Setting the time by hand

**There is no "Set Manual Time" control in the settings drawer.** Earlier versions of this
guide said there was, and there never has been on recent firmware — Don WB0LQW went looking
for it during a POTA activation and could not find it. Apologies. The clock is set from the
FT8 screen:

1. Switch to **FT8**
2. Tap **Options** (left pane)
3. Tap **Sync Time** (bottom right of the modal)
4. A panel appears with three boxes: **[HH] : [MM] : [SS]**
5. Tap **HH** or **MM** to type them on the numpad (0–23, 0–59)
6. **Hold the SS box and let go exactly on the minute** — the seconds are set to 00 the
   instant you release
7. Tap **Apply**

Step 6 is the one that matters off-grid. FT8 needs the clock right to about a second, and
until v1.8.1 the seconds could not be set at all — hours and minutes were editable and the
seconds were not, which left no way to get inside a second with no WiFi and no GPS.

The clock is set **when you release**, not when you tap Apply. The release is the
measurement, so an Apply a few seconds later would be a few seconds late. Use a watch with
a second hand, or listen for the gap between FT8 transmissions. While you hold the box it
turns amber and tells you what letting go will do.

If you have typed hours and minutes first, those are used. If you have not, the Tab5 takes
the nearest minute to its own clock — so this corrects the seconds without disturbing an
hour and minute that are already right.

The time is written to the RTC as well as the system clock, so it survives a power-off.

### 6. FT8-Derived Sync (Offline Fallback)

The panadapter can also **estimate the UTC offset** from decoded FT8/FT4 signal timing and nudge the clock:

1. Decode several messages (requires on-air activity)
2. Measure their timing relative to the slot boundary
3. Estimate the error and apply a damped correction

**This runs only as an offline fallback.** While SNTP or a GPS-disciplined QMX is available they are authoritative and FT8-derived sync is **ignored** — deliberately. The FT8 slot offset the device measures is dominated by ~½ second of one-way *receive audio latency* (QMX SDR + USB buffering), which is **not** a clock error; letting it pull the clock would actually drag your transmit timing late. So it only touches the clock when you're off-grid with no SNTP/GPS.

Most useful when: WiFi is unavailable, no GPS QMX, and your RTC has drifted.

#### Fine-Tune Time with "Sync Time" (FT8 Only)

In **FT8 mode**, the Options modal includes a **"Sync Time" button** that opens an interactive time-setting panel:

1. Tap the **Options** button (FT8 screen, left pane)
2. Tap the **Sync Time** button (bottom right of the modal)
3. A panel appears with three fields: **[HH] : [MM] : [SS]**
   - **HH** / **MM** (hours/minutes) — tap to edit via numpad (0–23, 0–59)
   - **SS** (seconds) — auto-syncs from FT8 signals (blue frame = actively syncing, grey =
     locked). **Hold it and release on the minute** to set the seconds to 00 yourself, which
     is what you want when there are no FT8 signals to sync from
4. Tap **Apply** to write the time to the RTC and system clock

The **SS field** updates automatically from decoded FT8 messages — each decode gives a sub-second correction estimate. Tap **SS** to toggle between auto-syncing (blue) and locked (grey). While locked, seconds still count at the captured offset, so you don't lose precision after locking.

**Use case:** You're operating portable without WiFi, your RTC is ~5 seconds off, and there's on-air FT8 activity. Set HH/MM manually, let SS auto-sync to the decoded signal timing for a few seconds, lock it, and you're done — FT8 timing is now precise.

FT8-derived sync shows **SS (sub-second)** in the bottom bar when active.

### 7. Bottom Bar Time Display

The center of the bottom bar shows the current UTC time and which source is active:

| Indicator | Meaning | Updates |
|---|---|---|
| **UTC(GPS)** | GPS-disciplined QMX, auto-detected, phase-locked to the GPS second (~10 ms) | Every 5 min |
| **UTC(NTP)** | WiFi + SNTP | ~1 hour |
| **UTC(FT4)** / **UTC(FT8)** | FT4/FT8-derived offline sync | Continuous (when decoding, offline only) |
| **UTC(RTC)** | Tab5 supercap RTC | At boot |
| **UTC(MAN)** | Manual time set | On-demand |
| **UTC(QMX)** | Non-GPS QMX RTC fallback (offline only) | Every 5 min |
| **UTC** | Fallback (no sync source) | — |

The suffix tells you at a glance which source is **currently in charge** (the active authority, not merely the last one that wrote the clock). A GPS-disciplined QMX (`UTC(GPS)`) and WiFi/SNTP (`UTC(NTP)`) are both accurate — if you see either, you're set. If you're off-grid you'll see `UTC(FT8)`/`UTC(FT4)` (on-air digital activity), `UTC(QMX)` (non-GPS QMX clock), or `UTC(RTC)` (the supercap RTC you set before leaving home).

### 8. Slot Timing

FT8 operates on **15-second slot boundaries** aligned to UTC. The panadapter:

1. Reads the current system time
2. Waits until the next 15-second boundary (hh:mm:00, hh:mm:15, hh:mm:30, hh:mm:45)
3. Starts a 15-second capture window
4. Decodes the received FT8
5. Transmits (if armed) at the start of the next boundary

This alignment is **automatic** — you don't configure slots. But **time accuracy is critical**:

- ±500 ms error → can miss decodes or transmit off-slot
- ±1 s error → very few decodes, transmit often off-time
- ±2 s error or worse → FT8 doesn't work

### 9. Time Sources Summary

| Source | Accuracy | Updates | Works Offline? |
|---|---|---|---|
| **QMX GPS** | ~10 ms (auto-detected, tick phase-lock) | every 5 min | Yes (if QMX has GPS) |
| **SNTP** | ±10 ms | ~1 hour | No |
| **RTC** | ±1 min | at boot | Yes (30–40 h) |
| **Manual** | ±1 sec | on-demand | Yes (until power cycle) |
| **FT8-derived** | offline fallback only | continuous | Yes |

---

**Next:** Configure [Settings](settings.md) or troubleshoot [Time Sync Problems](../reference/troubleshooting.md#time-is-wrong-ft8-doesnt-work).
