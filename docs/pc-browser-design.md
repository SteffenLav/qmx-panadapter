# Tab5 simulator - the Tab5 screen in a browser, on any PC

Status: **DESIGN, nothing built.** 2026-11-15. Goal set by the operator: a
**Tab5 simulator** - the Tab5's own screen and behaviour, pixel for pixel, in a
browser on any PC with the QMX on that PC's USB. No Tab5, no install.

Why the Tab5 screen and not the web page (operator): the UI is the reason
users choose this project. A browser app with a different UI is one more of
the many ham PC apps already doing the same job. The Tab5 build is unchanged
and stays the reference.

## 1. What is measured

| Fact | Source |
|---|---|
| Chrome on Windows delivers the QMX+ I/Q untouched: 2 ch, 47999 samples/s over 506 s, every sample an exact 24-bit integer, L/R independent, 36.3 dB image rejection (raw) | `tools/browser_probe/iq_probe.html`, commit `5b7bf30a` |
| Web Serial opens the QMX CDC port; `Q9 1;` accepted and read back | same run |
| The web page is a thin client: 88 `fetch` sites over 38 endpoints (`/api/cmd` 19, `/api/settings` 14), one WebSocket `/ws` carrying spectrum, audio and takeover frames | `main/net/www/index.html` |
| FFT, zoom-FFT, smoothing, viewport, demodulation, all decoders and all state run in the firmware, not in the page | same |
| `webserver.c` calls **75 distinct `ui_*`/`lv_*` functions**: part of the model lives in the LVGL UI layer (`ui.c`) | grep, 2026-11-15 |
| The firmware calls **248 distinct platform functions** (ESP-IDF, FreeRTOS, drivers). Most belong to modules the PC version drops (WiFi, netif, OTA, LCD, BSP, GPIO, I2C, SD, codec, MQTT) | grep, 2026-11-15 |
| 79 host test harnesses already build firmware C on a PC | `test/` |
| Emscripten is not installed on this machine | `which emcc` |

Not measured yet: Chrome on macOS/Linux, a run of hours, decoder CPU in WASM,
the real size of the shim layer (section 5, step 0 measures it).

## 2. Options

**A. Compile the firmware itself to WebAssembly ("virtual Tab5").**
The whole firmware, LVGL and `ui.c` included, runs in a Web Worker. Hardware
calls go to thin shims. `index.html` runs unchanged: its `fetch('/api/...')`
and `WebSocket('/ws')` are routed in-page to the firmware's own handlers.

**B. Rewrite the backend in JavaScript; compile only the decoders.**
`cmd_handler` alone has 121 action branches, and the QSO state machines,
band guard, ADIF and settings would all exist twice.

**Recommendation: A.** One codebase, one behaviour. The 75 `ui_*` calls are
the deciding fact: in B every one of them is model logic to re-write; in A
they come along for free. A fix on the Tab5 is a fix on the PC.

**Decided by the operator 2026-11-15: option A, and the UI is a visual copy of
the Tab5 screen.** No Tab5 is needed. LVGL draws the 1280x720 Tab5 screen into
a `<canvas>` - the same pixels as the device, not the web page. The same build
is also the demo (replay source instead of the QMX).

Consequences of the Tab5 screen as the UI:

- `index.html` is not needed for the PC version. The `/api` and `/ws` shim
  drops out of the first steps; `webserver.c` still compiles (it is coupled to
  `ui.c`) but nothing calls it.
- **Input:** mouse = LVGL pointer (touch). The firmware already takes a BLE
  mouse (right-drag pans, wheel) and a BLE / snap-on keyboard; the PC feeds the
  same input paths. Two-finger pinch has no mouse equivalent - the wheel must
  zoom. To verify: every gesture in `docs/mkdocs/reference/gestures.md` has a
  mouse/keyboard way.
- **Scaling:** the canvas scales to the window. A 1366x768 laptop shows it near
  1:1; a smaller window shrinks the text with it.
- **Device-only controls** have no meaning on a PC and must hide or stub:
  battery, WiFi setup, OTA update, SD card, GPS module, brightness, power off.
- **Audio out:** the Tab5 codec path becomes Web Audio output.

## 3. Architecture (option A)

```
 Browser tab (main thread)                  Web Worker (WASM, pthreads)
 ------------------------                   ---------------------------
 index.html (unchanged)                      firmware C: dsp, decoders, cat,
   fetch('/api/..') --+                      ft8/js8/rtty/wspr/cw, adif,
   WebSocket('/ws') --+-> transport shim --> webserver handlers, ui.c + LVGL
                                             (hidden display, or a canvas)
 getUserMedia (QMX audio) --AudioWorklet---> rx path (replaces UAC host)
 Web Serial (QMX CDC) <--------------------> cat.c (replaces cdc_acm)
 IndexedDB <-------------------------------> NVS + SPIFFS (Emscripten IDBFS)
```

Shims, by group:

| Firmware uses | Browser replacement |
|---|---|
| FreeRTOS tasks, semaphores, queues, stream/ring buffers, event groups | pthreads + a FreeRTOS-API shim (or the FreeRTOS POSIX port), Emscripten `-pthread` |
| `esp_timer`, `esp_log`, `heap_caps_*` | `emscripten_get_now`, `console`, `malloc` |
| `nvs_*`, SPIFFS/VFS | a small NVS-on-file shim; IDBFS so settings and the ADIF log survive a reload |
| `uac_host_*`, `usb_host_*` | samples from the AudioWorklet into the same ring the UAC callback fills |
| `cdc_acm_*` | Web Serial through a JS bridge |
| `httpd_*` (25 functions used) | an in-page request object; responses go back as `Response` objects / WS frames |
| `esp_http_client`/`esp_https_*` | `fetch` - see the CORS limit below |
| WiFi, netif, OTA, LCD, BSP, GPIO, I2C, SD, codec, battery | compiled out for this board |

Threads need `SharedArrayBuffer`, so the page must be served with
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`.

## 4. Limits known before any work

- **Chrome and Edge only.** Firefox and Safari have no Web Serial.
- **CORS.** A browser can only call another site that allows it. QRZ, eQSL,
  LoTW, wsprnet, POTA, spothole and PSK Reporter must each be checked; any
  that refuses needs a small relay or is left out of the PC version.
- **No raw sockets.** PSK Reporter reports (UDP) and the rigctld server
  (TCP 4532) cannot run in a browser. rigctld is the bigger loss for a PC
  user, because loggers expect it.
- **One program on the COM port.** The page cannot share the QMX's CAT port
  with another program, the same as any PC app.
- **TX.** Every keying site keeps `tx_band_guard_allows()`; the guard is
  compiled in, not re-written. `check_tx_guard.py` and
  `check_unattended_tx.py` must cover this build too.
- **Audio out.** The page's existing web-audio path plays the demodulated
  audio; no change needed.

## 5. Plan

| Step | What | Done when | Cost |
|---|---|---|---|
| 0 | **Measure the shim surface.** Install Emscripten, compile `main/` + LVGL with empty stub headers, list unresolved symbols | the real list of functions to shim, by group | 1 day |
| 1 | Panadapter only: I/Q + CAT -> FFT -> LVGL Tab5 screen on a canvas. Tune by mouse, band, mode, zoom by wheel | the canvas shows the same spectrum as a Tab5 on the same signal | 1-2 weeks |
| 2 | Settings and log persistence (IDBFS); audio out | settings survive a reload | days |
| 3 | Decoders: FT8/FT4 receive, then RTTY, CW, JS8, WSPR | decode counts match the Tab5 on the same band within a few % | 1-2 weeks |
| 4 | TX: FT8/FT4 first, band guard proven by the checkers | an FT8 QSO from the browser, refused out of band | 1 week |
| 5 | Network services that pass CORS; relay decision for the rest | | open |
| 6 | Hosting on tab5.lav.dk with COOP/COEP headers, or as a downloadable file | | open |

Step 0 is the cheapest decisive instrument: it costs no hardware time and
turns "how big is the port" from an estimate into a list.

## 6. Decisions for the operator

1. ~~Option A~~ - **decided 2026-11-15: A, with the Tab5 screen as the UI.**
2. **Hosting:** served from tab5.lav.dk (needs the two headers; check that
   Simply allows them in `.htaccess`), or a local file/PWA.
3. **Services that fail CORS:** a relay you host, or leave them out.
4. **Strategy:** a PC version lets people use the UI without buying a Tab5.
