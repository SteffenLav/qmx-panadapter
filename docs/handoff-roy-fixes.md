# Handoff: Roy KI0ER feedback fixes — build, flash, test

## Context
Branch **`claude/roy-groups-io-comments-3n0bmq`** implements fixes for feedback from
**Roy KI0ER** on the QRPLabs groups.io "QMX/QMX+ Panadapter for M5Stack Tab5" thread.
Two commits, authored in a remote (cloud) session; **not yet built or flashed to
hardware** at the time this handoff was written. Task for the shack session: build,
flash, and run the testing protocol below. No antenna needed for most of it (FT8
Simulation Mode); one item (F) needs a live band.

Commits:
- `1f1472c` — intelligent Transmit, reply-window + armed early-cut, pileup/ADIF lockout, reader retry
- `3f75589` — full WSJT-X cold-pounce (early-decode-during-monitoring toggle)

## Get the code + flash
```powershell
git fetch origin claude/roy-groups-io-comments-3n0bmq
git checkout claude/roy-groups-io-comments-3n0bmq
qmx bfm
```
No `fullclean` — an incremental build keeps `managed_components/`, so the standing
patches (esp_hosted PSRAM/SDIO, fatfs exFAT, hcd bulk) do NOT need re-applying. Only
re-apply those if you deliberately wipe/refresh dependencies. If a fresh shell where
the profile helper isn't loaded:
```powershell
& "C:\esp\v5.4.4\esp-idf\export.ps1" | Out-Null; idf.py build flash monitor
```

## What changed (5 items) and where

**1. Intelligent Transmit** — `main/ft8_qso.c` `ft8_qso_build_manual_reply()`, called from
`main/ui/ft8_screen_view.c` `row_activate()`; Pounce-gating in `main/ui/ft8_tx_modal.c`.
Tapping a decoded row's **Transmit** now sends the correct *next* message derived from
what that station last sent (WSJT-X double-click logic): their CQ→my grid; their grid→my
report; their report→R+my report; their R-report→RR73; their RR73/73→73. The R/report
value is *our* measured SNR of them, not an echo. **Auto Pounce** now appears only for a
fresh CQ (grid TX1); on a mid-QSO row you get Transmit only (its legend hides too). Field
Day rows keep plain TX1.

**2. Reply timing** — `main/ft8_test.c`: `FT8_REPLY_TX_WINDOW_MS` 2500→2800 (hard ceiling —
the decoder's candidate time search tops out at DT ~+3.0 s; `ft8_lib/ft8/decode.c` line ~259,
`time_offset -10..+19` blocks × 0.16 s). Early-decode cut now also fires when a reply is
merely ARMED, so a hand-armed exchange lands on-beat at DT≈0.

**3. Pileup/ADIF lockout** — `main/ui/ft8_screen_view.c`: short-press unchanged;
**long-press always opens the ADIF log** even while the button shows "Pileup"; one-time
toast teaches it when the button first flips.

**4. Reader first-open** — `main/net/reader_net.c`: waits up to 30 s for WiFi
("Waiting for WiFi…") then downloads, instead of demanding a reboot. Body text in
`main/ui/reader_view.c` softened to match.

**5. Full cold-pounce (toggle, default ON)** — setting `ft8_early_decode`
(`main/storage/settings.{c,h}`), toggle **"Fast pounce (early decode)"** in the FT8 drawer
under "Distance in miles" (`main/ui/ui.c`), read in `main/ft8_test.c` `want_early_cut`.
Runs the early-decode cut during plain monitoring so decodes surface before the slot
boundary (WSJT-X-style), letting a cold pounce fire in its own slot rather than a cycle
(30 s) later. **Trade-off:** capture stops ~1.8 s early (period − `FT8_DECODE_RESERVE_MS`,
~13.2 s); with our ~560 ms RX latency the margin is thin, so late-transmitting stations can
get their tail clipped and miss decoding.

## Testing protocol

### A. Sanity (no antenna, QMX on)
1. Boot clean, confirm UI comes up, FT8 mode entered.
2. FT8 drawer shows **"Fast pounce (early decode)"** row under "Distance in miles", checked
   by default. Toggle it off/on, reboot, confirm it persists (NVS).

### B. Intelligent Transmit — the headline (FT8 Simulation Mode, no antenna)
1. FT8 drawer → enable **FT8 Simulation Mode** (breathing red border appears; QMX is never
   keyed — hard interlock in `ft8_tx.c`).
2. Phantom W1AW/K9ZZ call CQ. **Tap a CQ row → modal shows your grid message**
   ("W1AW &lt;you&gt; &lt;grid&gt;"), Auto Pounce present. Hit **Transmit**.
3. After the phantom's next message, **tap that row again** → modal should now show the
   **next** step (report / R-report / 73 depending on what they sent), Auto Pounce hidden,
   legend gone. Transmit it.
4. Walk a full QSO to 73 by tapping+Transmit each step. **Pass = the message advances each
   time; never stuck on grid.**
5. Watch serial: `manual reply to … -> …` logs the chosen message.
6. Cross-check the report value is *your* SNR of them, not an echo of their number.

### C. Reply timing (Sim mode)
- Use **Auto Pounce** on a phantom CQ; confirm the exchange fires on-beat (no double-sends,
  no full-cycle stalls). Serial: `reply armed mid-slot (+Nms)`.

### D. Pileup/ADIF lockout (Sim mode CQ-run)
1. Call CQ in sim so a pileup builds (button flips to "Pileup", toast
   "hold this button for the ADIF log" appears once).
2. **Short-press** "Pileup" → pileup viewer. **Long-press** → ADIF log (this is the fix —
   the log must always be reachable).

### E. Reader retry
- Power-cycle with WiFi configured, open **User Manual immediately** (before WiFi fully
  associates). Should show "Waiting for WiFi…" then download — **no reboot needed**.

### F. Cold-pounce + YIELD (needs a live band + antenna — the one to scrutinize)
This is the risky one. Do a careful A/B:
1. Pick a busy band, note conditions/time.
2. **Toggle ON**: run ~10–15 slots, record **unique decodes/slot** (serial per-slot line)
   and whether a **cold pounce** (tap a fresh CQ, hit Pounce/Transmit quickly) fires in the
   *same* reply slot rather than 30 s later.
3. **Toggle OFF**: same band, same duration, record decodes/slot again.
4. **Decision:** if decodes/slot with it ON is materially lower (late stations dropping
   out), that's the expected trade-off — report the numbers. If yield holds and cold pounces
   now land in-slot, it's a win. Either way the toggle lets the operator choose.
- Watch for: stations that used to appear now missing; `off=%+dms` staying near 0 (capture
  still anchored to the UTC boundary).

## Known / not done
- **No `docs/version-history.md` entry or version bump** yet — add if you want a changelog
  for this branch.
- Field Day manual-Transmit stays plain TX1 (smart-advance intentionally gated off in FD
  mode; Pounce still runs the FD sequence).
- The `ft8_early_decode` setting is **not** in the config export/import (`config_io.c`) —
  deliberate; defaults on.
- No PR — this project doesn't use them; commit + push to the branch is the workflow.

## If the build breaks
Paste the compiler error to the session. The changes touch `main/ft8_qso.{c,h}`,
`main/ft8_test.c`, `main/ui/ft8_screen_view.c`, `main/ui/ft8_tx_modal.c`,
`main/net/reader_net.c`, `main/ui/reader_view.c`, `main/storage/settings.{c,h}`,
`main/ui/ui.c`.

## Roy's original four points (for reference when replying to the thread)
1. User Manual said "no cache — connect to WiFi" though WiFi was up; worked only after a
   reboot. → fix #4.
2. FT8 Transmit fired ~30 s late and only ever sent his grid. → fixes #1 (grid-only) + #2/#5
   (timing).
3. Pounce also fired 30 s late; other software transmits immediately a few seconds late. →
   fixes #2/#5 (decoder DT limit is ~+3 s, so "immediately" means within that window, which
   early-decode surfacing enables).
4. "ADIF-log" button stuck on "Pileup", couldn't reach the log. → fix #3.
