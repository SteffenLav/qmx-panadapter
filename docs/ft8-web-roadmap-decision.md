# FT8 web access — the decision Randy N4OPI is waiting on (#195)

## ✅ DECIDED 2026-08-19 (operator): NO SPECTRUM/WATERFALL ON THE WEB IN FT8 MODE

> "Randy cannot have the spec/WF on the web due to lack of resources in Tab5 —
> that will be the answer to him — a decision taken long time ago."

So the answer to Randy is **(c)/(a) — no mirror is coming, send the list.** This is
a long-standing decision, not a new one, and it is a *hardware* limit rather than a
missing feature: the FT8 decode pipeline saturates core 0, which is exactly why the
Tier-1 render gate turns the spectrum stream off in FT8 mode. Mirroring the screen
would cost decode yield, which is the thing the device exists to do well.

**Consequences for the reply:** tell him plainly that the mirror is not planned and
why, so he stops waiting — and ask him to send the list, because the four control
gaps in §1 below are exactly the kind of thing that CAN be added. Do not leave the
impression it is merely deferred.

The rest of this file is the material the decision was made from; kept because the
same question will be asked again.

---

**Original status: needs the operator. Nothing here is a recommendation I can make
for you — it is the material to make it with.**

Randy, 2026-08-19:

> What are the long term plans for the FT8 web access? I have quite a list of
> requests, but if you're planning to eventually mirror the screen on the Tab5,
> I'll wait till that comes out.

He is deliberately **holding back a list of feature requests** until he knows.
Answering costs nothing; not answering silently loses the list. That is the whole
reason this is worth ten minutes.

---

## 1. Where the web UI actually is today

Checked against the code on 2026-08-19, by diffing **surfaces**, not commit titles
(the standing guidance in `memory/project_web_tab5_parity.md`). It is **further
along than it looks** — worth knowing before answering, because "we plan to add
X" is embarrassing when X shipped already.

**The browser can already:**

| Capability | How |
|---|---|
| See the decode list, with distance + bearing | `/api/status` `ft8` block |
| **Work a station by clicking its row** | `POST /api/cmd {"action":"reply","call":…}` — a confirm dialog first |
| **Work a station from the pileup** | same action, `data-pcall` rows |
| Start a CQ run | `{"action":"cq_start"}` |
| Choose the TX audio tone | `tone-btn` |
| See TX status live | `ft8-tx` banner + tab-title dot |
| See and clear the grey-list | `{"action":"greylist_clear"}` |
| Switch the Tab5 between FT8 and Panadapter | `{"action":"set_screen"}` |
| Download logs/config, upload QSOs to QRZ/eQSL/LoTW | Files menu |

The `reply` handler is even commented *"Phase 6 of web parity, TX explicitly
blessed by the operator"* — so remote transmit was a deliberate decision already
taken, not an accident.

**The browser still cannot:**

| Gap | Tab5 equivalent |
|---|---|
| **See spectrum/waterfall while the Tab5 is in FT8 mode** | the page says so explicitly |
| Turn auto-answer on/off, set priority, edit include/exclude filters, Field Day, Skip-TX1, worked-before, CQ stop-after | `ft8_filter_modal.c` |
| Edit the three CQ presets | `ft8_cq_modal.c` |
| Re-send / RR73 / 73 override buttons mid-QSO | left pane |
| Sync the clock | `ft8_time_modal.c` |

**That first gap is probably what Randy means by "mirror the screen."** It is also
the one with a real reason behind it: the WS spectrum stream is gated off in FT8
mode because core 0 is saturated by the decode pipeline (the Tier-1 render gate).
It is not an oversight to be tidied up — undoing it costs FT8 decode yield, which
is the thing the device exists to do well.

---

## 2. The three coherent answers

**(a) The web stays a COMPANION, not a mirror.**
It does the things a laptop is better at — logs, uploads, config, watching the
band, working a station you can see. The Tab5 keeps the dense operating controls.
*Tell Randy: send the list.* Most of what he wants is probably small and additive.

**(b) Full mirror of the Tab5 screen.**
Honest cost: the FT8-mode spectrum gate exists to protect decode yield, so a true
mirror needs either the core-0 rebalance (the long-standing Phase 6.3 portrait
rewrite, a big job) or accepting fewer decodes while a browser watches. Every
future Tab5 feature then needs a web twin, permanently doubling UI work.
*Tell Randy: wait.* And be sure, because he will.

**(c) Parity for a NAMED subset, no mirror.**
Close the four control gaps above (filters, CQ presets, overrides, time sync) and
state that spectrum-in-FT8 is deliberately not coming. Randy gets a real answer
and his list stays useful.

---

## 3. The test to apply

From the same memory note: **"would this be needed at a POTA site with no
laptop?"** — that decides what must be on the *Tab5*. The inverse decides the web:
*would you want this from the sofa, or from another room, while the radio runs?*

Filters and auto-answer pass that test easily — they are exactly what you want to
change without walking to the rig. Spectrum-in-FT8 does not: if you are watching
the band you can switch to Panadapter mode, which the browser can already do
remotely with one click.

That asymmetry is why **(c) looks like the strongest answer** — but it is a
product call about what this thing is for, and it is yours.

---

## 4. What to do with it

Pick (a), (b) or (c) and I will draft the reply. If (c), say whether the four gaps
are the right four — that list came from reading the modals, not from Randy, and
he may want something not on it.
