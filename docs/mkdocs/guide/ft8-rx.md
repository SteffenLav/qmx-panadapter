# FT8/FT4 Receive & Decode List

The panadapter includes **on-device FT8 and FT4 decoders** with real-time spectrum waterfall and a live list of heard stations.

## 1. FT8 & FT4 View

Swipe → from the left edge to toggle to FT8/FT4 view. The same decode list and waterfall work for both modes — switch modes via the **Preset** dropdown in the left pane (top).

**FT4 notes:**
- Decodes refresh faster (~15 ms vs 15 s for FT8) because slots are 7.5 seconds
- Slot countdown shows 7.5 s instead of 15 s
- All filtering, priority, and auto-reply features work identically in FT4
- Time-sync from decoded signal timing (Sync Time button) is FT8-only for now

You'll see:

- **Decode list** (left pane) — all heard stations, sortable by signal strength, distance, or newest
- **Waterfall** (right pane) — same real-time spectrum as panadapter mode
- **Call CQ button** — start a CQ (requires QMX on the air)
- **Filter button** — include/exclude stations, prioritise by SNR/distance, show only CQ callers

## 2. Decode List

The list shows every decoded FT8 message:

| Column | Meaning |
|---|---|
| **Callsign** | Their call, or `CQ` if they're calling CQ |
| **Report** | SNR (signal-to-noise) or grid square they're sending you |
| **Grid** | Their grid square (estimated from signal geometry) |
| **Time** | When the message was decoded (slot boundary) |

**Own call highlight** — your callsign is shown in **inverted colours** (red fill, white text) so you spot replies to you instantly.

## 3. Filtering & Priority

Tap the **Filter** button to open the filter modal:

- **Include callsigns** — only show these calls (space or comma-separated)
- **Include callsigns containing** — show any call with these substrings (e.g., `/P` for portable)
- **Exclude callsigns** — hide these calls
- **Exclude if contains** — hide any call matching these substrings
- **Exclude worked before** — skip calls you've already logged QSOs with
- **Show only CQ callers** — hide replies, show only CQ messages
- **Auto-reply priority** — Strongest SNR, Weakest SNR, Most distant grid
- **Robot mode** — auto-answer CQ (disabled by default; see [FT8 Transmit](ft8-tx.md))

All filters persist across sessions. Toggle any filter on/off to enable or disable it without erasing the criteria.

## 4. Decoding Performance

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

## 5. Real-Time Waterfall

Unlike WSJT-X (which buffers a whole 15-second capture), the Tab5 **builds the waterfall on-the-fly** as audio arrives. You see CW and other traffic in real time, and FT8 stations appear instantly as their symbol blocks are decoded.

The waterfall also shows **non-FT8 activity**:
- CW (carrier lines)
- SSB (broader vertical smears)
- RTTY (FSK sidebands)
- Interference (wideband noise)

## 6. Tap to Reply

Tap any FT8 row in the decode list to **prepare a reply**:

1. A confirmation modal pops up showing your message
2. You can review the target callsign, grid, and SNR
3. Tap **Transmit** to send, or **Cancel** to abort

The reply always follows FT8 protocol (correct parity, proper message sequence) — you can't accidentally send a garbled or out-of-order message.

## 7. Auto-Reply (Robot Mode)

⚠️ **Experimental** — enabled via a checkbox in the Filter modal.

When robot mode is on, the Tab5 **automatically replies to CQ** without waiting for you to tap. It scans each FT8 slot for CQ callers matching your filters, picks the highest-priority station, and sends a full QSO exchange (TX1 → wait for report → TX2 → wait for RR73 → TX3). Everything is logged to ADIF.

**Important:** Robot mode **keys the QMX for real** — your signal goes on the air. Never leave it unattended unless you're confident in your filters and your station is in a safe state.

## 8. Search & Window

The decoder uses an **FFT-based search window** to find candidate tone blocks. The search is tuned for standard FT8 tone spacing (6.25 Hz per tone).

**Frequency stability:** The QMX's USB audio clock isn't bit-exact 48 kHz, so decodes slide slowly over time. The panadapter **re-locks to UTC boundaries every 15 seconds** (the slot boundary), preventing long-term drift.

## 9. Time Sync for FT8

FT8 requires **UTC time accurate to ±1 second** for slot synchronisation. The panadapter gets time from:

1. **WiFi + SNTP** (if available) — most accurate, syncs every ~hour
2. **Tab5 RTC** (if set) — accurate to ±1 min, no internet needed (POTA)
3. **QMX GPS** (if QMX has GPS) — re-syncs the RTC every 5 minutes
4. **Manual** — you can set time via the settings drawer

See [Time Sync](time-sync.md) for details.

---

**Next:** Learn about [FT8 Transmit](ft8-tx.md) or go back to [Panadapter](panadapter.md).
