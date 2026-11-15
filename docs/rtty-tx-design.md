# RTTY TX (R010) - design

Gate: R009 passed on air 2026-11-14 (fldigi copied a `TA`-keyed burst clean).
So TX = the R009 mechanism: `TX;`, then one `TA<Hz>;` per tone change, `RX;`.

## Decisions

| Question | Decision | Who |
|---|---|---|
| Where TX lands | On the decoder: tone centre = tap offset + AFC, mark/space at +/- shift/2 (reverse swaps them). You land on the station you answer. | design |
| AFC not confirmed by a decode | If no station was decoded since the tap (R026 not holding), AFC is reset at key-down and TX goes to the tap. First on-air CQ went 145 Hz off a free-spot tap: acquisition had jumped to a weak pair. | design, bench2 2026-11-14 |
| Lines outside the passband | At key-down the DIAL moves so the centre sits at 1500 Hz audio; the decoder offset moves by the same amount, so RF, lines and decode stay put. | operator 2026-11-14 ("Move the dial") |
| Passband accepted without a move | centre - shift/2 >= 400 Hz and centre + shift/2 <= 2700 Hz | design |
| Abort | Stop = `TA0;` + `RX;` at once (TA stops immediately; `KY` cannot, so RTTY never uses KY) | design |
| Typing stops while keyed | Diddle (LTRS) to hold the link; unkey after 3 s with nothing queued | design |
| Hard limit | 180 s key-down per over, then unkey whatever is queued | design (never leave a radio transmitting) |
| Refusals | no callsign, sim mode, WSPR running, another burst holds CAT, band guard (re-checked after any dial move, right above key-down) | as R009 |
| Radio mode | not DiGi: the over task switches it (MD6 via the poll task, confirmed within 1.5 s) - was a refusal until 2026-11-15 | as FT8 TX |

## Shape

- `rtty_tx_core.c` - no ESP: text queue + Baudot (USOS) + the code stream
  (preamble LTRS, text, diddle, tail). Host harness: core -> FSK IQ ->
  our demod -> same text back.
- `rtty_tx.c` - the device sequencer task (from R009's burst loop, which
  it replaces): keying, QSY, absolute-time bit edges, Stop, limits.
- API: `/api/cmd` `{"action":"rtty_send","text":".."}`, `{"action":"rtty_stop"}`;
  `{"action":"rtty"}` status gains `tx` state.
- UI: the RTTY page's "TX not built yet" box becomes the TX pane -
  layout shown to the operator BEFORE it is flashed.
- Inventory: `docs/unattended-tx-inventory.md` - RTTY send is human
  (a press); the diddle/continuation of an over is a continuation of it.
