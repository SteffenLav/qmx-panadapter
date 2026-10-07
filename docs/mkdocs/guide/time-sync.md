# Time Sync

FT8 requires accurate UTC time — within ±1 second of the real thing. The panadapter syncs time from multiple sources in priority order.

### 1. Time Sources (Priority Order)

1. **Unit GPS v1.1 on PORT.A** (if selected in Settings -> Device -> Port mode) — satellite UTC **including the full date**, phase-locked to the second, works offline
2. **GPS-disciplined QMX** (auto-detected) — phase-locked to the GPS second, ~10 ms, works offline; on radio firmware 1.04_004 and later it brings the **date** as well
3. **WiFi + SNTP** (if available) — ~10 ms, re-syncs every ~1 hour
4. **Tab5 RTC** (if set) — persists across power cycles (±1 min accuracy)
5. **FT8/FT4-derived** — offline fallback only (ignored while GPS/SNTP is up)
6. **Manual set** — enter time manually via the settings drawer

GPS and SNTP are the accurate sources and are used whenever present; if you're offline with neither, the panadapter falls back to the RTC (or FT8-derived / manual). The Unit GPS outranks the others while it has a fresh lock — see [Unit GPS Time Source](#unit-gps-time-source-port-a).

!!! note "Both GPS sources show `UTC(GPS)`"

    A Unit GPS and a GPS-disciplined QMX are both GPS, so the clock suffix is
    the same word for both. Which one you have is told apart by the **GPS
    indicator** on the bottom bar (see [Bottom Bar Time Display](#7-bottom-bar-time-display))
    and by the apply lines in the log, not by a different suffix.

### Unit GPS Time Source (PORT.A)

With **Settings -> Device -> Port mode** set to *Unit GPS v1.1*, the M5Stack Unit GPS on PORT.A disciplines the clock. It is the only offline source that brings the **date** with it, so it answers the "Is today's date right?" question by itself.

| | |
|---|---|
| **What it needs** | Port mode = *Unit GPS v1.1*, the Unit GPS plugged into PORT.A, sky view for the antenna |
| **What it gives** | UTC time and the **full date**, from the satellite's RMC sentence |
| **Priority** | Highest, while it has a fresh lock (see the table above) |
| **Date** | Verified automatically on every accepted fix — the modal does not appear |
| **Accuracy** | Phase-locked to the second; no measured figure is documented yet, and none will be until it has been measured against a reference on hardware |

The bottom-bar **GPS indicator** — one GPS symbol, coloured by state, the same construction as the Bluetooth glyph beside it — shows what the *receiver* is doing, which is a different question from the clock suffix (which shows who is *disciplining* the clock). The colour is the whole message:

| Symbol | Meaning |
|---|---|
| **amber** | Running, no fresh fix yet — antenna unplugged, indoors, or still acquiring |
| **green** | Locked: a fresh fix, and this is the clock's authority |
| **red** | Had a lock, it has gone stale (cable out, sky lost) — the clock falls back to whatever else is live |
| *(hidden)* | Port mode is **Relay** — there is no receiver to describe |

Locking takes 30 s to a few minutes from cold with a clear sky. **Switch the mode before plugging the GPS in** (enable-then-plug); see [Settings](settings.md) for why.

### Module GPS v2.1 on the 30-pin bus — untested

!!! warning "Supported in the code, never run on hardware"

    A **Module GPS v2.1 (M5Stack M003-V21)** stacked on the 30-pin bus is
    handled by the same driver as the Unit GPS — it is the same AT6668
    receiver, and the only difference is which pin the data arrives on
    (GPIO2). **Nothing about it has been verified on hardware**, there is no
    control for it on screen, and it is switched on over the API only:

    ```
    POST /api/cmd   {"action":"gnss_mbus","on":true}
    ```

    Set TXD DIP switch 5 on the module and leave every RXD switch off — the
    driver only ever listens. Being on the 30-pin bus, it does not conflict
    with the PORT.A relay, so both can be fitted at once.

    Use the **Unit GPS on PORT.A** if you want a receiver that has actually
    been used. This note exists so the option is not a secret, not because it
    is ready.


### 2. Offline (POTA / Portable)

If you're operating **without WiFi** (POTA, portable, SOTA):

0. **Best: take a Unit GPS.** With Port mode set to *Unit GPS v1.1*, the satellite gives you time **and the date** with no internet, no RTC remembered from home and no question to answer — see [Unit GPS Time Source](#unit-gps-time-source-port-a). Everything below is what to do when you have no Unit GPS.
1. **Set the Tab5 RTC before you leave home** (settings -> Time)
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
    (**FT8 -> Options -> Sync Time**), which also accepts seconds.

!!! warning "The date is checked separately from the time"

    The QMX, its GPS and the hours/minutes/seconds boxes all give only a
    **time of day**. If the Tab5 has been off longer than its clock lasts and
    there is no internet, the date it has is simply the last day it was used —
    so a perfectly set time can still be logged under the wrong day.
    Reported by Don WB0LQW, whose POTA log came out two days behind.

    So when the date cannot be checked, the Tab5 asks about a minute after it
    starts: **"Is today's date right?"** Step it with **- day / + day** if
    needed and tap **This date is right**. If the Tab5 asked by itself and
    WiFi then comes up, the question closes by itself; if **you** opened the
    window to change the date, it stays open until you are done with it. QSOs
    are still logged if you do not answer — nothing is ever held back.

    The Tab5's own clock surviving a power-off is **not** taken as proof that
    the date is right. That clock keeps running as long as its small battery
    holds, but it says nothing about who last wrote it — another firmware
    flashed onto the same Tab5 can leave a date decades out with the battery
    perfectly healthy, which is what happened to royord. The clock is trusted
    only when this firmware's own record agrees with it, the clock has not
    gone backwards since that record, and the gap is short enough that
    somebody has plausibly been watching.

    The **Unit GPS on PORT.A is the exception**: its sentences carry the
    satellite date, so the date is verified by the fix itself and the question
    never has to be asked. See
    [Unit GPS Time Source](#unit-gps-time-source-port-a).

    The **Set and Sync the Clock** window also shows the date on its second
    line, marked *(unverified)* when it has not been checked. Tap that line to
    change it.

### 3. WiFi + SNTP

When WiFi is active:

1. The panadapter connects to an SNTP server (default pool.ntp.org)
2. Gets the current UTC time
3. Sets the system clock and writes it to the Tab5 RTC
4. Checks periodically (~hourly) and re-syncs if time has drifted

SNTP sync is **automatic** — you don't need to do anything. The bottom bar shows the current time (updates every second).

### 4. QMX GPS Time Sync (auto-detected)

If your QMX has **internal GPS** (QMX+ models often do), there is **nothing to enable** — the Tab5 detects it automatically:

1. **On radio firmware 1.04_004 and later it asks the GPS receiver directly** (`GP;`). A reply settles two things at once: this radio *has* a GPS — the receiver's own answer, not a guess — and **today's date**, which the radio's clock has never carried. Both work offline and on the first try.
2. It then catches the QMX's `TM;` seconds *flip* (the true GPS second boundary) and **phase-locks the clock to that edge** (~10 ms, drift-free), not just to the whole second. Re-locks every 5 minutes.
3. On older radio firmware, or a radio with no GPS, there is no `GP;` answer. Then the `TM;` flip is compared against SNTP: if they agree tightly — **and the Tab5 has not itself set that radio's clock** — the QMX is flagged GPS-disciplined. If they don't agree (a non-GPS QMX with a free-running RTC), it's used only as a low-priority offline fallback, and the check is **repeated every 5 minutes** until it does agree.

!!! note "Why the time still comes from `TM;` when `GP;` is the receiver"

    `GP;` is asked for the **date** and for the GPS answer, not for the clock
    phase. Measured on the bench against a reference: the second boundary
    `GP;` reports is roughly **a second behind** UTC, because a GPS receiver
    emits the sentence for one second during the next one. The radio's own
    clock, set from the satellite at power-on, is within about **100 ms** —
    an order of magnitude better. So the date comes from the receiver and the
    phase comes from the radio, each from whichever is actually good at it.

!!! note "A GPS that locks late is no longer missed (v1.16.3)"
    Before v1.16.3 that check ran **once**, about 45 seconds after boot, and its
    verdict stuck for the whole session. A receiver still hunting for satellites
    at that moment was written off as "no GPS" and never looked at again, short
    of a reboot. That is not a rare case: a cold start, or an antenna that has
    just been moved, can easily take longer than 45 seconds.

    Measured on the bench: a QMX+ with a genuine fix failed the one-shot three
    times, then locked — and the panadapter picked it up **25 minutes after
    boot**, which the old code could never have reached. If your GPS takes its
    time, you no longer have to do anything.

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
| **UTC(GPS)** | GPS: either the Unit GPS on PORT.A, or a GPS-disciplined QMX (auto-detected, phase-locked to the GPS second, ~10 ms) | every fix / every 5 min |
| **UTC(NTP)** | WiFi + SNTP | ~1 hour |
| **UTC(FT4)** / **UTC(FT8)** | FT4/FT8-derived offline sync | Continuous (when decoding, offline only) |
| **UTC(RTC)** | Tab5 supercap RTC | At boot |
| **UTC(MAN)** | Manual time set | On-demand |
| **UTC(QMX)** | Non-GPS QMX RTC fallback (offline only) | Every 5 min |
| **UTC** | Fallback (no sync source) | — |

The suffix tells you at a glance which source is **currently in charge** (the active authority, not merely the last one that wrote the clock). A GPS source (`UTC(GPS)`) and WiFi/SNTP (`UTC(NTP)`) are both accurate — if you see either, you're set. If you're off-grid you'll see `UTC(FT8)`/`UTC(FT4)` (on-air digital activity), `UTC(QMX)` (non-GPS QMX clock), or `UTC(RTC)` (the supercap RTC you set before leaving home).

At the right-hand end of the bar, just left of the Bluetooth glyph, sits the **GPS indicator** when Port mode is *Unit GPS v1.1*: one GPS symbol, coloured by state, with no words beside it — the Bluetooth glyph's construction. It reports the **receiver**, not the clock: amber while it is acquiring, green when locked, red when a lock has gone stale. The suffix only says `UTC(GPS)` when the symbol is green — the two never disagree, because the suffix is derived from the same lock the symbol shows.

### 8. Slot Timing

FT8 operates on **15-second slot boundaries** aligned to UTC. The panadapter:

1. Reads the current system time
2. Waits until the next 15-second boundary (hh:mm:00, hh:mm:15, hh:mm:30, hh:mm:45)
3. Starts a 15-second capture window
4. Decodes the received FT8
5. Transmits (if armed) at the start of the next boundary

This alignment is **automatic** — you don't configure slots. But **time accuracy is critical**:

- ±500 ms error -> can miss decodes or transmit off-slot
- ±1 s error -> very few decodes, transmit often off-time
- ±2 s error or worse -> FT8 doesn't work

### 9. Time Sources Summary

| Source | Accuracy | Updates | Works Offline? |
|---|---|---|---|
| **Unit GPS (PORT.A)** | phase-locked to the second — no measured figure documented yet | every fix (1 Hz) | Yes (plus the **date**) |
| **QMX GPS** | ~10 ms (auto-detected, tick phase-lock) | every 5 min | Yes (if QMX has GPS) |
| **SNTP** | ±10 ms | ~1 hour | No |
| **RTC** | ±1 min | at boot | Yes (30–40 h) |
| **Manual** | ±1 sec | on-demand | Yes (until power cycle) |
| **FT8-derived** | offline fallback only | continuous | Yes |

---

**Next:** Configure [Settings](settings.md) or troubleshoot [Time Sync Problems](../reference/troubleshooting.md#time-is-wrong-ft8-doesnt-work).
