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

⭐ **DECIDED 2026-08-30, after driving both in the simulator: JUMP A WINDOW, not
push.** The operator's words: *"i like the jumping better"*. Paging holds
completely still and then re-frames once, rather than pinning the cursor near the
edge and sliding the spectrum under it for as long as tuning continues. Land the
cursor far enough from the opposite trigger that reversing direction does not
immediately page back: triggers at 5% / 95% landing at 75% / 25% proved stable,
8% / 92% landing at 82% / 18% did not.

### 4.2 The shipping behaviour: DEAD BAND -> SOFT PUSH -> PAGE WITH OVERLAP

Operator's design, 2026-08-30, after driving the simulator. A bare page is too
abrupt for the case that matters most - working a signal that happens to sit near
the edge of the screen. Three stages:

1. **Dead band.** Cursor inside the middle ~80% of the view: nothing moves.
2. **Soft push.** Past `EDGE` (0.90 of the view) the view nudges along just far
   enough to keep the cursor in sight. *"so that it will not jump when you just
   wanted to tune a signal on the edge or close to it"*. Bounded: it will give up
   to `PUSHMAX` of a view width of ground, accumulated.
3. **Page.** Keep tuning outward past `PUSHMAX` and it turns the page, landing
   the cursor at `OVERLAP` from the far side so a slice of the previous screen is
   still visible - *"a little overlap of the previous screen so you are not
   disoriented and can see where you came from"*. Show the carried-over slice for
   a second or so with the seam drawn.

Starting values, exposed as live sliders in the simulator so they can be tuned by
feel rather than guessed: `EDGE = 0.90`, `PUSHMAX = 0.10`, `OVERLAP = 0.28`.
At x4 that gives roughly **8.6 kHz still, 1.2 kHz of nudge, then a page**.

⚠ **The page target must be clamped into the cursor's reachable range**
(`[lMost, rMost]` from 4.1) before it is applied. Otherwise the capture clamp
overrides it and a deliberate page reports itself as a *forced* move - which is
what happens at x2 tuning up, where the 28% landing spot is under the 50% floor.
Found by running the state machine over a tuning sweep, not by reading it.

### 4.1 Trigger the page on the CURSOR'S OWN LIMIT, not a fixed fraction

⛔ **I got this wrong first, and the operator was right to push back.** I claimed
paging downward was impossible below x4 and blamed the hardware. It is not: the
viewport can slide anywhere inside the 48 kHz, exactly as the band-plan slider
already pans the view today. At x2 a 24 kHz window has 24 kHz of room.

What actually blocked it was **my trigger**. I paged when the cursor reached 5%
of the view; but the ceiling stops the cursor short of the left edge, so below x4
that threshold is unreachable and the clamp dragged the view down continuously
instead. A policy fault dressed up as a physical limit.

**The cursor's reachable range, as a fraction of the view:**

```
cursor in [ max(0, (W-12)/W) , min(1, 36/W) ]
```

| Zoom | W | cursor may sit | page down works | still travel between pages |
|---|---|---|---|---|
| x1 | 48 kHz | pinned at 75% | **no** (zero play) | — |
| x2 | 24 kHz | 50-100% | yes | **10.8 kHz** |
| x3 | 16 kHz | 25-100% | yes | **11.2 kHz** |
| x4 | 12 kHz | 0-100% | yes | **11.4 kHz** |
| x8 | 6 kHz | 0-100% | yes | 5.7 kHz |

**So the rule is: page when the cursor reaches the limit of where it can GO**, not
a fixed screen fraction — and land it at the far side (95%), letting the clamp
trim that where the ceiling forbids it. Paging then works in both directions at
every zoom except x1, with roughly the same still-travel at x2, x3 and x4, and
the cursor never leaves the screen.

x1 remains the sole exception, and for the reason in section 1: the view already
*is* the whole capture window, so there is nothing to page into.

**The cursor being confined to the right of the view is still true** and still
worth drawing — it is why a page lands at the right and walks left, rather than
landing centre. It just never prevented paging.

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

- **The `dial + 12 kHz` ceiling — CONFIRMED 2026-08-30.** The operator tuned
  about 12 kHz below 14.074 at x1 and the FT8 signals simply **disappeared**.
  That is the ceiling behaving exactly as section 1 predicts: at dial 14.062 the
  capture top *is* 14.074, so the sub-band sits at and above the edge and is
  genuinely absent from the data. First on-air confirmation of the capture
  window on our own hardware, and the foundation the rest of this document
  rests on.

- **The x1 wrap (#297) — still untested, and my first stated test was wrong.**
  Tuning *below* a signal cannot show the wrap; it walks the signal off the
  ceiling instead. A signal only reaches the wrapped quarter when it lies in
  `dial-36k .. dial-24k`, i.e. when you tune **above** it by 24-36 kHz.
  **Correct test: sit at about 14.100 MHz and the 14.074 FT8 cluster should
  appear in the right-hand quarter, where the axis claims ~14.112-14.124.**
- **The play figure:** at x4, tune across 36 kHz and confirm the display holds.
- **The x2 confinement:** confirm the cursor cannot enter the lower half.
