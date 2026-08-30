# A still spectrum, with the VFO moving over it

Design analysis for changing the panadapter from **dial-locked** (VFO pinned at
screen centre, spectrum slides underneath) to **spectrum-locked** (spectrum and
waterfall hold still, the VFO cursor moves across them) — the FlexRadio /
SmartSDR model the operator asked for on 2026-08-30.

Status: **analysis only. Nothing implemented.** The numbers below are arithmetic
and code reading, not measurements on air.

---

## 1. The one hard constraint, and it decides the whole feature

The QMX's LO is rigidly `dial - 12 kHz`, and it moves with the dial. So the
spectrum we can see is always

```
capture window = [ dial - 36 kHz , dial + 12 kHz ]      width 48 kHz
```

A "still" display means holding a **fixed absolute** range — call it the
**viewport** — while the dial moves. But the capture window moves 1:1 with the
dial, so the viewport can only stay still while it remains *inside* the capture
window.

Let the viewport be `[V_lo, V_hi]`, width `W = 48/Z` kHz at zoom `Z`. It stays
valid while

```
V_lo >= dial - 36        and        V_hi <= dial + 12
```

which rearranges to a legal dial range of length

```
play = 48 - W  kHz
```

**That is how far you can tune before the display is FORCED to move.** It is not
a policy choice; it is the hardware.

### 1.1 Where the VFO cursor is allowed to be

Requiring the cursor to also be *visible* (`V_lo <= dial <= V_hi`) tightens it to

```
dial in [ max(V_lo, V_hi - 12) , min(V_hi, V_lo + 36) ]
```

| Zoom | Viewport W | play | Where the VFO can sit in the view |
|---|---|---|---|
| x1  | 48 kHz | **0** | pinned at 75% across. **Stillness impossible.** |
| x2  | 24 kHz | 24 kHz | **top half only** (top 12 kHz) |
| x3  | 16 kHz | 32 kHz | top 12 kHz of 16 |
| **x4** | **12 kHz** | **36 kHz** | **anywhere — full freedom** |
| x8  | 6 kHz | 42 kHz | anywhere |

**The headline: full Flex-like behaviour needs zoom >= x4.** Below that the
cursor is confined to the top 12 kHz of the view, because the capture reaches
only 12 kHz above the dial. At x1 it cannot work at all — the viewport *is* the
capture window, so there is zero play.

This is not a defect in the plan; it is why a Flex can do this and we can only
partly. A Flex digitises megahertz and displays a slice. We digitise 48 kHz and
display all of it.

### 1.2 Consequence to state plainly in the UI

At x1 the feature is unavailable. Either make x2 the practical default for
"still" operation, or show how much play remains. **Do not let the operator
believe the display is still and then have it jump** — that is worse than it
moving predictably.

---

## 2. Why this is a smaller change than it looks

Above x1 the code **already** maintains a pan offset (`s_pan_offset_bins`) and
already draws an arbitrary window. What makes the display dial-locked is one
thing: `recompute_zoom_pan()` re-derives the pan from the passband centre on
**every** tune, and `ui_update_frequency()` calls it.

> The still-spectrum feature is, at its core, **deleting that automatic
> re-centre and clamping instead.**

Everything else is making the many places that assume "dial == screen centre"
ask one shared mapping instead. That work is **already scoped as #297** and is a
prerequisite either way.

---

## 3. Architecture

Replace "centre on the dial" with an explicit viewport as the single source of
truth:

```c
static int64_t s_view_lo_hz, s_view_hi_hz;   /* absolute Hz */
```

Per frame:

1. `cap_lo = dial - 36 kHz - cw`, `cap_hi = dial + 12 kHz - cw`, taking `cw`
   from `ui_get_if_offset_hz()` so CW pitch and `if_cal_hz` are included once,
   in the one place that already owns that sum.
2. **Clamp** the viewport into `[cap_lo, cap_hi]`, shifting it minimally.
3. Column -> absolute Hz -> baseband Hz -> bin, **with no modulo wrap**. A column
   outside the capture window draws as "no data" rather than wrapping. That guard
   *is* the #297 fix.
4. Draw the VFO cursor at the column for `dial`. If it is off-view, draw an edge
   arrow rather than clamping the cursor to the edge — a cursor parked on the
   edge reads as "tuned here", which would be a lie.

**Precompute the column->bin map as an int LUT** on viewport/zoom/dial change
rather than per column per frame. Today the inner loop does a float multiply and
divide per column, 1280 times a frame, on the core with no headroom — so this is
a performance *win*, not a cost.

---

## 4. Re-framing policy — when may the display move?

Two distinct triggers, and conflating them is a trap:

- **Forced (hardware):** the viewport would leave the capture window. Not
  optional. Fires continuously once the viewport is hard against an edge and you
  keep tuning that way.
- **Courtesy (policy):** the VFO nears a viewport edge and we re-frame so the
  operator can keep working.

**Use hysteresis, and re-frame on the courtesy trigger BEFORE the forced one can
bite.** Suggested: re-frame when the VFO comes within 15% of an edge, and
re-frame by a **half view** (page), not by a pixel. Otherwise the forced clamp
fires on every tune while at an edge and the display micro-jitters — the "still"
promise broken in the least readable way possible.

---

## 5. The waterfall — the hardest part

In a still display the waterfall becomes **frequency-aligned history**: a
signal's past sits directly above its present. That is the real prize here,
bigger than the aesthetics.

It also means a viewport move **invalidates history**, and that must be handled
honestly.

| Move | Behaviour |
|---|---|
| none | nothing to do (the common case, and the win) |
| small, whole pixels | **shift the image horizontally**, fill the exposed strip with the no-data background |
| large, or a zoom change | **clear** — old rows are at a different scale and cannot be rescaled without lying |

**Cost warning.** The canvas is 1280 x 824 RGB565 (~2.1 MB). A horizontal
memmove of that is roughly 20 ms on PSRAM — on a core already measured at 0-7%
idle in panadapter mode. Doing it per tune would be visible.

**Recommended: borrow the trick the vertical scroll already uses.** Make the
canvas *wider* than the screen (say 1280 + 2x256 px of margin) and move a
horizontal view offset for small shifts, doing a real memmove only when the
margin runs out. Memory goes ~2.1 MB -> ~2.9 MB in PSRAM, which has ~15 MB free.
This mirrors an existing, proven pattern in this codebase.

**v1 fallback if that proves fiddly:** clear on re-frame. Honest and cheap, and
tolerable *only* if the courtesy policy keeps re-frames rare. It is not tolerable
alongside a per-tune forced clamp — see section 4.

---

## 6. Zoom and pinch

- **Pinch zooms about the pinch centroid** in absolute Hz: map the centroid to a
  frequency and keep that frequency under the fingers. Map-like, and the only
  behaviour that feels still.
- After zooming, clamp the viewport. Zooming out at an edge will shove the
  viewport — unavoidable.
- **Zoom-out is limited to 48 kHz** because there is no more data. x1 stays "the
  whole capture window".
- **A zoom change clears the waterfall** (scale change). Say so, or accept the
  flush.
- `dsp_set_zoom()` currently mixes the **VFO+pan** to DC for the zoom FFT. In the
  viewport model it must mix the **viewport centre** to DC instead. This is a
  real change in `dsp.c`, not just UI.

---

## 7. Gestures — the UX decision the operator must make

A still display wants the classic SDR split:

- **Tap = tune** (place the VFO). Unchanged in spirit.
- **Drag = pan the viewport**, *without* retuning.

**This changes today's behaviour**, where a one-finger horizontal drag is a
"stroll" that retunes on release. That gesture is documented and liked. Options:
keep drag=stroll and pan by some new gesture; or switch to drag=pan and accept
the muscle-memory cost. **The operator's call, not mine.**

Panning is bounded by the capture window, so pan range = `play`. At x1 there is
no pan.

---

## 8. Pitfalls, in the order they will bite

1. **x1 cannot be still.** Design the UI around it; do not hide it.
2. **The forced clamp fires per tune at an edge** and will make a "still" display
   shimmer. Hysteresis is mandatory, not polish.
3. **The VFO is confined to the top 12 kHz of the view below x4.** Operators will
   read this as a bug unless the geometry is explained.
4. **Waterfall memmove on a pegged core 0.** Use the margin trick, or keep
   re-frames rare.
5. **Two mappings drifting apart.** Axis, cursor, passband, RIT marker, spots
   lane, band-plan strip and tap-to-tune must all call ONE helper. This codebase
   has already been bitten by exactly that (the v1.8.1 tap-to-tune bug).
6. **The sticky panadapter/FT8 snapshot** stores zoom/pan and must learn the
   viewport, or switching modes silently re-frames.
7. **Web parity.** `webserver_ws.c` sends the browser bins already wrapped and
   dial-centred. Either send raw fftshifted bins plus the capture window's
   absolute bounds and let the browser window them identically, or the two
   screens will disagree. The former also fixes #297 on the web.
8. **Per-column noise-floor state** (waterfall floor arrays) is meaningful only
   while columns keep meaning the same frequency; reset or re-index it on a
   viewport move. Per-**bin** state (baseband) is unaffected.
9. **Tuning is CAT-rate-limited** (one write / 200 ms). Dragging a cursor across
   a still spectrum must commit on release, not stream writes.

---

## 9. What this does NOT affect

Stated so nobody changes it by association:

- `dsp_get_peak_dbm_around_vfo(vfo_bin, ...)` — the S-meter and `signal_dbm`
  locate the VFO **bin**, which does not move with the display.
- **All FT8/FT4/WSPR decoding** — those consume audio, never the display mapping.
  Decode has never been affected by any of this, which is why the #297 wrap
  survived unnoticed for so long.
- The FT8 and WSPR screens, which do not draw the panadapter.

---

## 10. Staging — each step shippable and separately verifiable

| Phase | Content | Verified by |
|---|---|---|
| **0** | **#297**: one authoritative `x <-> Hz` helper, and clamp the window. Fixes the wrap and the lying axis. Prerequisite either way. | FT8 signals below 14.074 no longer appear in the right-hand quarter |
| **1** | Introduce the viewport as state, policy still "always centred on dial". **Behaviour identical** — pure refactor. | screenshot diff against v1.10.2 |
| **2** | Stop calling `recompute_zoom_pan()` on tune; add hysteresis re-framing. Waterfall clears on re-frame. | tune at x4 and watch the spectrum hold |
| **3** | Waterfall horizontal margin + shift, so small re-frames keep history. | history stays aligned across a re-frame |
| **4** | Gestures: drag pans, pinch zooms about the centroid. | on the bench |
| **5** | Web parity: raw bins + capture bounds over WS. | both screens agree on one signal |

Phases 0 and 1 are worth doing regardless of whether 2-5 are ever built.

---

## 11. Verification needs an antenna

Everything above is arithmetic and code reading. The falsifying tests all need
real signals:

- **The x1 wrap (#297):** on 20 m with FT8 active, tune below 14.074 and watch
  the cluster appear in the right-hand quarter once the dial is more than 12 kHz
  below it.
- **The play figure:** at x4, tune across 36 kHz and confirm the display holds.
- **The x2 confinement:** confirm the cursor cannot enter the lower half.
