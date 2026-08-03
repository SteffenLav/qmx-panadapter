# Web UI

Access the panadapter remotely from any browser on your WiFi network.

## Quick Start

1. Enable **WiFi** in the settings drawer
2. Note the **IP address** shown in the settings
3. Open your browser to `http://<ip>` (e.g., `http://192.168.1.50`)
4. You'll see the same spectrum and waterfall as the Tab5

No installation, no configuration — just open and go.

## Remote Control

The web UI provides full remote control:

- **Frequency** — tap the VFO to open the numeric keypad (drag it by its title bar to reposition; toggle between a 10-key and a phone-style digit layout), or click the spectrum to tune
- **Mode** — buttons for USB, LSB, CW, DiGi
- **Bandwidth** — selectors for SSB filter width or CW passband
- **Band** — jump between configured bands
- **Memory** — recall saved channels
- **Zoom** — pinch or scroll to zoom the spectrum

Everything mirrors the Tab5 display with sub-second latency.

## Spectrum Waterfall

The waterfall is **live-streamed** from the Tab5 every ~100 ms. You see the same real-time signal activity as on the device itself.

Click anywhere on the spectrum to tune to that frequency. The waterfall updates continuously — no refresh needed.

**Whole-band plan strip.** Along the bottom of the view (above the status bar) is a colour-coded CW/Digi/Phone strip spanning the entire current band, mirroring the one on the Tab5. A draggable "visible window" marks the slice currently on screen — drag it, or tap anywhere on the strip, to retune — and a marker shows the VFO position.

**Adjustable split.** Drag the divider between the spectrum and the waterfall to give either one more room. Your chosen split is remembered in the browser.

> **In FT8 or FT4 mode the live stream pauses.** The browser stops streaming the spectrum/waterfall and shows a notice plus the log and upload controls instead. This is deliberate — while you're operating digital modes the stream would compete with the on-device decoder and the WiFi link, so pausing it keeps FT8 decoding and WiFi noticeably steadier (new in v0.20.0). Switch the Tab5 back to Panadapter mode and the stream resumes automatically.

## FT8 Control

When the Tab5 is in **FT8/FT4 mode**, the browser pauses the live spectrum stream (see [Spectrum Waterfall](#spectrum-waterfall) above) and instead shows a **live TX status banner** plus the **log and upload controls** — download your ADIF, upload to QRZ/eQSL/LoTW, grab the diagnostic log. Operate FT8 (watch the decode list, tap to reply, call CQ) **on the Tab5 itself**.

The status banner (new in v1.3.6) mirrors the Tab5's own TX label so you can watch the radio from another room: **red** while transmitting — including the "call 2 of 4" counter when a [CQ stop limit](ft8-tx.md) is set — **amber** when a transmission is armed or a QSO is waiting, **green** on QSO complete, **orange** on timeout, and the persistent **"CQ stopped after N calls - no answer"** once an auto-stopped CQ run ends. The browser tab's title also shows a red dot while transmitting, so even a background tab signals when the radio is on the air.

FT8 transmit can only be initiated from the Tab5 — a safety feature, since only one interface should key the QMX at a time.

## CAT Control (Advanced)

The **CAT** section lets you send raw Kenwood-style commands directly to the QMX:

```
FA;        → reads current frequency
FA14074000;  → sets frequency to 14.074 MHz
MD;        → reads current mode
MD2;       → sets USB mode
```

This is for advanced troubleshooting — most users don't need it.

## Bottom Bar Menus

The bottom bar groups its actions into three popup menus, plus a battery indicator (e.g. `🔋 87% (8.0V)`):

**QSO Logs (n) ▲** — only visible when QSOs exist; *n* is the QSO count:

- **ADIF download ↓** — QSO log as an ADIF file (import into WSJT-X, EQSL, etc.)
- **QRZ upload ↑** — upload ADIF to QRZ Logbook (requires API key on first use, saved for future sessions)
- **eQSL upload ↑** — upload ADIF to eQSL (requires username/password on first use, saved)
- **LoTW setup** / **LoTW ↑** — upload ADIF to ARRL's Logbook of The World (see [LoTW Upload](#lotw-upload) below)
- **View / edit log** — opens the QSO log right in the browser (call, mode, band, frequency, date/time, reports, grid — newest first). **Click any column header to sort** by it (click again to reverse) — sorting by Date groups an activation's QSOs together. Each row has a ✕ to delete that one record, and a **Delete all** button clears the whole log (no undo, so it asks you to type `DELETE` to confirm — download the ADIF first if you want a copy). Handy before a POTA activation: start with an empty log and the ADIF at the end is exactly the file you submit

**Files ▲**:

- **Config download ↓** — all settings as a text file (backup or transfer to another Tab5)
- **Config upload ↑** — restore settings from a backup file
- **SD Files** — opens the **microSD file browser** (`http://<tab5-ip>/files`, new in v1.3.0): browse the card from your computer without pulling it — download logs, config backups and the offline manual; upload files; delete
- **Diagnostic download ↓** — downloads **both** diagnostic logs: the live session log (always on, nothing to enable) and the flash-persisted copy from before the last reboot/power-off

**Miscellaneous ▲**:

- **Tab5 screenshot** — current display as PNG, including any open pop-up (band/mode dropdown), not just the base screen
- **Reset settings** — clear stored settings back to defaults (see [Troubleshooting](../reference/troubleshooting.md))
- **Reset WiFi** — clear just the WiFi/network state

## LoTW Upload

Uploading to ARRL's **Logbook of The World** requires an existing LoTW account and a callsign certificate (made with ARRL's TQSL program).

**One-time setup:** open the **QSO Logs** menu → **LoTW setup**. A guided two-page window walks you through it:

1. **Page 1** explains how to export your callsign certificate from the TQSL program on your PC (**Callsign Certificate → Save the Callsign Certificate**, which produces a `.p12` file), with a button that opens ARRL's own instructions.
2. **Page 2** imports the `.p12` file, its passphrase, and your DXCC entity (plus optional CQ/ITU zones, and **US state and county** — see below). The `.p12` is parsed **in the browser** — the passphrase never reaches the device.

After setup the button reads **LoTW ↑**. Each click signs all not-yet-uploaded QSOs on the device with your certificate and uploads them to lotw.arrl.org.

**US stations: fill in state and county** (new in v1.3.3). These were not being sent at all before, which meant US operators' uploaded QSOs earned **no Worked All States and no county credit** — for them or for the stations they worked. Fill them in if your TQSL station location has them. The county is the **name on its own** (`Arlington`), not `VA,Arlington`. Operators outside the US can leave both blank.

**Certificate renewal** — LoTW certificates expire roughly every 3 years. **Ctrl-click** the **LoTW ↑** button to re-run setup. From v1.3.3, re-submitting the *same* certificate no longer re-uploads your whole log — only an actually-changed certificate resets the upload position, because a new key means every QSO has to be re-signed. So you can go back in to add a state and county without resending everything.

**If an upload does not appear on LoTW**, check [ARRL's queue status](https://www.arrl.org/logbook-queue-status) before assuming a fault — the queue has run hours behind at busy times. LoTW rejects a malformed file at upload time, so anything that reached the queue was signed correctly.

## Upload Behaviour

Uploads work **while FT8 or FT4 is actively running** — the panadapter briefly pauses the FFT and SD-archive activity during the HTTPS transfer, then resumes automatically. A result is shown once the upload completes, reporting how many QSOs were sent.

Each upload remembers where it left off — re-uploading skips QSOs that were already sent in a previous session.

## Network Requirements

- **WiFi must be on** (settings drawer)
- **IP address shown** in settings (or static IP if you prefer)
- **Both Tab5 and browser on the same LAN** (no internet needed)
- **5 GHz WiFi works** but 2.4 GHz is recommended (longer range)

## Limitations

- **Transmit from web** — not supported (must use Tab5)
- **Real-time chat** — no operator messaging
- **Export formats** — ADIF only (import to EQSL, WSJT-X, etc. yourself)
- **Latency** — ~200 ms typical (WiFi dependent)
- **Stale connection recovery** — if the browser's network drops without a clean disconnect (e.g., putting a laptop to sleep), the web view may freeze briefly before recovering. Recovery is automatic and capped at ~5 seconds.

---

**Next:** Set up [Time Sync](time-sync.md) or explore [Settings](settings.md).
