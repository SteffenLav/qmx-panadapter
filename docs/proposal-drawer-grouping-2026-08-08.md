# Drawer grouping — proposals for the operator, 2026-08-08

The panadapter drawer now carries **25 sections**, built in this order:

Flip 180 · QMX volume · QMX RF gain · Release radio · Display sleep ·
Battery care · Brightness · Antenna Tune (1.04+) · WiFi setup · Callsign & Grid ·
Band-plan region · IQ balance · Flat spectrum · Spots · dB presets · dB range ·
Smoothing · CW (pitch + TX offset) · IF calibration · Colour map · Waterfall ·
Distance units (FT8) · FT8 sync lines · Simulation mode (FT8) · Resource monitor (dev)

Two headers sit above them: **User Manual** and **Need guidance?**

The order is historical — each new setting landed wherever it fitted at the
time — so related things are scattered (the two QMX gain controls are adjacent,
but CW pitch is nine sections away from CW TX offset, and Brightness is three
away from Colour map).

Three options below. They are not exclusive: **B is the structure and A is the
length control**, and doing both is coherent. C is the operator's own hint.

---

## A. Basic / Expert switch at the top

A two-position segmented control pinned above the first section. **Basic** shows
the everyday set; **Expert** reveals all of it. Choice persists.

**Basic (9):** Callsign & Grid · WiFi setup · QMX volume · QMX RF gain ·
Release radio · Brightness · Spots · Display sleep · (FT8: Distance units,
Simulation mode)

**Expert adds (16):** Flip 180 · Battery care · Antenna Tune · Band-plan region ·
IQ balance · Flat spectrum · dB presets · dB range · Smoothing · CW ·
IF calibration · Colour map · Waterfall · FT8 sync lines · Resource monitor

- **For:** biggest relief for the smallest change; nothing moves, so muscle
  memory survives; a new operator is not shown IF calibration on day one.
- **Against:** "where did it go?" if someone forgets they are in Basic. Mitigated
  by making the switch the first thing in the drawer, always visible.
- **Effort:** small. One control, one persisted flag, one visibility pass —
  the machinery already exists (`drawer_set_ft8_mode` hides and restacks
  sections exactly like this today).

## B. Named groups, in a sensible order

Keep one list, but give it **group headers** and reorder underneath them. No new
interaction; purely scanability.

1. **Station** — Callsign & Grid · Band-plan region
2. **Radio** — QMX volume · QMX RF gain · CW (pitch + TX offset) ·
   Antenna Tune · Release radio
3. **Network** — WiFi setup · Spots
4. **Display** — Brightness · Display sleep · Flip 180 · Colour map
5. **Spectrum** — dB presets · dB range · Smoothing · Waterfall · Flat spectrum ·
   IQ balance · IF calibration
6. **FT8** — Distance units · Simulation mode · FT8 sync lines
7. **Device** — Battery care · Resource monitor (dev)

- **For:** the list finally reads as something designed; "Radio" collects every
  control that reaches the QMX, which is what an operator is usually after.
- **Against:** the drawer is exactly as long as it is now. Headers alone do not
  shorten anything.
- **Effort:** small-medium, mostly moving existing blocks. The FT8-mode keep[]
  list has to move with them.
- **Optional extra:** make the groups **collapsible**, one open at a time. That
  does shorten it, at the cost of a tap to reach anything.

## C. A second drawer page (the operator's hint)

One more swipe from the right edge opens page 2, "Advanced". Page 1 keeps the
Basic set from A; page 2 takes the rest.

- **For:** no vertical scrolling on either page; the everyday page becomes short
  enough to scan without moving.
- **Against:** a new gesture to discover, on an edge that already carries the
  drawer itself; and there is no visual promise that a second page exists.
  Every other page in this app is reachable without knowing a secret.
- **Effort:** medium. New pane, new gesture handling, a page indicator so it is
  discoverable at all.

---

## Recommendation

**B, then A.** Group and reorder first — that is the part that makes the drawer
*intuitive*, and it is worth doing whether or not anything is ever hidden. Then
add the Basic/Expert switch on top, which is what makes it *short*. Together
they need no new gestures and nothing becomes unreachable.

C is the more dramatic change and I would hold it: the drawer would still need
grouping inside each page, so it does not remove the work in B — it adds to it.
If page 1 ends up short after A, the reason for C mostly goes away.

**Open question for the operator:** should **Release radio** and **Antenna Tune**
be in the drawer at all? Both are actions rather than settings, both are things
you reach for with the radio in front of you, and both currently sit among
sliders. A small action row at the top of the drawer (or on the main screen)
might suit them better than a section each.
