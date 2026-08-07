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
| `set_screen` | `screen: "panadapter"/"ft8"` | Switch the Tab5's own view. Applied within about a second (handed to the display task) |
| `cq_start` | *(none)* | Start a CQ run, as the Tab5's own **Call CQ** button does. **Keys the radio.** Only acted on while the Tab5 is in FT8/FT4 mode; otherwise discarded, not queued |
| `reset_settings` | *(none)* | Clear stored settings back to defaults |
| `reset_network` | *(none)* | Clear the stored WiFi/network state |

The response is `{"ok":true}`; an unrecognised action returns `400`. Effects are
applied asynchronously (mode and filter writes are queued onto the CAT poll task,
`cq_start` onto the display task), so read `/api/status` to see the result rather
than assuming it from the response.

### GET /api/memory

All 32 memory channels: `{"slots":[{"idx":0,"occupied":true,"freq_hz":14074000,"mode":"DIGI","label":"FT8"}, ...]}`.
Empty slots carry only `idx` and `occupied:false`.

### POST /api/memory

Write one channel - `{"idx":3,"freq_hz":14074000,"mode":"DIGI","label":"FT8"}` -
or clear one with `{"idx":3,"clear":true}`. Fields you omit keep their stored
value, so renaming a channel needs only `idx` and `label`.

A frequency outside a recognised amateur band returns **400** - the same refusal
the Tab5 makes, so the browser cannot be a way around it.

There is no recall endpoint: recalling a channel is `set_freq` plus `set_mode`,
which `/api/cmd` already does.

### GET /api/settings

The settings a browser can usefully edit: callsign, grid, the CQ presets, the FT8
filters, the everyday toggles, QMX volume, band-plan region and the WiFi SSID.

**The WiFi password is never returned** - only `wifi_pass_set` says whether one is
stored.

### POST /api/settings

Applies a **partial** update: only the keys present are touched, everything else
is left alone. So a single toggle is a one-key body, and a stale browser form can
never reset fields it did not show.

```json
{ "my_grid": "JO65ab", "rbn_en": true, "cq": { "max_calls": 10 } }
```

`wifi_ssid` and `wifi_pass` take effect only when both are supplied - an empty
password means "keep the stored one", never "erase it". `qmx_vol_db` is in dB
(0-50) and is sent to the radio as well as stored.

### GET /api/decodes

The FT8/FT4 decode list as the Tab5 shows it - same ordering (the station being
worked, then messages addressed to you, then CQ calls, then strongest signal) and
the same Filter-window hides applied. Read-only.

```json
{
  "mode": "FT8",
  "working": "DK7CVD",
  "rows": [
    { "call": "DK7CVD", "text": "OZ1LAV DK7CVD -07", "grid": "JO31",
      "snr": -7, "hz": 1503, "dt": 210, "age": 6, "heard": 3,
      "sl": "E", "me": true, "cq": false, "pin": true }
  ]
}
```

`dt` is milliseconds, `hz` the station's audio tone, `age` seconds since it was
last decoded, `sl` the transmit window it was heard in. `me` marks a message
containing your callsign, `pin` the station you are working.

### GET /api/help

The guidance rows, ranked by the device from its own live state. `flagged` marks a
row the firmware can see is happening right now.

```json
{
  "context": { "page": "guide/panadapter.md", "anchor": "Layout", "label": "Panadapter" },
  "rows": [
    { "symptom": "My radio is not showing up", "flagged": true,
      "page": "reference/troubleshooting.md", "anchor": "won't reconnect",
      "label": "Radio not connecting" }
  ]
}
```

`context` is the chapter the Tab5's own **User Manual** button would open right
now. `anchor` is a case-insensitive heading substring, not a URL fragment.

### GET /api/manual?page=guide/ft8-tx.md

One page of the built-in manual, as the raw markdown that built the documentation
site. `page=toc.json` returns the contents list. Served from the firmware, so it
needs no internet and always matches the running version. 404 if the page is not
in this firmware's manual.

## CAT Endpoints

There is no raw-CAT HTTP endpoint. Send CAT-level changes through
`POST /api/cmd` (`set_freq`, `set_mode`, `set_bw`, `set_band`), which routes them
through the CAT poll task - the only writer to the radio's serial port. The web
UI's own **CAT** box uses those actions.

## FT8 Endpoints

The decode list is `GET /api/decodes` (above). Transmit from the browser is
limited to `POST /api/cmd {"action":"cq_start"}`; there is no endpoint that sends
an arbitrary FT8 message, deliberately - replies are chosen from the decode list
in front of you, at the Tab5.

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

**Request**: the config file as the raw request body (not multipart)

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

### GET /api/log/saved

The flash-persisted copy of the diagnostic log from before the last reboot or
power-off - the one that survives a crash in the field.

**Response**: Text file

### GET /api/signal

The signal level alone, for a faster S-meter than the 1 Hz `/api/status` poll can
give. Same DSP call and parameters as `/api/status` and the Tab5's own S-meter -
only the cadence differs.

```json
{ "dbm": -83.5 }
```

### POST /api/adif/delete?idx=<n>&call=<CALL>

Delete one QSO. **Both** the index and the callsign must match the record, or the
request is refused with `409` - so a stale browser view can never delete the wrong
contact.

### GET /api/upload_status

Progress of a running QRZ/eQSL/LoTW upload (`busy`, `kind`, `uploaded`, `failed`,
`error`).

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
