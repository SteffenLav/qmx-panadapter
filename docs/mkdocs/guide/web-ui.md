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

When the Tab5 is in **FT8/FT4 mode**, the browser pauses the live spectrum stream (see [Spectrum Waterfall](#spectrum-waterfall) above) and instead shows a status notice plus the **log and upload controls** — download your ADIF, upload to QRZ/eQSL, grab the diagnostic log. Operate FT8 (watch the decode list, tap to reply, call CQ) **on the Tab5 itself**.

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

## Download Links

Bottom bar offers several downloads:

- **ADIF ↓** — QSO log as an ADIF file (import into WSJT-X, EQSL, etc.)
- **Config ↓** — all settings as a text file (backup or transfer to another Tab5)
- **Diag ↓** — live diagnostic log for troubleshooting (always on, nothing to enable); **Diag(saved) ↓** — the copy persisted to flash from before the last reboot/power-off
- **Tab5 Screenshot** — current display as PNG, now including any open pop-up (band/mode dropdown), not just the base screen

## Upload Functions

- **Config ↑** — restore settings from a backup file
- **QRZ ↑** — upload ADIF to QRZ Logbook (requires API key on first use, saved for future sessions)
- **eQSL ↑** — upload ADIF to eQSL (requires username/password on first use, saved)

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
