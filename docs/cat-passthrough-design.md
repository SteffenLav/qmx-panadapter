# CAT pass-through design (T63)

Asked by Ralph DL1HR, seconded by Gyula HA3HZ 2026-10-22: let a PC logger
(N1MM+, flrig) reach the QMX through the Tab5.

Desk assessment, 2026-10-22. Nothing here has been built or flashed.

---

## 0. The use case — not stated by the requesters, but determinable

⚠ **Neither Ralph nor Gyula described a use case.** What we have is one request
and one second. "Over the same USB-C that powers it" has been in T63's row since
the day it was written, but it is **not marked as a quote** the way Gyula's
sentence is, so treat it as our paraphrase and not as his words. Nothing else
survives in this tree.

So it has to be settled by elimination. Two scenarios are possible:

**A. The Tab5 is the operating position; the PC only logs.** The operator
watches the spectrum, clicks spots and tunes on the glass. N1MM+ on the PC
follows frequency and mode so each QSO is logged on the right band, and in a
contest also sets frequency.

**B. The Tab5 is a protocol adapter.** The PC wants the radio; the Tab5 is
merely in the path, and its screen is irrelevant.

⭐ **B is eliminated, and by a fact rather than a preference: the QMX has its own
USB CDC-ACM CAT port.** Plug the radio straight into the PC and N1MM+ has CAT
directly — one cable, no Tab5, no network, no pass-through. Nobody writes to a
mailing list asking for a worse version of a direct cable.

**So the use case is A, and the reason the request exists at all is that the
panadapter has taken the radio's only USB port.** Ralph is not asking for a
bridge. He is asking to get his logger back without giving up the panadapter.
That is the sentence to design against.

### ⛔ Therefore the screen is ON and the firmware is fully running

The question was asked directly (operator, 2026-10-22): in the CAT-server case,
could the Tab5 just show a black screen? **No — and not for display reasons.**
The pass-through is a *second consumer* of a link the panadapter owns, and it
depends on the panadapter running:

- the 50 ms poll loop is what produces the cached state that makes reads free
  (§4 Tier A) and what gives the band guard a dial to judge;
- IQ mode (`Q9`) has to stay asserted or the radio streams non-IQ audio;
- the reply tap lives in `cat_rx_task()` (§4 Tier C);
- the hold and pause arbitration (§4 Tier B, `cat_poll_hold()`,
  `cat_user_pause_active()`) exists because FT8/WSPR bursts and the operator's
  own front-panel pause exist.

Blank the screen and none of that goes away — but the reason the Tab5 is in the
path does. ⚠ The display is, separately, the heaviest consumer on the box (the
`display_lock` priority-inversion history), so a screen-off mode is not
technically absurd; it is simply a different feature from this one, and the
headless-QMX+ operators are its audience, not Ralph.

### ⛔ THE GAP THIS OPENS, AND IT IS NOT SMALL

The request as recorded says **USB-C**. We are answering with **TCP over WiFi**.
In most setups that is better — no cable at all — but it is **not what was
asked**, and `main/wifi/wifi.c:1189` sets `WIFI_MODE_STA` and nothing else:
**the Tab5 has no SoftAP.**

So the pass-through needs a network that both the PC and the Tab5 join. N1MM+
is contest software, and a field or portable contest often has no
infrastructure. **In that case this design delivers nothing at all.** It would
be the first *radio-control* feature to require a network — the panadapter
itself does not.

Two ways out, and only one costs us anything:

1. ⭐ **The PC hosts the network.** Windows Mobile Hotspot, or a phone hotspot
   both join. **Zero firmware work**, and it is the answer to give Ralph today.
2. **SoftAP on the Tab5** (`WIFI_MODE_AP` / `APSTA`). Real work, and it lands in
   the WiFi stack that already carries the double `STA_START`, the roam-back and
   the DMA-pool-collapse-after-WiFi history. Not to be started casually.

⛔ **Ask Ralph whether his PC and the Tab5 are on a common network, and whether
he means to use this in the field.** That question decides whether TCP alone is
the whole feature or only half of it — and it matters far more than which radio
model he has selected.

## 1. What already exists

`main/net/rigctld_server.c` is a Hamlib NET-rigctl server on **tcp/4532**,
started from `wifi.c` once the network is up. It answers:

| Command | Served from |
| --- | --- |
| `\dump_state`, `\chk_vfo`, `\get_powerstat`, `\get_lock_mode`, `\get_vfo` | static text |
| `f` get_freq | `cat_get_frequency()` — cached |
| `F` set_freq | `cat_set_frequency()` |
| `m` get_mode | `ui_get_mode_str()` + `ui_get_passband_width_hz()` — cached |
| `M` set_mode | `cat_request_mode()` (deferred), `cat_set_passband_hz()` |
| `v`, `s`, `t` | static (`VFOA`, no split, not transmitting) |

So reads cost **no CAT round trip** — they come out of the state the 50 ms
poll loop already refreshes every ~150 ms. Up to four clients, each on its own
reapable PSRAM task with an 8 KB stack (nothing in it touches DMA, which is why
those stacks are not in internal RAM — #284).

Any Hamlib-based client therefore works today: WSJT-X, fldigi, JTDX, CQRLOG,
Log4OM, gpredict.

⚠ **T63's TODO row said "we do NOT do this today", which was wrong about the
transport.** What is actually missing is narrower:

- no `w` / `W` (send_cmd) — no way to pass arbitrary CAT text at all;
- no reply path for text that is passed through;
- no PTT (`T` / `t` is hard-wired to "not transmitting");
- no levels (`l` / `L`) — S-meter, power, SWR, AF/RF gain, keyer WPM are all
  cached in `cat.c` and none is reachable.

## 2. Transport: TCP, not USB-CDC

**USB-CDC is ruled out by the silicon and by the field update path, not by
effort.** From the IDF tables, not inferred:

- `soc/esp32p4/usb_dwc_periph.c`: controller 0 is the HS DWC on the UTMI PHY
  with no GPIO mapping; controller 1 is the FS DWC on
  `USB_PHY_INST_FSLS_INTERN_0`, `dp = 27`, `dm = 26`.
- `usb_host_install()` with the default `peripheral_map` takes index 0, i.e.
  UTMI. That is the USB-A port holding the QMX (CAT **and** UAC audio). It
  cannot also be a device.
- A device therefore has to be the FS OTG, which needs the internal FSLS PHY.
  `hal/usb_phy_types.h` makes the controller a **selector**:
  `USB_PHY_CTRL_OTG` *or* `USB_PHY_CTRL_SERIAL_JTAG`. One PHY, one pin pair,
  one owner.

GPIO 26/27 are the USB-C data pair. Switching them to OTG device mode costs, in
one step:

1. the ESP log console (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`) — so
   `bench capture` dies, and capture is the diagnostic record;
2. `esptool` over USB-C, which is the **published** update route for every user
   in the field (see `docs/version-history.md`, the v1.15.0 partition-table
   note and the flasher instructions).

Cost (2) ends it. The primary console is UART0, so logging could move to a
header, but the flashing path cannot.

Two smaller costs, recorded so they are not re-derived:

- `esp_tinyusb` would be a new managed component, i.e. a new release-time
  stale-patch surface — the shape of the `apply_cdc_acm_close_tolerant.ps1`
  failure at v1.16.0.
- CDC endpoint buffers want `MALLOC_CAP_DMA`, a closed fixed pool last measured
  at 1.9 KB free (#284).

**TCP costs nothing new.** `rigctld_server.c` already proves the pattern
(listener plus per-client reapable PSRAM task). A second listener is ~200 lines
and zero new dependencies. Flash is not the constraint either way: 5.32 MB app
in the 7.15 MB `ota_1` slot.

## 3. What the two named programs actually need

Looked up 2026-10-22, from the N1MM+ Configurer documentation and the flrig
threads. **The two differ, and the difference sets the phasing.**

### ⛔ IT IS A TRANSPARENT PIPE. IT IS NOT A RADIO EMULATOR.

Operator's call, 2026-10-22, and it is right: **this cannot be tailored to a
brand or a model, and it must not try.**

The reason is that **what appears on the socket is not ours to choose.** N1MM+'s
Configurer offers **TCP** in the Port column with an `IP Addr:Port` field — the
documentation calls it "used for radios that support radio control via TCP
(Ethernet)" and names "a TCP-to-serial adapter" as a case where it applies.
N1MM+ runs its **own driver for whatever model the user picked** and tunnels
those bytes to the socket. Pick a Kenwood, Kenwood CAT arrives. Pick an Icom,
binary CI-V arrives, not even `;`-terminated. It does **not** speak rigctld: a
community answer on the N1MM+ group says N1MM+ does not use Hamlib, and a Linux
tutorial that tried rigctld on 4532 gave up and bridged a virtual serial port
with `socat`, selecting model TS-2000 over TCP.

So the dialect on the wire is **a dropdown on the operator's PC**, and there are
dozens of entries in it. Guessing it is not a design.

**Carry bytes both ways and decide nothing.** Then the compatibility question
becomes "does the model the user selected overlap what the QMX answers" — which
is **exactly** the question they already face with a direct USB cable to the
radio. We neither add nor subtract compatibility. Three consequences, all good:

1. ⭐ **The promise is testable and worth making:** anything that works on a
   direct cable to the QMX works through the Tab5. Nothing else needs claiming.
2. ⭐ **The compatibility surface is the QMX's, not ours.** When QRP Labs add a
   command it works through this the day they ship it, with no change here, and
   no table in this tree to go stale.
3. ⭐ **It is not "the N1MM+ feature".** It is the radio's CAT port, over the
   network — so it also serves flrig (through a PC virtual COM port), Log4OM's
   native Kenwood driver, DXLab Commander, Omnirig, and anything not yet
   written. `rigctld` on 4532 stays as the convenience layer for Hamlib
   clients. Two sockets, two audiences, **neither model-specific**.

⚠ So "which radio model have you selected?" is **not a design input.** It is
only useful as a test case to reproduce, and the reply to Ralph should not ask
it as though the answer gates the work. It does not.

### flrig — not reachable from here at all

flrig has **no network-rig option**. It wants a real serial port on the machine
it runs on; its own networking is the other direction (an XML-RPC *server* on
port 12345 that other programs connect to). So flrig needs a virtual COM port
on the operator's PC, and no amount of work on the Tab5 changes that. Say so
plainly rather than implying a future release will cover it.

⚠ The 2026-10-22 draft reply claimed "flrig and N1MM+ can both talk over TCP".
Half right, and the wrong half. It was never sent.

### ⛔ RETRACTED: "answer `IF;` from cache"

An earlier version of this document (commit `9ed99c12`) proposed synthesising
the `IF;` reply from cached state, to spare the link a logger's poll rate.
**Transparency beats it, and the proposal is withdrawn.**

Synthesising `IF;` is emulation, and it buys a contention saving at three costs
this tree has already paid once:

- `IF` is a **fixed-format frame** carrying fields we do not track — RIT/XIT
  offset, memory channel, scan status and more. One wrong field breaks a logger
  in a way that looks random, and nothing on the bench would show it.
- It would answer for firmware whose `IF` we have not read. The QMX's CAT
  surface has already changed under us twice (six new commands at 1_04_003/004,
  the `GP` longitude sign at 1_04_005).
- ⛔ It invents a reply the radio never sent. That is the class the PA-voltage
  cache comment already rules on: **a label that can be stale is worse than one
  that admits it does not know.**

**What replaces it, and is general:** cache the radio's **own reply bytes**, per
verb, with a staleness bound of one poll interval. If the client asks `FA;` and
the poller's `FA` answer landed 40 ms ago, return **that answer, verbatim** —
the radio's bytes, not ours. No round trip, no synthesis, and the bound is
explicit (~150 ms, the poll rotation).

That is dialect-agnostic: it works for any verb we happen to poll, needs no
knowledge of any frame's internals, and degrades to a real round trip for
everything else — including `IF;`, which the QMX answers itself (it is in the
supported set, `docs/qmx-1_04-cat-comparison.md` §1.1).

⚠ `process_cat_message()` has no `IF` handler, recorded in the CAT comparison
doc as a risk for the `AI` auto-info modes. Carrying `IF;` through Tier C does
not need one: the tap (§4 Tier C item 5) matches on the verb and hands back the
radio's own bytes without parsing them.

### ⚠ What a transparent pipe lets a PC program do to us

Stated so it is not discovered in the field. A pipe that carries any byte
sequence carries these too:

- `Q9` — IQ mode off. The spectrum goes flat while USB audio keeps streaming at
  full rate.
- `MU` — discards session state, with the same effect (measured on 1_04_004 and
  recorded in the CAT comparison doc).
- `MM` menu writes — can change the radio's stored configuration.

⛔ The answer is **re-assert, not forbid.** Forbidding verbs is the emulator
trap again, and `cat_request_iq_reassert()` already exists for exactly this —
it is what the resume-from-pause and dead-stream paths use. A pass-through
session ends with the same re-assert, and the dead-stream watchdog covers the
rest. A PC program that reconfigures the operator's radio over their own CAT
port is doing something the operator asked it to do; a panadapter that stays
flat afterwards is our defect, not theirs.

## 4. Design: three tiers, one arbitration mechanism

### Tier A — cached reads (already built)

`f`, `m`, split, PTT state, and the levels listed in §1 answer from firmware
state the 50 ms poller refreshes every ~150 ms, which is faster than any logger
polls. Zero link contention.

Most logger traffic lives here and **must stay here**: extend the native verb
set (`l` / `L` for S-meter, power, SWR, AF/RF gain, keyer WPM — all already
cached) rather than letting clients reach them through raw text.

### Tier B — writes (already built)

Into the existing per-field pending slots (`s_pending_freq_hz`,
`s_pending_mode_digit`, `s_pending_ssb_bw`, …), drained by `poll_task()`, which
owns the pipe.

**That is the arbitration.** There is exactly one writer task and everything
else queues behind it. Do not add a second queue.

⚠ One trap to honour, from `cat.h`'s own warning: a parked write returns
`ESP_OK` and can land seconds later. A pass-through reports **accepted**, never
**applied** — the same reason rigctld's `M` handler already returns `RPRT 0`
for "understood", not "done".

### Tier C — arbitrary CAT with a reply (the only new mechanism)

New file `main/net/cat_passthrough.c` plus a small core in `cat.c`:

1. **One request in flight, globally.** A mutex held across the whole
   request/response, bounded at ~300 ms. Not per-client: the radio is one
   radio.
2. **Through the one door.** `cat_send_external_cmd()`, mandatory. The new file
   lives under `main/net/`, so `check_tx_guard.py`'s `EXTERNAL_INPUT_DIRS` rule
   refuses the build if it reaches `cat_send_raw_cmd()` directly. That rule is
   already written and already aimed at T63.
3. **Queue to the poll task; never write from the socket task.** The request
   goes in a slot, `poll_task()` sends it on its rotation, the socket task
   waits on a task notification. This keeps "the poll task owns the pipe" true,
   which is the invariant the `s_poll_paused` mechanism exists to protect.
4. **Refuse, never queue, when the link is held.** Check
   `cat_poll_held_by_other(0)` and `cat_user_pause_active()` first. A TX burst,
   a GPS tick sync, or the operator handing the radio to its own front panel
   each mean *no*. A logger retries; a command landing inside a QMX menu is the
   fault `cat_user_pause_set()` was written to prevent.
5. **Reply correlation by verb, through a tap — not a change to the parser.**
   Tap in `cat_rx_task()` *before* `process_cat_message()`: if a request is in
   flight and the message's first two characters match its verb, copy to the
   waiter and signal. Then fall through to `process_cat_message()` unchanged.

   ⛔ Two reasons the tap must be additive. First, `process_cat_message()` is a
   flat if-chain with no default branch — unknown replies are dropped, so there
   is no existing seam to reuse. Second, its gating (`s_pa_awaiting_reply` and
   its siblings) is what keeps a hand-sent read from poisoning our cache: the
   2026-08-29 fault was a burst labelling itself 15.0 V after a raw set.
   Widening that gate to serve the pass-through re-opens it.

   **Verb matching is required, not optional.** The 50 ms rotation means an
   `FA` / `MD` / `FW` reply will land between the write and the answer, so
   first-message-wins is wrong. Time out at ~300 ms. A bare `?;` inside the
   window is the radio's own rejection and is returned as such.
6. **Silent sets get a truthful contract.** Kenwood sets answer nothing. Return
   "sent" and say nothing about whether it took. Do not synthesise a
   confirmation.
7. **Rate-cap it.** ~1 request per 100 ms per client with a global cap, so a
   10 Hz raw poller cannot halve the poll rate. Tier C is the escape hatch;
   Tiers A and B are the road.

## 4a. CW — the one case where the network genuinely bites

Asked by the operator, 2026-10-22, pushing back on a draft that said a network
hop "costs milliseconds, nothing for PTT and frequency". **He is right, and the
sentence was imprecise in the way that matters.**

⛔ **The quantity is JITTER, not latency.** A constant delay is harmless for CW:
the whole transmission starts 20 ms late and no listener can tell. CW element
timing is made of **differences** between edge arrival times, so what breaks it
is the spread, not the mean. A dot at 25 WPM is 48 ms. Shared WiFi routinely
jitters tens of ms and occasionally hundreds — retries, power save, a beacon
interval, a roam — and none of that is bounded. **Element-by-element keying
over WiFi cannot be made reliable, and it is not a software problem to engineer
around.**

So CW splits in two, and only one half has the problem:

| | Who makes the timing | Over WiFi |
| --- | --- | --- |
| `KD1;` / `KD0;` per element | the PC | ⛔ dead on arrival — jitter is the dot length |
| `KY<text>;` buffered text | **the radio's own keyer** | ⭐ fine — one packet, jitter only shifts the start |

⭐ **`KY` is in the QMX's documented set** ("KY (both variants)",
`docs/qmx-1_04-cat-comparison.md` §1.3), so buffered CW crosses a transparent
pipe with no timing exposure at all. A **WinKeyer on a separate PC serial
port** never touches us either.

### ⛔⛔ RETRACTED 2026-10-22: "KY is what N1MM+ normally uses"

An earlier version of this section said `KY` is what a contest logger normally
uses for F-key messages. **A QMX group thread the operator supplied says
otherwise, and it is primary evidence from people running this exact radio.**

**Stan KC7XE** (a correspondent already known to this project — the split/VFO
report, and the `MD8` Tune confirmation of 2026-08-09) describes his working
N1MM+ ↔ QMX+ setup, and **Dan NM3A** confirms it on 1_04_010:

- N1MM+ opens **one** COM port — the QMX's first virtual serial port.
- CAT runs on that port's **data** lines (radio model **TS-480**).
- ⛔ **CW is keyed by toggling DTR on that same port, element by element.**
  N1MM+ does the on/off timing itself. The radio side is
  `CW → CW Keyer → Key from USB DTR`, pointed at that same virtual port, and in
  N1MM+'s Configurer the port has **CW/Other** ticked with **DTR (pin 4) = CW**.
- ⛔ **`KY` is deliberately not supported by N1MM+.** Stan, verbatim: *"for
  various reasons, N1MM+ authors have decided to not support such keyer
  commands"*. It can be forced with `{CAT1ASC KY ...}` macros, but he moved to
  DTR because it is better integrated.
- Dan adds that on 1_04_010 the `{CAT1ASC KS0;}` / `{CAT1ASC KS16;}` wrapper is
  unnecessary: USB-DTR keying always keys as straight key whatever the paddle
  mode says, which also frees the paddle speed from the macro's `KS`.

⚠ **Ralph was not in that thread.** This is Stan's and Dan's setup, not
evidence of Ralph's. It establishes what the popular QMX+N1MM+ method *is*, not
what Ralph uses.

### ⛔⛔ DTR CANNOT CROSS THIS DESIGN, AND THAT IS STRUCTURAL

| | Crosses a TCP pipe? | Timing made by | Verdict |
| --- | --- | --- | --- |
| CAT data (`FA`, `MD`, `KS`, `KY`…) | yes — it is bytes | — / radio's keyer | ⭐ fine |
| **DTR keying** | ⛔ **no — DTR is not data** | **the PC, per element** | ⛔ impossible, twice over |
| Paddle | n/a | operator | ⭐ untouched |
| WinKeyer | n/a (own port, keys by wire) | WinKeyer hardware | ⭐ untouched |

**DTR fails for two independent reasons, and either alone is fatal:**

1. **It is not in the stream.** DTR is a USB CDC control-line state
   (`SET_CONTROL_LINE_STATE`), not a byte. A TCP socket has no DTR, and N1MM+'s
   plain TCP option tunnels data only — it does not speak RFC 2217.
2. **It is element-by-element.** Even if some protocol carried it, this is
   exactly the case §4a rules out: a 48 ms dot against unbounded WiFi jitter.

⚠ **And it would fail halfway, which is worse than failing cleanly.** Stan's
macros interleave `{CAT1ASC KS…;}` with the keying on the **same port**. A pipe
that carries the CAT half and drops the DTR half would set the keyer speed on a
radio that nothing is keying.

⭐ **We do have the mechanism, for what it is worth:**
`cdc_acm_host_set_control_line_state(hdl, dtr, rts)` is in the CDC-ACM host
driver, so the Tab5 **can** assert DTR on the QMX. What is missing is a way for
the PC to ask for it with usable timing — and over WiFi there is none.

⛔ **The fundamental constraint underneath all of this: two USB hosts cannot
share one USB device.** The QMX presents several CDC interfaces (we already use
#0 for CAT and the function at #5 for the terminal), but the Tab5 enumerates the
whole device as host. The PC cannot reach *any* of those ports while the Tab5
holds the radio. There is no "split the ports" arrangement.

### What survives, and what to promise

- ⭐ **Paddle:** untouched, no computer in the path.
- ⭐ **WinKeyer:** untouched — own PC port, keys the radio by wire.
- ⭐ **`{CAT1ASC KY …}` macros:** pure CAT data, so they cross the pipe and the
  radio's own keyer makes the timing. Usable, but the operator must write their
  F-key macros that way, and it is a user workaround rather than a supported
  N1MM+ feature.
- ⛔ **USB-DTR keying — the method Stan recommends and Dan confirmed: NOT
  deliverable over a network, at any effort.**

⛔ **So the question for Ralph is now specific and answerable: how does he key
CW from N1MM+ today — USB DTR, a WinKeyer, or the paddle?** If he answers DTR,
TCP cannot serve him and he should be told so plainly rather than waiting for a
release that cannot help.

⚠ The only design that could serve a DTR user is USB-C CDC device mode on the
Tab5 (§2 — costs the log console) **plus** a hard-real-time DTR relay into
`cdc_acm_host_set_control_line_state()`, on a box where core 0 is already the
wall and LVGL has caused priority inversions before. Possible; expensive;
entirely unproven. Do not offer it.

### ⛔⛔ THIS QUESTION FOUND A LIVE KEYING HOLE (fixed 2026-10-22)

`cat_keying_classify()` knew `TX`, `KD`, `MD8` and `TA`. **It did not know
`KY`.** So `/api/cmd {"action":"cat_raw","cmd":"KY TEST;"}` passed the T64 door
and keyed the radio with **no band check at all** — T64's exact class, in a verb
nobody had listed. Fixed, with 5 new harness cases; 31 cases, 0 wrong.

⚠ **The first fix was wrong and the harness caught it.** Added as another
`else if`, `KYCQ CQ DE OZ1LAV;` classified as harmless: the scanner takes the
whole **letter run** as the verb, so the message's first word is eaten into it
("KYC") and nothing matches. Every other keying verb takes a **digit**
argument, which is why greedy letter-scanning was safe until a verb arrived
whose argument is free text. `KY` is now matched by prefix, ahead of the
scanner.

### ⛔ THE REMAINING WORK THIS EXPOSES, AND IT IS A BLOCKER FOR TIER C

**The classifier was written against the six keying sites *this firmware* uses.
A transparent pipe can send anything the radio understands.** `KY` was found by
a question, not by a checker, and `check_tx_guard.py` cannot find the next one:
it greps our source for literals, and a pass-through's strings arrive at
runtime.

The QMX's documented set (§1.3 plus the 1_04 additions) includes commands whose
function is not recorded anywhere in this tree — `QJ`, `TQ`, `SA`, `SS`, `C2`,
`ML`, `PL`, `LC`, `OM`, `Q0`–`Q8`, `RC`, `RD`, `RT`, `RU`, `TR`, `RR`, `PS`.
⛔ **Guessing which of those can emit is exactly the wrong move.** The CAT
manual has to be read against the classifier, verb by verb, and every verdict
written into the harness.

⛔ **Do not open a transparent pipe before that sweep is done.** Until then the
pipe is only as safe as a classifier that has already been caught missing one.

## 5. Keying — a decision, not code

A LAN socket with no authentication that can carry `TX;` is **T64 repeating in
a new file**. `cat_send_external_cmd()` makes it band-guarded; it does not make
it authorized, and there is no HTTP auth on this device today.

- A setting, **default off**: "allow a PC logger to transmit". Off means the
  logger tunes, reads and logs, while `T 1` and raw `TX;` / `KD` / `MD8` /
  non-zero `TA` are refused with a reason. That alone covers what Ralph and
  Gyula asked for — a logger following the radio — and for most users it is the
  whole feature.
- ⛔ Turning it on **adds an arming site**. `tools/check_unattended_tx.py` and
  `docs/unattended-tx-inventory.md` are updated in the same change (today 12
  sites, 6 unattended), and the release notes say so.
- ⛔ **Gate the SOCKET, not the verbs.** A transparent pipe (§3) cannot carry a
  verb whitelist without ceasing to be transparent, so the switch governs
  everything the classifier flags, for the whole session. That is simpler and
  safer than a list, and `cat_keying_cmds.h` already says the classifier is
  deliberately over-eager: "a false refusal on a developer escape hatch costs
  one log line; a false pass is the operator's licence." A transparent pipe is
  precisely the case that was written for.
- ⛔ **A dead-man release is mandatory and has no existing equivalent.** If the
  socket closes, or the client goes quiet, while the radio is keyed, send
  `TA0;RX;`. Nothing in the tree covers a TCP peer vanishing mid-transmission,
  and "never leave the radio transmitting" is a standing rule. No keying
  through this path until that is in and proven.

## 6. Relation to the jpwatters decision

`cat.c`'s T46 funnel comment records the call already made: the network
transport is **not** taken, because handing the burst schedule to a relay moves
transmit timing — and six unattended keying paths — onto software this tree
cannot audit.

This design does not contradict that. The PC **asks**; the Tab5 decides, holds
the burst schedule, and runs the guard. FT8/WSPR/JS8 timing stays where it is.

⚠ Say this explicitly in any reply to Ralph: "CAT pass-through" sounds like the
thing that was declined, and it is not.

## 7. Phasing and effort

⚠ **Phase 2 is the one Ralph needs, not Phase 1** (§3). Phase 1 widens what the
Hamlib clients can do; it does nothing for N1MM+.

| Piece | Effort | Notes |
| --- | --- | --- |
| **Phase 2: the transparent CAT socket** — bytes both ways, no dialect knowledge | Small | ⭐ **Start here.** Copies `rigctld_server.c`'s task pattern. This is what N1MM+'s Configurer TCP option talks to, with nothing installed on the PC — and it is the general tool (§3), not an N1MM+ feature |
| Verbatim reply cache, one poll interval, per verb | Small | The radio's own bytes, not a synthesised frame (§3). Cuts a logger's poll traffic to nothing without emulating anything |
| Tier C core: poll-task slot, RX tap, verb match, timeout | Moderate | The only genuinely new mechanism; shared by both transports. Everything Phase 2 does not serve from cache falls through here |
| Dead-man `TA0;RX;` on socket close | Small | Mandatory before any keying |
| Phase 1: extend rigctld — `l`/`L` levels, `w`/`W`, `T`/`t` behind the switch | Small | All the state is already cached in `cat.c`. Benefits WSJT-X/fldigi/Log4OM, not N1MM+ |
| flrig | — | Not reachable from the Tab5 (§3). Needs a virtual COM port on the operator's PC |
| USB-CDC device | — | Not taken. Breaks the field flashing path (§2) |

Nothing here needs the radio. The Tier C core and the dead-man release are both
exercisable on the host shim and against `/api/cmd` from a browser, with no
flash and no power cycle.
