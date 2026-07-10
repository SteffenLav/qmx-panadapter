# FT8 and FT4 Transmit

The panadapter **keys the QMX and transmits full FT8/FT4 QSOs** — reply to CQ, run your own CQ, auto-answer (robot mode), or conduct a full exchange.

> **⏸️ FT4 is temporarily disabled in v0.20.0.** It was exhausting the device's internal memory and crashing, so it is switched off this release while that is fixed — fully reversible, and **FT8 is unaffected**. FT4 transmit returns in a later update; the FT4-specific details on this page apply to when it is re-enabled.

### 1. FT8 vs FT4 — Which to Use?

Both modes transmit via the same CAT interface; the difference is **slot length and symbol rate**:

| Feature | FT8 | FT4 |
|---------|-----|-----|
| **Slot length** | 15 seconds | 7.5 seconds |
| **Symbols transmitted** | 79 | 105 |
| **Symbol duration** | 160 ms | 48 ms |
| **QSO time** (full exchange) | ~90 sec (6 slots) | ~45 sec (6 slots) |
| **Use case** | Weak signal, SOTA/POTA | Busy bands, contests |
| **Preset picker** | Panadapter mode → Freq dropdown | FT8 mode → Preset picker |

**Choose FT4 when:**
- The band is crowded (faster exchanges, less QRM)
- You're in a contest (time is critical)
- You want quicker pileup resolution

**Choose FT8 when:**
- Signal is marginal (longer decode window tolerates more QRN)
- You're portable/POTA (less pressure to transmit fast)
- You prefer the relaxed pace

FT4 has full feature parity with FT8, including time-sync from decoded signals (fixed in v0.19.3 — the time-sync modal shows "FT4" when appropriate and the bottom-bar clock correctly reads `UTC(FT4)` while in FT4 mode).

### 2. Switching Between FT8 and FT4

**In FT8 screen (left pane):**
- Tap the **Preset** dropdown (shows current frequency/mode)
- Select an FT4 frequency to switch to FT4
- Select an FT8 frequency to switch back

**In Panadapter screen:**
- Tap the **Band preset dropdown** (top left)
- Some bands have both FT8 and FT4 frequencies listed
- Tap the FT4 variant to switch

Your **settings and decode list are sticky** — when you switch back to FT8 or FT4, the panadapter remembers your last frequency, bandwidth, and filter settings for each mode.

### 3. Important Notes

**FT8 and FT4 transmit are functional but not yet soaked for multi-hour sessions.** Known gaps:

- No duty-cycle protection (you could transmit continuously if filters match poorly)
- No audio loopback verification (can't confirm modulation before keying)
- No over-temperature monitoring

Standard operating practice applies:

1. **Use a dummy load** for your first tests
2. **Monitor power and SWR** if you have a meter
3. **Watch the displayed TX power** (typically ~3–5 W, depending on band and supply voltage)
4. **Never leave auto-reply unattended** unless your station is attended

The beta label goes away at v1.0.0 after multi-day soak testing.

### 4. Modes of Transmission

#### 1. Reply to a CQ

In FT8 view, tap a CQ row in the decode list. A confirmation modal appears:

```
┌──────────────────────────────┐
│ Confirm FT8 Transmission     │
│ ▲  K9ZZ EN52  -07  2138 Hz  │ ← nudge up
│ ►  W1AW FN31  -12  1406 Hz  │ ← selected row
│ ▼  SV1ENG JN37 -09 1869 Hz  │ ← nudge down
│                              │
│  TX1: W1AW OZ1LAV JO45      │
│                              │
│  [▲ Nudge up]  [▼ Nudge down]│
│  [Auto Pounce]    [Cancel]   │
│  [Transmit ●]                │
└──────────────────────────────┘
```

- **Transmit** (green) — send the reply on the next correct slot
- **Auto Pounce** (blue) — handle the entire exchange automatically (TX1 → wait for report → TX2 → wait for RR73 → TX3 73)
- **▲ / ▼ Nudge** — move the target to the row above or below, without closing the modal and redoing the selection gesture
- **Cancel** — abort

**A quick tap is enough** — you do not need to hold a row before releasing. Hold-and-drag still works for selecting across rows in a busy list.

The reply follows **correct FT8/FT4 parity** — if you're replying to an even slot, you transmit on the odd slot, and vice versa. For FT4 the countdown timer correctly counts down in 7.5-second slot increments, not 15-second FT8 ones.

**Skip TX1 (faster pounce).** With **Skip TX1** enabled in the Filter modal, pouncing a station opens with your signal report straight away instead of the grid exchange — saving one round trip. If the station has already dropped out of the decode list, it falls back to the normal grid exchange automatically.

#### Working a pile-up

When you call CQ (or work a busy run), more than one station may answer at once — and a caller who replies while you're mid-QSO with someone else used to vanish from the live decode list as soon as they stopped transmitting. They are now kept in a **Pileup** list so you don't lose them:

- Whenever one or more stations are waiting, the **ADIF-log** button on the FT8 screen becomes a **Pileup** button in a distinct colour, and reverts once the list empties.
- Tap **Pileup** to see everyone who has called you and isn't worked yet.
- Tap a station in the list to work them (the same confirmation modal as a decode-list tap), or tap the small **✕** to dismiss them without working them.
- A station is removed from the list automatically once you start a QSO with them.

The pile-up tracker never transmits on its own — it only remembers callers; you choose who to work.

### 2. Call CQ

Tap the **Call CQ** button. A modal opens:

```
┌──────────────────────────┐
│ CQ Preset:               │
│ [○] 1. CQ OZ1LAV JO45    │
│ [○] 2. DX OZ1LAV JO45    │
│ [○] 3. POTA OZ1LAV JO45  │
│                          │
│ [Save Preset] [Transmit] │
└──────────────────────────┘
```

Choose a preset (or edit/save a new one), then tap **Transmit**. The QMX starts calling CQ and listening for replies.

**CQ tone selection** — the panadapter automatically picks a quiet frequency (6.25 Hz tone spacing, 200–2800 Hz audio range, no other stations nearby). If your chosen tone gets busy during a QSO, the panadapter **auto-relocates to the nearest clear frequency** to avoid stepping on other stations.

### 3. Auto-Reply (Robot Mode)

⚠️ **Requires explicit checkbox in the Filter modal** to enable.

When robot mode is on and you're idle, the panadapter scans every FT8 decode for CQ callers matching your filters, picks the highest-priority match (Strongest SNR, Weakest SNR, or Most distant grid — your choice), and starts a full QSO:

1. **TX1** — send your call, grid
2. **RX** — wait for their report
3. **TX2** — send their report + your call back
4. **RX** — wait for RR73 or 73
5. **TX3** — send 73 or RR73 (exchange complete)
6. **Log** — ADIF entry created, move to next CQ

Each QSO takes **~90 seconds** (6 FT8 slots × 15 s/slot). If a station doesn't respond in 4 slots, the panadapter times out and resumes scanning CQ.

**Important:** Robot mode **transmits on the air unattended**. Never enable it unless:

- Your station is physically attended
- Your filters are tested and correct
- Your antenna/SWR is known good
- Your QMX power is set appropriately

A permanent on-screen warning appears whenever robot mode is available.

### 4. FT8 Simulation Mode (Practice QSOs)

⚠️ **For practice only** — no real stations involved, radio never keys up.

Enable **FT8 Simulation Mode** in the settings drawer to practice full QSO exchanges with phantom stations (W1AW and K9ZZ) without transmitting on the air. Useful for:

- Learning the full FT8 exchange sequence
- Testing your setup without risking interference
- Verifying ADIF logging works correctly
- Practicing in a realistic environment

**How it works:**
- Two phantom stations periodically call CQ on their own schedule
- You can reply, they respond, and complete a full QSO
- Messages and timing are identical to real FT8 — the decoder runs actual GFSK synthesis
- Every simulated QSO logs to ADIF with the phantom callsign

**Visual indicator:** When simulation mode is active, a **10 px red border pulses around the entire screen** (breathing red frame). This is your visual reminder that you're in practice mode — if the red frame is gone, simulation is off and real stations are in play.

**Important:** 
- The QMX is **never keyed** in simulation mode, no matter what
- QSOs are logged as real ADIF entries (you may want to delete practice contacts from your log afterward)
- All other features (panadapter, web UI, settings) work normally

### 5. ARRL Field Day Mode

During [ARRL Field Day](https://www.arrl.org/field-day), enable **Field Day mode** in the settings:

- **Mode** — on/off
- **Class** — your transmitter class (1–2 kW)
- **Section** — your ARRL section

When FD mode is active:

- **CQ message changes** to `CQ FD` (replaces any other modifier)
- **Exchange message** includes your class and section (e.g., `5B WCF` = class 5, section WCF)
- **ADIF record** logs `CONTEST_ID=ARRL-FD`, your section, and their section

The full FD exchange is automatic — no manual mode switching needed.

### 6. Message Status

The FT8 left pane shows a **status label** indicating what's happening:

| Status | Meaning |
|---|---|
| **ACTIVE** (red) | Transmitting right now |
| **ARMED** (amber) | Ready to transmit next slot |
| **QSO Complete** (green) | Exchange finished, logged to ADIF |
| **QSO Timeout** (orange) | No response after 4 slots, exchange aborted |
| *(dim white)* | Idle, no active QSO |

Tap the status label to **abort** the current QSO (only works if ARMED or ACTIVE; once COMPLETE, it's logged).

### 7. Power & SWR Readout

After each transmit burst, the status bar briefly shows:

```
Last TX: 5.2W SWRx1.25 [N=79]
```

- **5.2W** — average output power measured via QMX `PC;` CAT command
- **SWRx1.25** — SWR measured via QMX `SW;` CAT command (only valid during TX)
- **[N=79]** — number of symbols transmitted
  - FT8: always 79 symbols (~12.7 s at 160 ms/symbol)
  - FT4: always 105 symbols (~5.0 s at 48 ms/symbol)

These readings are **informational only** — the panadapter does not enforce limits. Monitor your antenna system independently.

### 8. CQ Presets

You can save up to 3 custom CQ messages:

1. Tap **Call CQ**
2. Tap **Edit Presets** (or long-press the active preset)
3. Type your message: callsign, modifier (optional), grid
4. Tap **Save**

Presets persist across power cycles. Common modifiers:

- **DX** — calling DX only
- **POTA** — Parks on the Air activation
- **SOTA** — Summits on the Air activation
- *(blank)* — standard CQ

### 9. Frequency & Tone Control

When a CQ is active, the panadapter holds your **CQ tone** (6.25 Hz audio frequency) fixed across slots. If another station lands on your tone during an exchange, you'll see a warning **⚠ FREQ BUSY** on the status label — but the QSO continues on the same tone (doesn't auto-relocate mid-exchange).

To tune to a different frequency while CQ is running:

1. Tap the **Freq** label on the top bar
2. Enter a new frequency
3. The CQ continues on the new frequency next slot

### 10. ADIF Logging

Every completed QSO is **automatically logged to ADIF** on the Tab5's internal storage:

- **Callsign**, grid, report (SNR), time (UTC)
- **Frequency**, mode (`FT8`)
- **Duration** (QSO start to finish)
- **Operator** (your callsign)

Download the log via the web UI (**ADIF ↓** button) or Settings → ADIF Log.

### 11. Upload to QRZ & eQSL

Via the web UI:

1. Click **QRZ ↑** or **eQSL ↑** in the bottom bar
2. Enter your API key (QRZ) or username/password (eQSL) when first prompted — credentials are saved for future sessions
3. Click again to upload

Logs are batched — each upload session records which QSOs have been sent, so re-running skips already-submitted entries.

Uploads work **while FT8 or FT4 is actively running** — the panadapter briefly steps the FFT and SD-archive activity aside for the duration of the HTTPS transfer, then resumes automatically. You typically miss one FT8 slot and won't notice. A progress result is shown once the upload completes.

### 12. Troubleshooting

**"QMX not responding to TX command"**

- Check USB cable (must be data cable, not charge-only)
- Verify QMX is in the correct mode (USB/LSB/CW/DiGi, not AM)
- Try a manual `TX;` command via the web UI's CAT tab

**"⚠ FREQ BUSY warning, but no other station is there"**

- The panadapter's tone-clash detector can be overly sensitive on a busy band
- Tap the status label to clear the warning and continue
- See [Settings → FT8 Filters](settings.md) for tone-clash sensitivity

**"QSO timeout — no response"**

- The called station didn't hear you (too weak, bad timing, etc.)
- Try a different station, or switch to manual mode and check levels first
- See [Troubleshooting](../reference/troubleshooting.md) for more

---

**Next:** Set up [Time Sync](time-sync.md) for accurate FT8 timing, or explore the [Web UI](web-ui.md).
