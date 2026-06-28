# Time Sync

FT8 requires accurate UTC time — within ±1 second of the real thing. The panadapter syncs time from multiple sources in priority order.

### 1. Time Sources (Priority Order)

1. **WiFi + SNTP** (if available) — most accurate, syncs every ~1 hour
2. **Tab5 RTC** (if set) — persists across power cycles (±1 min accuracy)
3. **QMX GPS** (if your QMX has GPS) — re-syncs the RTC every 5 minutes
4. **Manual set** — you can enter time manually via the settings drawer

The panadapter **always uses SNTP if WiFi is up**. If WiFi drops, it falls back to the RTC, and finally manual time if neither is available.

### 2. Offline (POTA / Portable)

If you're operating **without WiFi** (POTA, portable, SOTA):

1. **Set the Tab5 RTC before you leave home** (settings → Time)
2. The RTC is powered by a **supercap battery** and holds time for **30–40 hours** without power
3. When you turn the Tab5 on in the field, it reads the RTC immediately
4. Optional: if your QMX has GPS, the panadapter syncs the RTC from QMX GPS every 5 minutes

No internet needed — FT8 timing works offline.

### 3. WiFi + SNTP

When WiFi is active:

1. The panadapter connects to an SNTP server (default pool.ntp.org)
2. Gets the current UTC time
3. Sets the system clock and writes it to the Tab5 RTC
4. Checks periodically (~hourly) and re-syncs if time has drifted

SNTP sync is **automatic** — you don't need to do anything. The bottom bar shows the current time (updates every second).

### 4. QMX Time Sync

If your QMX has **internal GPS** (QMX+ models often do):

1. The panadapter queries the QMX for time via the `TM;` CAT command
2. Uses that to verify the RTC is correct
3. Re-syncs every 5 minutes while the QMX is connected

This is **automatic backup only** — SNTP always takes precedence when WiFi is up.

### 5. Manual Time Set

To set time manually:

1. Swipe ← to open the settings drawer
2. Tap **Time Sync** section
3. Tap **Set Manual Time**
4. Enter hours, minutes, seconds (UTC)
5. Tap **Apply**

The time is set immediately and written to the RTC.

### 6. FT8-Derived Sync (Advanced)

The panadapter can also **estimate UTC offset** from decoded FT8 signals:

1. Decode several FT8 messages (requires on-air activity)
2. Measure their signal timing relative to the 15-second slot boundary
3. Estimate the RTC error (sub-second precision)
4. Automatically adjust the system clock

This is **optional** and most useful when:

- WiFi is unavailable
- Your RTC is way off (e.g., you haven't set it in weeks)
- You need sub-second precision

**⚠️ FT8 mode only** — not yet available in FT4 mode.

### Fine-Tune Time with "Sync Time" (FT8 Only)

In **FT8 mode**, the Filter modal includes a **"Sync Time" button** that opens an interactive time-setting panel:

1. Tap the **Filter** button (FT8 screen, left pane)
2. Tap the **Sync Time** button (bottom right of the modal)
3. A panel appears with three fields: **[HH] : [MM] : [SS]**
   - **HH** / **MM** (hours/minutes) — tap to edit via numpad (0–23, 0–59)
   - **SS** (seconds) — auto-syncs from FT8 signals (blue frame = actively syncing, grey = locked)
4. Tap **Apply** to write the time to the RTC and system clock

The **SS field** updates automatically from decoded FT8 messages — each decode gives a sub-second correction estimate. Tap **SS** to toggle between auto-syncing (blue) and locked (grey). While locked, seconds still count at the captured offset, so you don't lose precision after locking.

**Use case:** You're operating portable without WiFi, your RTC is ~5 seconds off, and there's on-air FT8 activity. Set HH/MM manually, let SS auto-sync to the decoded signal timing for a few seconds, lock it, and you're done — FT8 timing is now precise.

FT8-derived sync shows **SS (sub-second)** in the bottom bar when active.

### 7. Bottom Bar Time Display

The center of the bottom bar shows the current UTC time and which source is active:

| Indicator | Meaning | Updates |
|---|---|---|
| **UTC(NTP)** | WiFi + SNTP (most accurate) | ~1 hour |
| **UTC(FT8)** | FT8-derived sub-second sync | Continuous (when decoding) |
| **UTC(QMX)** | QMX GPS (if available) | Every 5 min |
| **UTC(RTC)** | Tab5 supercap RTC | At boot |
| **UTC(MAN)** | Manual time set | On-demand |
| **UTC** | Fallback (no sync source) | — |

The suffix tells you at a glance which time source the panadapter is currently using. **SNTP always wins if WiFi is up** — if you see `UTC(NTP)`, that's your most accurate source and you don't need to do anything. If WiFi drops and you're in the field, you'll see `UTC(FT8)` (if on-air FT8 is active) or `UTC(RTC)` (from the supercap-backed RTC you set before leaving home).

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
| **SNTP** | ±10 ms | ~1 hour | No |
| **RTC** | ±1 min | at boot | Yes (30–40 h) |
| **QMX GPS** | ±100 ms | every 5 min | Yes (if QMX has GPS) |
| **Manual** | ±1 sec | on-demand | Yes (until power cycle) |
| **FT8-derived** | ±300 ms | continuous | Yes |

---

**Next:** Configure [Settings](settings.md) or troubleshoot [Time Sync Problems](../reference/troubleshooting.md#time-sync).
