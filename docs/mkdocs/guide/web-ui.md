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

- **Frequency** — tap to open numeric keypad, or click the spectrum to tune
- **Mode** — buttons for USB, LSB, CW, DiGi
- **Bandwidth** — selectors for SSB filter width or CW passband
- **Band** — jump between configured bands
- **Memory** — recall saved channels
- **Zoom** — pinch or scroll to zoom the spectrum

Everything mirrors the Tab5 display with sub-second latency.

## Spectrum Waterfall

The waterfall is **live-streamed** from the Tab5 every ~100 ms. You see the same real-time signal activity as on the device itself.

Click anywhere on the spectrum to tune to that frequency. The waterfall updates continuously — no refresh needed.

## FT8 Control

From the web UI you can:

- **View the live FT8 decode list** (read-only from web; use the Tab5 to reply)
- **Monitor signal strength** (S-meter)
- **Watch power/SWR readout** (post-transmit)

FT8 transmit must be initiated from the Tab5 (safety feature — only one interface can key the QMX at a time).

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
- **Diag log ↓** — diagnostic log for troubleshooting
- **Tab5 Screenshot** — current display as PNG

## Upload Functions

- **Config ↑** — restore settings from a backup file
- **QRZ ↑** — upload ADIF to QRZ Logbook (requires API key)
- **eQSL ↑** — upload ADIF to eQSL (requires username/password)

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

---

**Next:** Set up [Time Sync](time-sync.md) or explore [Settings](settings.md).
