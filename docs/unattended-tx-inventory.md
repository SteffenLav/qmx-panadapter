# What can key this radio, and who decided

`tools/check_unattended_tx.py` is the authority over this table. It finds every
call to `ft8_tx_arm()` and `wspr_tx_arm()` outside their own implementations and
**fails if this file does not list exactly those sites.** Add an arming call and
the checker complains until somebody writes down what triggers it.

## Why it exists

2026-10-13. Gyula HA3HZ reported that his Tab5 answered four JS8 callers with
signal reports, on its own:

    TX burst starting (JS8): 'IU2ITE HA3HZ -14'
    TX burst starting (JS8): 'HB9ETH HA3HZ -07'
    TX burst starting (JS8): 'DM3ME HA3HZ -18'
    TX burst starting (JS8): '9A3SWO HA3HZ -15'

He was told twice that this could not happen. The JS8 phase plan said
unattended transmit arrived at **J10**; it had actually arrived at **J4**, in
v1.17.0 (`d6ff5c1e`, whose own commit message says *"ft8_robot.c opened its
pounce with a grid; it now opens with a report in JS8"*). The v1.17.0 release
note said *"a full automatic two-way JS8 QSO has not been completed yet"*,
meaning **not proven**, and it was read as **not implemented**.

⛔ **The code was never wrong. Nothing connected "a new site can key the radio"
to "the operator is told."** A phase plan is prose and went stale; a release
note was literally true and practically misleading. The operator then had to
say in public that he did not know what his own release did.

⭐ **The one genuinely reassuring fact, which nobody had stated either:**
unattended answering is **opt-in**. `ft8_robot_tick()` begins
`if (!f->robot_en) { s_robot_qso = false; return; }`. Gyula had it switched on
and called it "the automatic reply function", which is exactly what it is.

## The table

`human` = did a person decide **this particular transmission**?

| File | Function | human | What triggers it |
|---|---|---|---|
| `main/ui/ft8_tx_modal.c` | `transmit_btn_cb` | yes | The operator pressed Transmit. |
| `main/ui/ft8_screen_view.c` | `web_reply_drain` | yes | A reply the browser asked for, drained on the LVGL task. A remote human is still a human. |
| `main/net/webserver.c` | `cmd_handler` | yes | An `/api/cmd` request. Remote, deliberate. |
| `main/ft8_qso.c` | `ft8_qso_override_next` | yes | The operator overrode the next message. |
| `main/js8_ftx.c` | `js8_ftx_start` | yes | The operator pressed Send on a JS8 free-text message, having been shown its frame count and air time first. |
| `main/ui/ft8_screen_view.c` | `js8_arm_one` | yes | The operator pressed **SNR** in JS8, on the Tab5 or in the browser. One message, armed once; it is deliberately not `ft8_qso_start()`, so nothing follows it. ⭐ This site is why a JS8 row tap no longer transmits at all - Ryan KK6DZB, 2026-10-12: *"anytime I would click on a call it would auto start sending the SNR which is not handy in this mode."* The tap now selects and this button sends. |
| `main/ft8_qso.c` | `ft8_qso_start` | **no** | ⛔ **THE ONE THAT ANSWERED GYULA'S CALLERS.** Reached from a tap, but ALSO from `ft8_robot_tick()` (`ft8_qso.c:650` says so: *"'reply', not 'manual reply': ft8_robot.c calls this too"*) and from `ft8_hound.c:308`. Gated on `robot_en`, so opt-in. |
| `main/ft8_qso.c` | `ft8_qso_start_cq` | **no** | A CQ run. The operator starts it; every later call goes out without being asked again. Bounded by the stop-after-N limit, which defaults to off. ⚠ Ryan KK6DZB's point that JS8 should not repeat a CQ the way FT8 does lands here. |
| `main/ft8_qso.c` | `rearm_current` | **no** | The automatic continuation of an exchange or a CQ run - the next slot's transmission, decided by the state machine. |
| `main/ft8_qso.c` | `final_track_try_resend` | **no** | A resend inside an exchange already running. The operator consented to the QSO, not to this repeat. |
| `main/js8_ftx.c` | `js8_ftx_tick` | **no** | The second and later frames of a multi-frame JS8 message. Consented as a whole at `js8_ftx_start`, so this is a continuation rather than a new decision. |
| `main/wspr_rx.c` | `wspr_tx_sched_task` | **no** | The WSPR scheduler, on the UTC grid. Unattended by design - that is what a beacon is. Gated on WSPR TX being enabled, and `feedback_clearing_tx_en_does_not_stop_a_burst` records that clearing the flag does NOT stop a burst in flight. |

## Reading this honestly

- **Six of eleven sites can transmit with nobody deciding at that moment.**
  That is not a defect: a beacon, a CQ run and an auto-answer are all features
  somebody asked for. The point is that the number should never be a surprise.
- ⚠ **The `human` column is read by a person and can rot like any comment.**
  The checker proves the table lists the right SITES; it cannot prove these
  answers. Re-read it when touching the QSO machine.
- ⛔ **It cannot see a path that keys the radio without going through
  `ft8_tx_arm()` or `wspr_tx_arm()`.** A new TX route must be added to
  `ARMING_CALLS` in the checker, or it is invisible to all of this.

## The release rule this implies

Any release that changes this table says so in the release notes, in the
operator's terms - "your station will now answer X on its own, and here is the
switch". "Not completed" is not that sentence.
