# Carry forward to `feat/cw-page`

Recorded 2026-09-07. Nothing here is on `main`, deliberately.

## #351 - CW transmit, two paths at TWO speeds (Michael KZ4LY, #178669)

He is a contester describing what he actually does, unprompted:

> "I have both keyboard and key. I send their call and the exchange by
> keyboard, but I send fills by key. My preference is that the keyer is set
> slower than keyboard sending. So if I am running 20WPM on the keyboard, the
> keyer might be set to 18WPM, so that fills come a little slower to help
> comprehension."

**The two speeds are a REQUIREMENT, not a preference to average away.** One WPM
setting is wrong for both jobs.

**The split is by each path's real constraint, and it is the design:**

- **Keyboard / memory text -> `KY`, and the RADIO does the timing.** USB latency
  stops mattering, which is the same reason the panadapter can key FT8 over CAT
  at all. This half stands alone and carries most of the value.
- **Paddle -> wired to the radio, NOT through us.** His words: "Key-sending
  across a network connection is latency-sensitive, and it does not take much
  network jitter to make your fist sound terrible. I would expect this to only
  be worse with a resource-constrained unit like the tab5." Take that as a
  reason rather than pessimism. His own answer bypasses us: DECW
  (github.com/tompatulpan/duration-encoded-cw-protocol) from a vail adapter
  (vailadapter.com/devices) into the QMX's own "Key from USB DTR".

⚠ `KY` is what this branch was already written against and is **still
unverified** - first test needs a DUMMY LOAD.

## #352 - the CW decode pane extras (Samuel W7STF, #178667/#178647)

The cursor half SHIPPED on main in v1.12.0 (two lines, wrapping down then
overwriting, the overwrite in a second colour). What was deliberately NOT built
for the panadapter and belongs here instead:

- timestamps, on a significant tuning jump or periodically
- a resizable pane, taking its height from the waterfall
- tap-to-save the decode window to a timestamped file

Reason: a decode window is the POINT of the CW page, where there is room for it,
rather than a strip under a spectrum.

⛔ **Keep the ONE line model in `cw_decode.c`.** Both screens draw from it and
the browser is sent the line ALREADY RENDERED, precisely so the two cannot
drift. A multi-line pane must extend that model, never add a second one.

⛔ **Not to be said publicly yet** - the CW page is unannounced, so a reply
promising these "later" would commit to a screen nobody has been told about.
Answer users on what shipped.
