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
| `main/net/webserver.c` | `cmd_handler` | yes | An `/api/cmd` request. Remote, deliberate. ⚠ **Who may make it is now a setting (T206, `tx_subnet_interlock`, DEFAULT OFF).** With it on, an `/api/cmd` action that transmits is refused unless the request arrived from the Tab5's own subnet, and the refusal goes back as 403 with `tx_interlock:true` - not only to the log. `cancel`, `pause`, `skip`, `tune_stop` and `rtty_stop` are deliberately NOT interlocked: refusing a stop would be the interlock holding a transmitter on the air. ⛔ It is a subnet test, NOT a login - it stops the internet, not a guest on the WiFi (`main/tx_subnet.h`). Includes `rtty_send` (R010 RTTY TX): text typed in the browser's RTTY panel or from a script, or `"macro"` - one of the four RTTY messages, a click in the browser; one over, unkeyed 3 s after the queue empties and at 180 s at the latest, Puts the radio in DiGi mode first if it is not; band-guarded after any dial move. The rest of an over (diddles, queued text) is a continuation of that request. |
| `main/ui/rtty_screen_view.c` | `macro_cb` | yes | RTTY page TX pane (R010): a press of CQ / Answer / Exch / 73 (text built by `rtty_tx_macro()`, shared with the web). Every call is a touch. One over, unkeyed 3 s after the queue empties and at 180 s at the latest; STOP and leaving the page stop it at once. Puts the radio in DiGi mode first if it is not; band-guarded after any dial move. |
| `main/ui/rtty_screen_view.c` | `tx_send` | yes | RTTY page TX pane (R010): a key typed after Type... (each key is sent as typed; closing the keyboard ends the over). Every call is a touch. One over, unkeyed 3 s after the queue empties and at 180 s at the latest; STOP and leaving the page stop it at once. Puts the radio in DiGi mode first if it is not; band-guarded after any dial move. |
| `main/ui/cw_screen_view.c` | `send_cw_text` | yes | CW page: a memory button, the NEXT button, or a `cw_send` from the browser (`/api/cmd`, drained on the LVGL task while the page is shown). The two buttons are a press; the browser send is a remote request. The QMX keys it with its own keyer (`KY`). No abort once the radio holds the text: Stop and leaving the page drop only what it has not received. Band-guarded in `cat.c` against dial and dial + CW TX offset. ⚠ A `cw_send` that arrives while the page is not shown stays queued and is keyed on the next entry to the page. |
| `main/ft8_qso.c` | `ft8_qso_override_next` | yes | The operator overrode the next message. |
| `main/ft8_qso.c` | `ft8_qso_final_resend_now` | yes | The operator pressed **Re-send** on a contact whose final has gone out but has not been acknowledged (Don WB0LQW). One message, armed once. It does not spend `final_track_try_resend`'s automatic budget, so pressing it cannot exhaust the machine's own fallback. |
| `main/js8_ftx.c` | `js8_ftx_start` | yes | The operator pressed Send on a JS8 free-text message, having been shown its frame count and air time first. |
| `main/ui/ft8_screen_view.c` | `js8_arm_one` | yes | The operator pressed **SNR** in JS8, on the Tab5 or in the browser. One message, armed once; it is deliberately not `ft8_qso_start()`, so nothing follows it. ⭐ This site is why a JS8 row tap no longer transmits at all - Ryan KK6DZB, 2026-10-12: *"anytime I would click on a call it would auto start sending the SNR which is not handy in this mode."* The tap now selects and this button sends. |
| `main/ft8_qso.c` | `ft8_qso_start` | **no** | ⛔ **THE ONE THAT ANSWERED GYULA'S CALLERS.** Reached from a tap, but ALSO from `ft8_robot_tick()` (`ft8_qso.c:650` says so: *"'reply', not 'manual reply': ft8_robot.c calls this too"*) and from `ft8_hound.c:308`. Gated on `robot_en`, so opt-in. |
| `main/ft8_qso.c` | `ft8_qso_start_cq` | **no** | A CQ run. The operator starts it; every later call goes out without being asked again. Bounded by the stop-after-N limit, which defaults to off. ⚠ Ryan KK6DZB's point that JS8 should not repeat a CQ the way FT8 does lands here. |
| `main/ft8_qso.c` | `rearm_current` | **no** | The automatic continuation of an exchange or a CQ run - the next slot's transmission, decided by the state machine. ⚠ Also what transmits after `resume_restore()`, which has two automatic triggers, both only for a station the operator already chose to work, within 5 minutes (`QSO_RESUME_WINDOW_SEC`), and only while the machine is idle or running CQ: the partner sends a message to us again (comeback, since 2026-07-16), and - since 2026-11-24 - a partner we dropped because they were busy with someone else signs off with that station (73/RR73/RRR) or calls CQ (re-call, `resume_target_free_heard()`). The re-call fires at most once per drop. |
| `main/ft8_qso.c` | `final_track_try_resend` | **no** | A resend inside an exchange already running. The operator consented to the QSO, not to this repeat. |
| `main/js8_ftx.c` | `js8_ftx_tick` | **no** | The second and later frames of a multi-frame JS8 message. Consented as a whole at `js8_ftx_start`, so this is a continuation rather than a new decision. |
| `main/wspr_rx.c` | `wspr_tx_sched_task` | **no** | The WSPR scheduler, on the UTC grid. Unattended by design - that is what a beacon is. Gated on WSPR TX being enabled, and `feedback_clearing_tx_en_does_not_stop_a_burst` records that clearing the flag does NOT stop a burst in flight. |

## Reading this honestly

- ⛔ **DO NOT WRITE THE TOTALS HERE.** This line read "Six of eleven sites"
  while the table held twelve and then thirteen - the same rot `CLAUDE.md`
  records for its patch count and its own copy of this number.
  `python tools/check_unattended_tx.py` prints both figures and is the
  authority; run it rather than reading a sentence.
- **Some of these sites can transmit with nobody deciding at that moment.**
  That is not a defect: a beacon, a CQ run and an auto-answer are all features
  somebody asked for. The point is that the number should never be a surprise.
- ⚠ **FT8/FT4 band hopping moves these sites to other bands** (operator,
  2026-11-26). ⛔ **Switched off in v2.1.2** (`FT8_HOP_ENABLED 0`,
  `main/storage/settings.h`): the hop mask reads as 0, so nothing below
  applies to a released build until it is switched back on. `ft8_hop.c` arms nothing, so it is not a row. But since then a
  CQ run, auto-answer and auto-work pileup keep running through a hop, so
  they transmit on every band the operator ticked, not only the one he
  dialled. The hop waits for a QSO with a partner; each key-down still passes
  the band guard. A band change by any other route still stands them down
  (`ft8_band_change_stand_down`, #144).
- ⚠ **The `human` column is read by a person and can rot like any comment.**
  The checker proves the table lists the right SITES; it cannot prove these
  answers. Re-read it when touching the QSO machine.
- ⛔ **It cannot see a path that keys the radio without going through
  `ft8_tx_arm()` or `wspr_tx_arm()`.** A new TX route must be added to
  `ARMING_CALLS` in the checker, or it is invisible to all of this.
- ⚠ **`human: yes` on the web row means a person asked, not that the person
  was entitled to.** Until T206 nothing checked where the request came from,
  and the API has no password, so "a remote human is still a human" was true
  of any human on the internet who could reach the port. The interlock is the
  second lock and it is off by default; the first lock is a tunnel or a TLS
  reverse proxy, which is not firmware work. `tools/check_tx_guard.py` is the
  authority over which web TX sites consult it - and a new web TX route it
  cannot name is invisible to that too.

## The release rule this implies

Any release that changes this table says so in the release notes, in the
operator's terms - "your station will now answer X on its own, and here is the
switch". "Not completed" is not that sentence.
