# Remote Access

Reaching the Tab5 from outside your home.

## The short answer

The web interface has **no password and no TLS**, and it can key the radio. So
do not expose it to the internet directly. Put it behind a tunnel instead, and
reach it as if you were at home.

Two tunnels are worth your time:

- **[Tailscale](#tailscale)** — easiest. No router changes, no fixed IP address.
- **[WireGuard](#wireguard)** — more setup, nothing to sign up for.

Either one runs on something else in the house — a Raspberry Pi, a PC that is
on anyway, or a router that supports it. The Tab5 cannot run a tunnel itself.

## Why there is no login

A password is only as private as the link it crosses. The Tab5 serves plain
HTTP, so a password typed into it travels the internet in clear, and so does
everything it was meant to protect. A code from an authenticator app does not
change that — every site you have used one on runs it over HTTPS first.

A half-built login on a device that transmits fails towards *somebody else is
keying your radio*, which is worse than no remote access at all. So the tunnel
is the login. It encrypts the link and it decides who gets in, before anything
reaches the radio.

## Tailscale

Tailscale builds a private network between your own devices. Nothing is
exposed to the internet, and you do not touch your router.

1. Make a free Tailscale account.
2. Install Tailscale on a machine at home that stays on — a Raspberry Pi, a
   NAS, a desktop. This machine is your way in.
3. On that machine, advertise the route to your home network. If your home
   network is `192.168.1.0/24`:

        sudo tailscale up --advertise-routes=192.168.1.0/24

4. In the Tailscale admin console, approve that route.
5. Install Tailscale on your phone or laptop and sign in with the same account.

Now, from anywhere, open the Tab5's normal home address in a browser — for
example `http://192.168.1.210`. It works exactly as it does on the sofa.

!!! tip "Find the Tab5's address"
    It is on the bottom status bar of the Tab5 screen.

## WireGuard

WireGuard does the same job with no account and no third party, in exchange for
one port forward and a little more setup.

1. Install WireGuard on a machine at home that stays on, or enable it on your
   router if it has it built in.
2. Forward **one** UDP port on your router to that machine — the WireGuard
   port, usually `51820`. Forward nothing else.
3. Generate a key pair for the server and one for each phone or laptop.
4. On the client, set the tunnel to route your home network through it. If your
   home network is `192.168.1.0/24`:

        [Peer]
        AllowedIPs = 192.168.1.0/24

5. Bring the tunnel up, then open the Tab5's normal home address.

The forwarded port exposes WireGuard, not the Tab5. WireGuard does not answer
an unauthenticated packet at all, so a scanner finds nothing there.

## If you forward the web port directly

Some people forward the Tab5's web port straight out to the internet. It works,
and it is the one arrangement worth being blunt about:

- Anyone who finds that port can key your transmitter, from anywhere. There is
  no password to stop them.
- Putting the Tab5 in a DMZ or IoT network is good practice and protects the
  rest of your house. It does not protect the radio, and it does not protect
  your licence.

If that is your setup today, you have two ways forward:

- **Turn on the TX interlock** (below) so the radio cannot be keyed from
  outside, and keep the remote view and the stop controls.
- **Put a reverse proxy with a login and TLS in front of it** and forward to
  the proxy instead of the Tab5. The password then crosses an encrypted link,
  and the Tab5 sees the proxy as a local caller.

## The TX interlock

**Settings → TX interlock.** Off by default.

When it is on, the Tab5 refuses any transmit request that did not arrive from
its own network. It checks the address the request actually came from, taken
from the connection itself, so it cannot be faked by a header.

It is **off by default on purpose**: switching it on would take transmit away
from remote stations that work today, with no warning. Turn it on yourself if
you want it.

### What it does and does not do

| | |
|---|---|
| Stops | Someone on the internet who found your forwarded port |
| Does **not** stop | A guest on your WiFi, or anyone holding the WiFi key |
| Does **not** stop | A caller on another of your own networks, if the router between them translates addresses |

It is a second lock, not a login. "On my LAN" is a weak identity. The tunnel is
the first lock and the important one.

### A router that translates addresses passes the interlock

The check reads the address off the connection, which is the only address a
caller cannot choose. That address is the last thing that spoke to the Tab5. If
a router sits between two of your networks and translates addresses as it
passes traffic — NAT, masquerading, "DMZ" on many home routers — then the
address the Tab5 reads is the **router's**, not the computer behind it. The
router is on the Tab5's own network, so the request is allowed.

That is the interlock working as built, not a fault. No check of this kind can
see past a translating router; the address of the original caller is gone before
the request arrives.

**How to tell.** Transmit once with the interlock on and look at the Tab5's log
(Settings → Diagnostics, or `/api/log`). Every transmit decision is recorded,
including the ones it allows:

```
W (73118) tx_subnet: CQ run ALLOWED by the TX interlock: peer 192.168.1.167 is
on-subnet (ours 192.168.1.210 mask 255.255.255.0)
```

If the `peer` address is your router rather than the computer you are sitting
at, the traffic is being translated. Put the Tab5 and the computer on the same
network, or route between the two networks without translating, and the
interlock will then see the real caller.

### Stops are never refused

`Cancel`, `Pause`, `Skip`, stop tuning, stop RTTY and power off **always work**,
whatever the interlock is set to and wherever you are connecting from.

Refusing a stop would hold a transmitter on the air, so the interlock is not
allowed anywhere near them. If something goes wrong while you are away, you can
always shut it down.

### Will my tunnel pass the interlock?

It depends on whether your tunnel makes your traffic look like it came from the
home network, and that varies between setups. You do not have to guess.

Turn the interlock on, connect through your tunnel and try to transmit. If it
refuses, the message names the address you arrived from:

> Antenna Tune was refused: this request came from 100.81.4.12, which is not on
> the Tab5's own network. Reach the Tab5 through a VPN or tunnel, or turn the
> TX interlock off in Settings.

Compare that address with the Tab5's own, on its status bar. If they are on the
same network, you are through. If they are not, your tunnel is routing you
rather than presenting you as local — leave the interlock off, or set the
tunnel to masquerade traffic onto the home network.

## What to do, by situation

| Your situation | Do this |
|---|---|
| Only ever at home | Nothing. |
| Want to check in from away | Tailscale. Leave the interlock off. |
| No accounts, happy with router setup | WireGuard. Leave the interlock off. |
| Web port forwarded today | Turn the interlock on now, then move to a tunnel. |
| Running a shared or networked station | Leave the interlock off; it refuses remote callers by design. |
