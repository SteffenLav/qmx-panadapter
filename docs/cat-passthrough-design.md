# CAT pass-through design (T63)

Asked by Ralph DL1HR, seconded by Gyula HA3HZ 2026-10-22: let a PC logger
(N1MM+, flrig) reach the QMX through the Tab5.

Desk assessment, 2026-10-22. Nothing here has been built or flashed.

---

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

### N1MM+ — a raw Kenwood-CAT socket, no PC software

The Configurer's Hardware tab offers **TCP** in the Port column, with an
`IP Addr:Port` field. The documentation describes it as "used for radios that
support radio control via TCP (Ethernet)" and names "a TCP-to-serial adapter"
as a case where it applies — i.e. N1MM+ puts the **selected radio model's own
CAT bytes** on the socket, exactly as it would on a COM port. It does **not**
speak rigctld: a community answer on the N1MM+ group says N1MM+ does not use
Hamlib, and a Linux tutorial that tried rigctld on 4532 gave up and bridged a
virtual serial port with `socat` instead, selecting model TS-2000 over TCP.

So **N1MM+ needs §5 Phase 2, not Phase 1**, and with Phase 2 it needs nothing
installed on the PC. Phase 1 does not help it at all.

⚠ Open, and the one thing the repo cannot answer: **which radio model Ralph has
selected in N1MM+.** That decides which verbs to answer first. Ask him.

### flrig — not reachable from here at all

flrig has **no network-rig option**. It wants a real serial port on the machine
it runs on; its own networking is the other direction (an XML-RPC *server* on
port 12345 that other programs connect to). So flrig needs a virtual COM port
on the operator's PC, and no amount of work on the Tab5 changes that. Say so
plainly rather than implying a future release will cover it.

⚠ The 2026-10-22 draft reply claimed "flrig and N1MM+ can both talk over TCP".
Half right, and the wrong half. It was never sent.

### ⭐ The refinement this produced: answer `IF;` from cache

`IF` is in the QMX's own supported command set (`docs/qmx-1_04-cat-comparison.md`
§1.1 documents its fixed format), so a pass-through does not have to synthesise
it — the radio answers. But a Kenwood-model logger polls `IF;` several times a
second, and through Tier C every one of those is a serialized round trip against
a 300 ms budget. That is the contention point, and it is avoidable: frequency,
mode, split, RIT and TX state are **all already cached** in `cat.c`, and the
frame format is documented.

⛔ So `IF;` is a **Tier A** verb, built from cache, costing no link traffic —
the same trick that makes rigctld's `f` and `m` free today. That single verb is
most of what a logger ever asks for. Tier C stays the escape hatch.

⚠ `process_cat_message()` has no `IF` handler, which is recorded in the CAT
comparison doc as a risk for the `AI` auto-info modes. Serving `IF;` from cache
does not need one; carrying it through Tier C does not either, because the tap
(§4 Tier C item 5) matches the verb and returns the radio's own bytes.

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
| **Phase 2: raw Kenwood-CAT TCP port**, `IF;` and the common verbs served from cache | Small | ⭐ **Start here.** Copies `rigctld_server.c`'s task pattern. This is what N1MM+'s Configurer TCP option talks to, with nothing installed on the PC |
| Tier C core: poll-task slot, RX tap, verb match, timeout | Moderate | The only genuinely new mechanism; shared by both transports. Everything Phase 2 does not serve from cache falls through here |
| Dead-man `TA0;RX;` on socket close | Small | Mandatory before any keying |
| Phase 1: extend rigctld — `l`/`L` levels, `w`/`W`, `T`/`t` behind the switch | Small | All the state is already cached in `cat.c`. Benefits WSJT-X/fldigi/Log4OM, not N1MM+ |
| flrig | — | Not reachable from the Tab5 (§3). Needs a virtual COM port on the operator's PC |
| USB-CDC device | — | Not taken. Breaks the field flashing path (§2) |

Nothing here needs the radio. The Tier C core and the dead-man release are both
exercisable on the host shim and against `/api/cmd` from a browser, with no
flash and no power cycle.
