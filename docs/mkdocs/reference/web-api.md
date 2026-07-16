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
  "frequency_hz": 14074000,
  "mode": "USB",
  "bandwidth_hz": 2500,
  "band": "20m",
  "s_meter": "S7",
  "signal_dbm": -83.5,
  "waterfall_enabled": true,
  "wifi_connected": true,
  "wifi_rssi": -65,
  "ip_address": "192.168.1.50",
  "battery_percent": 85,
  "callsign": "OZ1LAV",
  "grid": "JO45",
  "qmx_connected": true,
  "qmx_fw": "1_03_002",
  "time_utc": "2026-06-27T14:07:30Z",
  "uptime_seconds": 3661,
  "ft8_enabled": true,
  "ft8_decoding": false,
  "ft8_decode_count": 42
}
```

## Control Endpoints

### POST /api/cmd

Send a command to the panadapter.

**Request** (JSON):

```json
{
  "cmd": "set_frequency",
  "value": 14074000
}
```

**Supported commands**:

| Command | Parameter | Effect |
|---|---|---|
| `set_frequency` | `value: Hz` | Set VFO frequency |
| `set_mode` | `value: "USB"/"LSB"/"CW"/"DIGI"` | Change mode |
| `set_bandwidth` | `value: Hz` | Set filter width (SSB) |
| `set_band` | `value: "20m"` etc | Switch band |
| `enable_wifi` | `value: true/false` | Enable/disable WiFi |
| `set_zoom` | `value: 1.0/2.0/4.0/8.0` | Zoom level |

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
