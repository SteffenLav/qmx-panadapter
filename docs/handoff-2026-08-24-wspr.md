# WSPR handoff — night of 2026-08-23/24

Branch `feat/wspr-page`, worktree `C:/dev/qmx-panadapter-wspr`. Local only,
never pushed. Five commits on top of the merge with v1.9.3.

## The headline: OZ1LAV was heard in Australia

The WSPR TX engine went on the air for the first time, supervised, one burst at
**21:28 UTC on 14.09710 MHz, 5 W**. **50 stations decoded it**, 913 km to
**15,663 km** (VK5WA/2 and VK5ARG in Australia, WA2TP on the US east coast).

Grid `JO65` and power `+37 dBm` came back identical on all 50 rows, the
frequency landed within ~10 Hz of prediction, and **drift was 0 on every single
spot** — 50 independent receivers confirming the tone stepping and symbol
timing under real RF, which no dry run can prove. Full detail in
`docs/wspr-phase2-status.md`.

`WSPR_TX_SEND_LIVE` is **0** in git and always has been. The transmitting build
was a temporary local edit, reverted in the source immediately after flashing.
Going on air stays a deliberate act with the operator awake.

## What else changed, in order

| commit | what |
|---|---|
| `6ebaee9` | First hardware burst observed; **66 ms of symbol jitter** found and fixed (task priority 1 → 5) with a controlled A/B |
| `3db70a8` | **On the air**, plus the missing 1 s start offset and dry runs no longer needing a radio |
| `922cffc` | Merge of v1.9.3 into the branch (was 26 commits behind) |
| `846fe3e` | The decoder **could not run on the device at all** — three fixes, and the real blocker measured |
| `9b443fb` | **Decimating front end** — RX now fits the cycle, 456 % → 53.4 % |
| `000ed08` | `/api/wspr` + the web spot panel, and an SNR I refused to invent |

## The RX story, which is the substantial engineering

Phase 1 was proven as an **algorithm**, on a laptop, and had never been sized
for the target. The first on-device run returned `0 candidates in 0 ms` — which
is impossible for an FFT over 1.44 M samples, and the only fast exit is a failed
allocation. Three host-shaped assumptions:

- `build_twiddles()` wanted **46.1 MB** (tables as long as the capture)
- `wspr_find_candidates()` wanted ~23 MB (one FFT over the whole capture)
- three live `double tp[162][4]` arrays wanted ~18 KB of **stack**, against 16 KB

All three fixed, each verified behaviour-preserving against the real reference
WAV *before* going near the device. Then the timing answer: it decoded
**correctly** on the P4 — exact message, `dt=1.600s` matching the synthesized
offset, `cycles=81` identical to the host — and took **67 s per candidate**,
i.e. 456 % of a 120 s cycle. Even two candidates was 120 %, so trimming the list
could not have saved it.

Root cause was **memory bandwidth, not CPU**: ~630 M PSRAM reads per candidate.
The fix is what `wsprd` already does — mix each candidate to complex baseband
and decimate 32× before correlating. Result: **67 s → 6.8 s per candidate,
64 s total, 53.4 % of a cycle**, with the real WAV, the sensitivity floor
(−22.7 dB) and the Fano cycle counts all **unchanged**.

## Where it stands

**Works, hardware-verified:** TX (on the air, 50 spots), the decoder on P4
silicon (3 stations separated at 1420/1500/1580 Hz in one capture), the spot
store, `/api/wspr`, and the web panel's content.

**The one thing still missing for a real receiver: the RX slot loop.** Nothing
captures live audio on even minutes and feeds the decoder yet. Until it exists
the only spots in the store come from `{"action":"wspr_selftest"}`, and
`/api/wspr` reports `rx_live:false` so nothing can mistake them for real
traffic. The budget question that used to block this is now answered — 53.4 %
of a cycle leaves room for the capture.

**Not built: the Tab5 screen.** The design is written
(`docs/wspr-ui-design.md`) and the data layer it needs exists. The integration
was deliberately not attempted overnight: `ui_set_base_mode()` is a binary
`if (FT8) … else …` with FT8 snapshot and task lifecycle woven through it, and
adding a third mode means restructuring it on a 10,000-line file that drives
the operator's daily-driver panadapter. That is a change to make with someone
awake, not at 4 am. The CW branch already solved the same problem
(`UI_MODE_CW = 2`) and is the pattern to follow; **take `UI_MODE_WSPR = 3`** so
the two branches do not collide.

**Not verified: how the web panel LOOKS.** Its content was read back out of the
DOM, but the browser pane would not composite a screenshot, so spacing and
alignment against the decode table still need eyes.

## Things worth not rediscovering

- **A dry-run WSPR TX needs no radio now.** `wspr_tx_arm()` used to refuse
  without a CAT link, which made the engine untestable exactly when this bench
  is cheapest — the QMX wedges on **every** reflash (#74, confirmed
  deterministic by the operator: *"it will never survive on its own"*). A live
  build keeps the check.
- **`double` on this chip is a software library.** The P4's FPU is
  single-precision; the self-test's synthesis took **80 s** in double and 2.8 s
  in float. Worth knowing before writing any new DSP for this target.
- **One boot had HTTP dead while ICMP answered** — TCP 80 refusing for 200+ s,
  nothing logged, cleared by a reset. Not caused by that session's changes (the
  same build worked after reboot). Unexplained; noted rather than explained
  away.
- **`main.bat`-style CRLF churn**: `tools/QMX-Panadapter flasher/flash.bat`
  shows as permanently modified in this worktree. It is a line-ending artifact
  only — `git diff --ignore-cr-at-eol` is empty. Ignore it.
