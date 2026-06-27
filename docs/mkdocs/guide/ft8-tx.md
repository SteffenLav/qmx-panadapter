# FT8 Transmit

The panadapter **keys the QMX and transmits full FT8 QSOs** — reply to CQ, run your own CQ, auto-answer (robot mode), or conduct a full exchange.

## ⚠️ Important Notes

**FT8 transmit is functional but not yet soaked for multi-hour sessions.** Known gaps:

- No duty-cycle protection (you could transmit continuously if filters match poorly)
- No audio loopback verification (can't confirm modulation before keying)
- No over-temperature monitoring

Standard operating practice applies:

1. **Use a dummy load** for your first tests
2. **Monitor power and SWR** if you have a meter
3. **Watch the displayed TX power** (should stabilise at ~5 W on QMX+)
4. **Never leave auto-reply unattended** unless your station is attended

The beta label goes away at v1.0.0 after multi-day soak testing.

## Modes of Transmission

### 1. Reply to a CQ

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

## ARRL Field Day Mode

During [ARRL Field Day](https://www.arrl.org/field-day), enable **Field Day mode** in the settings:

- **Mode** — on/off
- **Class** — your transmitter class (1–2 kW)
- **Section** — your ARRL section

When FD mode is active:

- **CQ message changes** to `CQ FD` (replaces any other modifier)
- **Exchange message** includes your class and section (e.g., `5B WCF` = class 5, section WCF)
- **ADIF record** logs `CONTEST_ID=ARRL-FD`, your section, and their section

The full FD exchange is automatic — no manual mode switching needed.

## Message Status

The FT8 left pane shows a **status label** indicating what's happening:

| Status | Meaning |
|---|---|
| **ACTIVE** (red) | Transmitting right now |
| **ARMED** (amber) | Ready to transmit next slot |
| **QSO Complete** (green) | Exchange finished, logged to ADIF |
| **QSO Timeout** (orange) | No response after 4 slots, exchange aborted |
| *(dim white)* | Idle, no active QSO |

Tap the status label to **abort** the current QSO (only works if ARMED or ACTIVE; once COMPLETE, it's logged).

## Power & SWR Readout

After each transmit burst, the status bar briefly shows:

```
Last TX: 5.2W SWRx1.25 [N=78]
```

- **5.2W** — average output power measured via QMX `PC;` CAT command
- **SWRx1.25** — SWR measured via QMX `SW;` CAT command (only valid during TX)
- **[N=78]** — number of symbols transmitted (always 79 for FT8)

These readings are **informational only** — the panadapter does not enforce limits. Monitor your antenna system independently.

## CQ Presets

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

## Frequency & Tone Control

When a CQ is active, the panadapter holds your **CQ tone** (6.25 Hz audio frequency) fixed across slots. If another station lands on your tone during an exchange, you'll see a warning **⚠ FREQ BUSY** on the status label — but the QSO continues on the same tone (doesn't auto-relocate mid-exchange).

To tune to a different frequency while CQ is running:

1. Tap the **Freq** label on the top bar
2. Enter a new frequency
3. The CQ continues on the new frequency next slot

## ADIF Logging

Every completed QSO is **automatically logged to ADIF** on the Tab5's internal storage:

- **Callsign**, grid, report (SNR), time (UTC)
- **Frequency**, mode (`FT8`)
- **Duration** (QSO start to finish)
- **Operator** (your callsign)

Download the log via the web UI (**ADIF ↓** button) or Settings → ADIF Log.

## Upload to QRZ & eQSL

Via the web UI:

1. Click **QRZ ↑** or **eQSL ↑** in the bottom bar
2. Enter your API key (QRZ) or username/password (eQSL)
3. Click **Upload**

Logs are batched — each session records which QSOs have been uploaded, so re-running the upload skips already-submitted QSOs.

## Troubleshooting

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

**Next:** Learn about [ADIF Logging](../reference/adif.md) or [Web UI](web-ui.md).
