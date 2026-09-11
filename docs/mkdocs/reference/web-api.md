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
  "bt":       { "en": true, "conn": false },
  "tab5_fw": "v1.7.2",
  "bands":    [ { "name": "20", "center_hz": 14100000 } ]
}
```

Notes:

- `screen` is `"panadapter"` or `"ft8"`.
- **`ft8` is present only while the FT8/FT4 screen is live** — the engine does not run otherwise, so its text would be stale. `st` is one of `active` / `armed` / `done` / `timeout` / `wait` / `idle`, and `text` is the same line the Tab5 shows on its own TX status label. This is what drives the browser's TX banner and the red dot in the tab title.
- `bt` is the Bluetooth mouse: `en` is whether it is switched on, `conn` whether a mouse is connected right now. `en: true, conn: false` means it is scanning or waiting for a paired mouse to come back.
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
| `set_rit` | `hz` | RIT offset. Retuning clears it |
| `set_screen` | `screen: "panadapter"/"ft8"` | Switch the Tab5's own view. Applied within about a second (handed to the display task) |
| `set_ft8_mode` | `mode: "ft8"/"ft4"`, optional `freq_hz` | **Switch between FT8 and FT4**, optionally retuning. Omit `freq_hz` to keep the current frequency. This is an action rather than a setting because it also retunes the radio and clears stale decodes — read `ft8_op_mode` back from `/api/settings` to confirm |
| `set_activation` | `ref` | Set the POTA/SOTA activation reference |
| `cq_start` | *(none)* | Start a CQ run, as the Tab5's own **Call CQ** button does. **Keys the radio.** Only acted on while the Tab5 is in FT8/FT4 mode; otherwise discarded, not queued |
| `reply` | `call` | Work a decoded station — the same intelligent Transmit a Tab5 row-tap runs. **Keys the radio.** Outcome in `/api/status` `ft8.web_r` |
| `qso_override` | `what: "resend"/"rr73"/"73"/"cancel"` | Mid-QSO override, as the Tab5's own three buttons do. **Keys the radio** except for `cancel` |
| `clear_swr` | *(none)* | Clear a latched SWR-protection trip, same as tapping the Tab5's own fault prompt. TX stays refused until this is called |
| `tune_start` / `tune_stop` | *(none)* | Antenna Tune (QMX 1.04+ only). **Keys the radio continuously**; 60 s safety stop on the device; live power/SWR in `/api/status` `tune` while running |
| `pause` / `resume` | *(none)* | Release the radio so its own menus can be used, and take it back. While paused the CAT poll, the dead-stream watchdog and the stuck-decode watchdog all stand down |
| `greylist_clear` | *(none)* | Un-skip every grey-listed station |
| `ota_install` | `url` | Download and install firmware. Refused while transmitting or mid-QSO, with the reason in the reply body. Does **not** restart on its own |
| `ota_reboot` | *(none)* | Restart into firmware already installed |
| `usb_shutdown` | *(none)* | Orderly USB teardown before a reflash: radio to RX, CAT and audio closed, VBUS dropped |
| `usb_replug` | *(none)* | Power-cycle the USB port to recover a wedged device |
| `power_off` | *(none)* | Power the Tab5 down |
| `reset_settings` | *(none)* | Clear stored settings back to defaults |
| `reset_network` | *(none)* | Clear the stored WiFi/network state |

**Radio menus** (the QMX's own terminal, on its second serial port):
`open`, `close`, `key`, `text`, `term_view`, `qmx_ports`, `qmx_term_probe` —
see [Radio menus](#) and `GET /api/term`.

**Developer actions.** Not referenced by any web-UI element, and easy to regret:

| Action | Effect |
|---|---|
| `cat_raw` | Send one raw CAT string to the radio |
| `drawer` | Open the settings drawer, optionally at `scroll_y`, so a section below the fold can be screenshotted |
| `resmon` | Toggle the resource-monitor overlay |
| `time_redetect` | Re-arm the once-per-boot QMX GPS detection without rebooting |
| `help` | Open the on-device manual at a topic |
| `panic_test` | **Crash the device deliberately**, to prove the crash recorder works. ⚠ Radio **off** only — a panic reboot is a warm reset, which leaves the QMX needing a power cycle |

**⚠ Check the response BODY, not the status code.** The reply is `{"ok":true}`
on success — but an unrecognised action returns **HTTP 200** with the body
`unknown action`, and a refused one (for example `ota_install` while
transmitting) returns `{"ok":false,"error":"..."}`, also with HTTP 200. A caller
that only tests the status code cannot tell success from a typo, and a mistyped
action is then a silent no-op.

Effects are applied asynchronously — mode and filter writes are queued onto the
CAT poll task, and `cq_start`, `reply`, `qso_override` and `set_ft8_mode` onto
the display task — so read `/api/status` or `/api/settings` to see the result
rather than assuming it from the response.

### GET /api/tone

The TX audio tone and the live occupancy of the 200-2800 Hz window.

```json
{ "hz": 1650, "hold": false, "min": 200, "max": 2800, "step": 50,
  "stations": 14, "busy": "0000000000111110...", "partner_hz": 1750, "busy_tx": false }
```

`busy` is one character per 50 Hz slot (`1` = a station or its guard band), sent
as a **string** because 52 bits do not survive JavaScript's number type.
`busy_e` and `busy_o` carry the **EVEN and ODD windows separately** — two
stations only collide in the same window, and only the operator knows which one
they are about to transmit into; `busy` remains the our-window/union view the
automatic verdict uses.
`stations` is how many decoded stations fed the mask - **zero means the band
picture is unknown, not empty**, and a caller must say so rather than showing the
all-clear.

### POST /api/tone

`{"hz":1650}` and/or `{"hold":true}`. Out-of-range frequencies are clamped.

A tone change while a QSO is running is applied to the running exchange first
(your partner tracks your slot, not your tone). Mid-burst it is **refused with
409** and the body carries the device's own reason - the preference is not stored
either, so the two can never end up half-committed.

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
The spot sources (`spots_en` overall, then `rbn_en` and `cluster_en`),
propagation feedback (`psk_rx_en`) and the Bluetooth mouse (`bt_mouse_en`) are
here too — `bt_mouse_en` is stored immediately but only takes effect after a
restart. `swr_limit_x10` (the SWR limit × 10, so `30` is 3.0:1) is **reported
but not writable** — a transmit safety limit is set at the device.

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

**Everything the settings drawer can change is reachable here.** Besides the keys
above that includes `flat_mode`, `distance_in_miles`, `greylist_en`,
`ft8_early_decode`, `field_day_en` (with `fd_class` / `fd_section`), `spots_en`,
`sota_en`, `rbn_en`, `pskreporter_en`, `resmon_en`, `tx_tone_hold`, `tx_tone_hz`,
`cw_pitch_hz` and `cw_cal_hz` — all readable back from `GET`.

**`sim_mode_en` is the one to know about if you are automating tests.** With it
on, the firmware runs complete FT8 exchanges against phantom stations and
`ft8_tx.c` hard-refuses to key the radio, so a QSO can be driven end to end with
no antenna and no QMX attached. ⚠ Simulated contacts land in the **real** ADIF
log, so turn it off afterwards and delete them — they have `FREQ=0`, and the
ADIF viewer's "Del N test" button finds exactly those.

**Read-only reporters.** `ft8_op_mode` (`"ft8"` / `"ft4"`) and `ft8_freq_hz` are
returned but not writable here — change them with the `set_ft8_mode` action,
which also retunes the radio and clears stale decodes, then read these back to
confirm. `swr_limit_x10` is likewise reported only.

### GET /api/decodes

The FT8/FT4 decode list as the Tab5 shows it - same ordering (the station being
worked, then messages addressed to you, then CQ calls, then strongest signal) and
the same Filter-window hides applied. Read-only.

```json
{
  "mode": "FT8",
  "miles": false,
  "working": "DK7CVD",
  "rows": [
    { "call": "DK7CVD", "text": "OZ1LAV DK7CVD -07", "grid": "JO31",
      "snr": -7, "hz": 1503, "dt": 210, "age": 6, "heard": 3,
      "km": 612, "brg": 173,
      "sl": "E", "me": true, "cq": false, "pin": true }
  ]
}
```

`dt` is milliseconds, `hz` the station's audio tone, `age` seconds since it was
last decoded, `sl` the transmit window it was heard in. `me` marks a message
containing your callsign, `pin` the station you are working.

`km` and `brg` (v1.8.3) are distance and bearing from your grid to theirs, worked
out on the device with the same maths the Tab5's own list uses. **Both are omitted
entirely when either grid is missing** — there is no null and no zero, so a client
should render an absent field as "unknown" rather than as a distance. `km` is in
miles when the top-level `miles` flag is true, which mirrors the *Distance in
miles* setting.

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

`?activation=<REF>` limits the download to contacts made during an activation of
that reference (matched on `MY_SIG_INFO`), so a park's log can be uploaded on its
own rather than the whole file. An unknown reference returns a valid ADIF file
with no records, not an error.

`?date=today` or `?date=YYYYMMDD` limits it to one UTC day, and names the download
`qso-YYYY-MM-DD.adi` so a daily file is self-identifying once it is saved. A day
with no contacts returns a valid empty ADIF, not an error.

### POST /api/adif/import

Restore records from a raw ADIF file — the browser's own "ADIF download", or any
other logger's export. Body is the ADIF text itself (`Content-Type: text/plain`).

`?mark_uploaded=0` imports the contacts as **not yet uploaded**, so the next QRZ/
eQSL/LoTW upload pass sends them. Default (`1`, or the parameter omitted) marks
them as already uploaded instead — advancing all three upload cursors to the new
record count — since the ordinary reason to restore a log is that these contacts
were already sent before whatever wiped the device.

A pretty-printed, multi-line record (as some loggers export) is normalised to
this device's own one-line-per-record form before being written. A record whose
`CALL`+`QSO_DATE`+`TIME_ON` already matches one in the log is skipped, so
re-importing the same file twice — or a file that overlaps an earlier partial
restore — adds nothing extra.

**Response**:

```json
{ "ok": true, "added": 12, "total": 47 }
```

`added: 0` with `ok: true` means every record in the file was already logged.
`ok: false` means the log could not be written at all (storage full).

### POST /api/adif/edit

Correct **one field of one record**: `?idx=<n>&call=<CALL>&field=<F>&value=<V>`.

`idx` is the record's position in the file and `call` must match the callsign
already stored there — both, or the device answers `409` and changes nothing, so
a browser showing a stale list cannot edit the wrong QSO.

Only three fields are accepted: `RST_SENT`, `RST_RCVD` and `SIG_INFO`. An empty
`value` removes the field, which for a report is the honest record of one that
was never exchanged. `SIG_INFO` is the reference the *other* station was
activating (`US-1241`, `G/LD-049`, `DLFF-0123`); the device derives `SIG` from
its shape and writes it alongside, and clears it again when the reference is
cleared. Everything else is refused with `400`: callsign, band, mode, date and
time are what QRZ, eQSL and LoTW match a contact on, and a LoTW record is signed
over exactly those.

**Response**: `{"ok":true}`, or `{"ok":false,"error":"…"}`

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

### GET /api/psk_rx

Propagation feedback — which receivers have copied **your** call, from PSK
Reporter.

```json
{ "enabled": true, "age_s": 96, "receivers": 14, "max_km": 7412,
  "reports": [
    { "call": "W1ABC", "grid": "FN42", "dxcc": "United States", "mode": "FT8",
      "f": 14074812, "t": 1785340050, "snr": -14, "km": 6031, "brg": 291 }
  ] }
```

Requesting this asks for a refresh, but the collector's own **five-minute floor**
is enforced on the device, so polling faster than that returns the same data with
a larger `age_s` rather than fetching again. `age_s` is `-1` before the first
successful query.

`snr` is omitted when the receiver gave none — 0 dB is a real report, so it
cannot double as "unknown". `km` and `brg` are omitted together when the
receiver's grid is unknown.

Off by default. The endpoint always answers: with the feature disabled nothing is
fetched, so `enabled` is `false` and `reports` holds whatever the last query
returned before it was switched off — read `age_s` rather than assuming the list
is current. It is also empty while offline or before a callsign is set.

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
