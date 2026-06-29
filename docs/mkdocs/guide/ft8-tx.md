# FT8 and FT4 Transmit

The panadapter **keys the QMX and transmits full FT8/FT4 QSOs** — reply to CQ, run your own CQ, auto-answer (robot mode), or conduct a full exchange.

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

⚠️ **FT4 support is new (v0.19.0).** Full feature parity with FT8; time-sync from decoded signals is not yet available in FT4 mode (FT8-only for now).

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
┌──────────────────────────┐
│ About to transmit:       │
│                          │
│  TX1: K9ZZ OZ1LAV JO45   │
│                          │
│ [Auto Pounce]  [Cancel]  │
│ [Transmit]               │
└──────────────────────────┘
```

Review the message (callsign, your grid), then:

- **Transmit** — send the reply immediately (next FT8 slot)
- **Auto Pounce** — automatically handle the full exchange (reply + wait for report + TX report back + wait for RR73 + TX 73)
- **Cancel** — abort

The reply follows **correct FT8 parity** — if you're replying to an even slot, you transmit on the odd slot, and vice versa.

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
2. Enter your API key (QRZ) or username/password (eQSL)
3. Click **Upload**

Logs are batched — each session records which QSOs have been uploaded, so re-running the upload skips already-submitted QSOs.

### 12. Troubleshooting

**"QMX not responding to TX command"**

- Check USB cable (must be data cable, not charge-only)
- Verify QMX is in the correct mode (USB/LSB/CW, not AM/FM)
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
