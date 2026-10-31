# Hamlib rigctld

The Tab5 runs a Hamlib **NET rigctl** server on TCP port **4532**. Any
Hamlib-aware program on your network — a logger, WSJT-X, fldigi, N1MM — can
read and set the QMX's frequency and mode through it, with nothing installed on
the PC and no serial cable.

It starts on its own as soon as the Tab5 joins WiFi. There is no setting to
turn on.

## Connecting

Point your program at the IP address shown on the Tab5's bottom bar, port 4532,
and pick the Hamlib rig model **NET rigctl** (model 2).

### QLog

Thanks to Brian KB9WFS for these steps.

1. Add a rig, and choose interface **Hamlib**, then **Hamlib NET rigctl**.
2. Set the **TX range** to match your QMX.
3. Set the **default PWR**.
4. Leave the features at their defaults.
5. For the host name enter the **IP address shown on the Tab5 screen**, and
   leave the port at **4532**.

### WSJT-X and fldigi

Set the rig to **Hamlib NET rigctl**, and the network server to
`<tab5-ip>:4532`. Leave PTT method at **none** — see the limits below.

### From a terminal

`rigctl` itself is the quickest way to prove the link before you configure
anything else:

```bash
rigctl -m 2 -r <tab5-ip>:4532 f
```

That prints the dial frequency in Hz. If it answers, the path works and
anything else that failed is configuration in the other program.

## What it supports

| Command | Meaning |
|---|---|
| `f` / `F` | get / set frequency |
| `m` / `M` | get / set mode and passband |
| `v` | get VFO (always `VFOA`) |
| `s` | get split (always off) |
| `t` | get PTT |
| `q` | quit |

It also answers the `\dump_state`, `\chk_vfo`, `\get_powerstat`,
`\get_lock_mode` and `\get_vfo` handshake commands that Hamlib sends when a
program first connects.

Up to **four** programs can be connected at once.

## Limits worth knowing

**It cannot key the radio.** There is no `T` (set_ptt) command, and `t`
(get_ptt) always answers "not transmitting". A logger or digital-mode program
cannot start a transmission through this port, and will never see the Tab5
report that it is transmitting. Set PTT to **none**, VOX, or CAT on the QMX's
own serial port, depending on how you normally key it.

This is also why the TX range and default power you enter in your logger are
for that program's own bookkeeping. The Tab5 does not read them back.

**No split.** `s` always reports split off and `VFOA`.

**Frequency and mode only.** S-meter reads, filter width and level controls are
not on this port. The [Web API](web-api.md) carries the rest of the radio's
state if you need it from a script.

## If it does not answer

- Check the IP on the Tab5's bottom bar. It can change if your router hands out
  a new lease — pin it in the router if you use it often.
- Only four programs can connect at once. Close one you are not using.
- The server needs WiFi. If the Tab5 is not associated, there is no port to
  reach.
