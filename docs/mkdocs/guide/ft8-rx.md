# FT8/FT4 Receive & Decode List

The panadapter includes **on-device FT8 and FT4 decoders** with real-time spectrum waterfall and a live list of heard stations.


### 1. FT8 & FT4 View

Swipe → from the left edge to toggle to FT8/FT4 view. The same decode list and waterfall work for both modes — switch modes via the **Preset** dropdown in the left pane (top).

**FT4 notes:**
- Decodes refresh roughly twice as fast as FT8 because slots are 7.5 seconds
- Slot countdown shows 7.5 s instead of 15 s
- All filtering, priority, and auto-reply features work identically in FT4
- Time-sync from decoded signal timing works in both FT4 and FT8 (the bottom-bar clock shows `UTC(FT4)` when synced from an FT4 decode)
- **v0.19.4 made FT4 reliable**: a memory-placement fault previously killed decoding on every other slot (FT8's longer slots mostly hid it), the EVEN/ODD slot markers were on the wrong 15-second grid, and the slot clock could visibly jump between slots — all fixed; if FT4 seemed deaf or erratic on an earlier version, update the firmware

You'll see:

- **Decode list** (left pane) — all heard stations, sortable by signal strength, distance, or newest
- **Waterfall** (right pane) — same real-time spectrum as panadapter mode
- **Call CQ button** — start a CQ (requires QMX on the air)
- **Filter button** — include/exclude stations, prioritise by SNR/distance, show only CQ callers

### 2. Decode List

The list shows every decoded FT8 message:

| Column | Meaning |
|---|---|
| **SL** | Slot parity: **E** (blue) or **O** (amber) |
| **CALL** | Their callsign |
| **MESSAGE** | The full decoded message text |
| **CTY** | Country as a 3-letter code (from the callsign prefix) |
| **SNR** | Signal-to-noise estimate, colour-banded by strength |
| **DT** | Slot-timing offset in seconds, relative to the band — an on-time station reads ~0.0 (v1.3.1) |
| **HZ** | The station's audio tone within the FT8 passband (v1.3.1) |
| **KM / MI** | Great-circle distance from your grid |
| **BRG** | Bearing from your grid |
| **HRD** | Times decoded since last appearance |

**Own call highlight** — your callsign is shown in **inverted colours** (red fill, white text) so you spot replies to you instantly.

**Stale entries** — the list is a live picture of who's on frequency now; stations not heard again within **2 minutes** drop off automatically.

**During your own CQ run** — stations transmitting on *your* transmit slot (which you can't hear while transmitting) are hidden from the list and return when the run ends; other stations' CQ rows are hidden during a run as before.

**Nonstandard callsigns** — special-event and compound calls now resolve to the full callsign instead of showing `<...>`, once the station has been heard in full.

### 3. Filtering & Priority

Tap the **Filter** button to open the filter modal:

- **Include callsigns** — only show these calls (space or comma-separated)
- **Include callsigns containing** — show any call with these substrings (e.g., `/P` for portable)
- **Exclude callsigns** — hide these calls
- **Exclude if contains** — hide any call matching these substrings
- **Exclude worked before** — skip calls you've already logged QSOs with
- **Show only CQ callers** — hide replies, show only CQ messages
- **Skip TX1** — when you pounce a station, open with your signal report straight away instead of the grid exchange, for a quicker QSO (falls back to the normal grid exchange if the station has dropped out of the decode list)
- **Allow grey-listing** — off by default. A station that times out two of your pounces in a row is set aside: the robot and Auto-work-pileup skip it, its decode row turns **violet**, and tapping it offers to clear it from the grey-list instead of opening the TX dialog. Handy on a busy band where one station simply never comes back. The list is held in memory only and forgotten at power-off
- **Auto-reply priority** — Strongest SNR, Weakest SNR, Most distant grid
- **Robot mode** — auto-answer CQ (disabled by default; see [FT8 Transmit](ft8-tx.md))

All filters persist across sessions. Toggle any filter on/off to enable or disable it without erasing the criteria.

### 4. Decoding Performance

On-device decoding runs at:

- **FT8:** ~15–20 QSOs per 15-second slot
- **FT4:** ~15–20 QSOs per 7.5-second slot (faster decode cadence, same per-slot throughput)

This is sufficient for casual operation but not a full WSJT-X-equivalent decoder.

**Why not faster?**

- **Single CPU (core 0)** decodes ~1–2 QSOs/s (LDPC is computationally heavy)
- **Real-time FFT** (waterfall) competes for CPU cycles
- **USB audio streaming** needs constant servicing

**FT4 advantage:** because FT4 slots are half as long (7.5 s vs 15 s), you get refresh feedback roughly twice as fast, which feels snappier on a crowded band even though the per-slot decode count is the same.

The panadapter prioritises **receive latency** (decode every 15 s, not accumulate) over volume.

### 5. Real-Time Waterfall

Unlike WSJT-X (which buffers a whole 15-second capture), the Tab5 **builds the waterfall on-the-fly** as audio arrives. You see CW and other traffic in real time, and FT8 stations appear instantly as their symbol blocks are decoded.

The waterfall also shows **non-FT8 activity**:
- CW (carrier lines)
- SSB (broader vertical smears)
- RTTY (FSK sidebands)
- Interference (wideband noise)

### 6. Tap to Reply

Tap any FT8 row in the decode list to **prepare a reply**:

1. A confirmation modal pops up showing your message
2. You can review the target callsign, grid, and SNR
3. Tap **Transmit** to send, or **Cancel** to abort

The reply always follows FT8 protocol (correct parity, proper message sequence) — you can't accidentally send a garbled or out-of-order message.

**Pile-up list.** If a station answers you while you're busy with another contact, they used to disappear from the list once they stopped transmitting. They're now remembered in a **Pileup** list — the **ADIF-log** button on the FT8 screen turns into a **Pileup** button (a different colour) whenever callers are waiting. Tap it to see who's called you, tap a station to work them, or tap the ✕ to dismiss one. See [FT8 Transmit](ft8-tx.md) for the full workflow.

### 7. Auto-Reply (Robot Mode)

⚠️ **Experimental** — enabled via a checkbox in the Filter modal.

When robot mode is on, the Tab5 **automatically replies to CQ** without waiting for you to tap. It scans each FT8 slot for CQ callers matching your filters, picks the highest-priority station, and sends a full QSO exchange (TX1 → wait for report → TX2 → wait for RR73 → TX3). Everything is logged to ADIF.

**Important:** Robot mode **keys the QMX for real** — your signal goes on the air. Never leave it unattended unless you're confident in your filters and your station is in a safe state.

### 8. Search & Window

The decoder uses an **FFT-based search window** to find candidate tone blocks. The search is tuned for standard FT8 tone spacing (6.25 Hz per tone).

**Frequency stability:** The QMX's USB audio clock isn't bit-exact 48 kHz, so decodes slide slowly over time. The panadapter **re-locks to UTC boundaries every 15 seconds** (the slot boundary), preventing long-term drift.

### 9. Time Sync for FT8

FT8 requires **UTC time accurate to ±1 second** for slot synchronisation. The panadapter gets time from:

1. **WiFi + SNTP** (if available) — most accurate, syncs every ~hour
2. **Tab5 RTC** (if set) — accurate to ±1 min, no internet needed (POTA)
3. **QMX GPS** (if QMX has GPS) — re-syncs the RTC every 5 minutes
4. **Manual** — you can set time via the settings drawer

See [Time Sync](time-sync.md) for details.

---

**Next:** Learn about [FT8 Transmit](ft8-tx.md) or go back to [Panadapter](panadapter.md).
