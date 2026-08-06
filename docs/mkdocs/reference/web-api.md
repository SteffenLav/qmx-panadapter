# Web API Reference

The Tab5 runs an HTTP server that exposes a REST API for remote control and diagnostics. All endpoints are on port 80.

## Base URL

```
http://<tab5-ip-address>/api/
```

Example: `http://192.168.1.50/api/status`

## Status Endpoints

### GET /api/status

Returns the current state of the panadapter.

**Response** (JSON):

```json
{
  "battery":  { "level": 85, "mv": 8000, "charging": false },
  "wifi":     { "ssid": "MyNet", "rssi": -65, "ip": "192.168.1.50" },
  "freq_hz": 14074000,
  "qmx_fw": "1_03_002",
  "mode": "DIGI",
  "band": "20m",
  "screen": "ft8",
  "ft8":      { "st": "armed", "text": "TX armed: CQ OZ1LAV JO65 - call 2 of 4 (~7s)" },
  "passband_hz": 2700,
  "signal_dbm": -83.5,
  "zoom": 1.0,
  "pan_bins": 0,
  "cw_pitch_hz": 700,
  "if_cal_hz": 0,
  "flat_mode": false,
  "utc_epoch": 1785340050,
  "qso_count": 42,
  "qrz_key_set": true,
  "eqsl_creds_set": false,
  "lotw_ready": true,
  "bandplan": { "lo": 14000000, "hi": 14350000, "segs": [ { "lo": 14000000, "hi": 14070000, "c": "20B0FF", "l": "CW" } ] },
  "tab5_fw": "v1.5.0",
  "bands":    [ { "name": "20", "center_hz": 14100000 } ]
}
```

Notes:

- `screen` is `"panadapter"` or `"ft8"`.
- **`ft8` is present only while the FT8/FT4 screen is live** — the engine does not run otherwise, so its text would be stale. `st` is one of `active` / `armed` / `done` / `timeout` / `wait` / `idle`, and `text` is the same line the Tab5 shows on its own TX status label. This is what drives the browser's TX banner and the red dot in the tab title.
- `signal_dbm` and `bandplan` are `null` when unavailable (no spectrum yet; VFO outside a known band).
- `passband_hz` falls back to a per-mode default until CAT reports a real width.

## Control Endpoints

### POST /api/cmd

Send a command to the panadapter. The command name is the **`action`** key; each
action names its own parameter key (there is no generic `value`).

**Request** (JSON):

```json
{
  "action": "set_freq",
  "hz": 14074000
}
```

**Supported actions**:

| Action | Parameter | Effect |
|---|---|---|
| `set_freq` | `hz` | Set VFO frequency |
| `set_band` | `hz` (band centre) | Switch band — recalls the frequency last used on it |
| `set_mode` | `mode: "USB"/"LSB"/"CW"/"DIGI"/"AM"` | Change mode (AM needs QMX firmware 1.04+) |
| `set_bw` | `hz` | Filter width — SSB at 1000 Hz and above, CW passband below |
| `set_zoom` | `zoom: 1.0/2.0/4.0/8.0` | Zoom level |
| `cq_start` | *(none)* | Start a CQ run, as the Tab5's own **Call CQ** button does. **Keys the radio.** Only acted on while the Tab5 is in FT8/FT4 mode; otherwise discarded, not queued |
| `reset_settings` | *(none)* | Clear stored settings back to defaults |
| `reset_network` | *(none)* | Clear the stored WiFi/network state |

The response is `{"ok":true}`; an unrecognised action returns `400`. Effects are
applied asynchronously (mode and filter writes are queued onto the CAT poll task,
`cq_start` onto the display task), so read `/api/status` to see the result rather
than assuming it from the response.

## CAT Endpoints

### POST /api/cat

Send a raw CAT command.

**Request** (JSON):

```json
{
  "cmd": "FA;"
}
```

**Response** (JSON):

```json
{
  "response": "FA14074000;",
  "error": null
}
```

### GET /api/cat/freq

Query frequency.

**Response**:

```json
{
  "frequency_hz": 14074000
}
```

### GET /api/cat/mode

Query mode.

**Response**:

```json
{
  "mode": "USB",
  "mode_code": 2
}
```

## FT8 Endpoints

### GET /api/ft8/decodes

List recent FT8 decodes.

**Response**:

```json
{
  "decodes": [
    {
      "callsign": "K9ZZ",
      "grid": "EN52",
      "report": "-07",
      "time_slot": 14,
      "signal_dbm": -88,
      "frequency_hz": 14074093
    }
  ],
  "timestamp": "2026-06-27T14:07:30Z"
}
```

### POST /api/ft8/transmit

Initiate FT8 transmission (requires Tab5 to be in FT8 mode).

**Request**:

```json
{
  "message": "K9ZZ OZ1LAV JO45"
}
```

## ADIF Endpoints

### GET /api/adif

Download ADIF log.

**Response**: ADIF file (text/plain)

### POST /api/adif/clear

Clear all QSOs from ADIF log.

**Response**:

```json
{
  "status": "cleared",
  "count": 42
}
```

## Configuration Endpoints

### GET /api/config

Download current configuration (all settings + memory channels).

**Response**: INI text file

### POST /api/config

Upload a configuration file to restore settings.

**Request**: multipart form-data with file upload

**Response**:

```json
{
  "status": "imported",
  "merged": true
}
```

## Logging Endpoints

### GET /api/log

Download diagnostic log.

**Response**: Text file (qmx-log.txt)

### POST /api/log/clear

Clear diagnostic log buffer.

**Response**:

```json
{
  "status": "cleared"
}
```

## Upload Endpoints

### POST /api/qrz_key

Set QRZ API key (stored server-side).

**Request**:

```json
{
  "api_key": "1a2b3c4d5e6f..."
}
```

### POST /api/qrz_upload

Upload ADIF to QRZ Logbook.

**Response**:

```json
{
  "uploaded": 5,
  "failed": 0,
  "error": null
}
```

### POST /api/eqsl_creds

Set eQSL username and password.

**Request**:

```json
{
  "username": "oz1lav",
  "password": "secretpass"
}
```

### POST /api/eqsl_upload

Upload ADIF to eQSL.

**Response**:

```json
{
  "uploaded": 5,
  "failed": 0,
  "error": null
}
```

### POST /api/lotw_cert

Store the LoTW callsign certificate and key (extracted from the TQSL `.p12` in the browser during LoTW setup).

### POST /api/lotw_upload

Sign all pending QSOs with the stored certificate and upload them to lotw.arrl.org.

**Response**:

```json
{
  "uploaded": 5,
  "failed": 0,
  "error": null
}
```

### GET /api/lotw_tq8

Download the whole log as a signed `.tq8` file, for manual upload to LoTW or offline verification.

**Response**: `.tq8` file (gzipped, signed)

## WebSocket (Streaming)

The web UI uses WebSocket for **live spectrum streaming**:

```
ws://<tab5-ip>:80/ws
```

The server streams FFT magnitude bins and waterfall data every ~100 ms. This is handled automatically by the web UI — you don't need to implement it yourself unless building a custom client.

## Error Responses

All endpoints return errors in a standard format:

```json
{
  "error": "CAT timeout",
  "status": 500
}
```

Common HTTP status codes:

- **200** — Success
- **400** — Bad request (invalid command)
- **500** — Server error (QMX not responding, etc.)
- **503** — Service unavailable (WiFi off, etc.)

## Example: Tune to 7.074 MHz in Python

```python
import requests

ip = "192.168.1.50"
api = f"http://{ip}/api"

# Set frequency
requests.post(f"{api}/cmd", json={"cmd": "set_frequency", "value": 7074000})

# Set mode
requests.post(f"{api}/cmd", json={"cmd": "set_mode", "value": "USB"})

# Check status
status = requests.get(f"{api}/status").json()
print(f"Now on {status['frequency_hz']} Hz, {status['mode']} mode")
```

---

**Next:** See [Troubleshooting](troubleshooting.md) for API debugging.
