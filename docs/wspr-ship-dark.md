# Shipping WSPR on the main track, switched off

The plan is to put the WSPR code into a normal release *before the feature is
finished*, with it disabled by default, so that:

- the OTA update path that will eventually deliver it is exercised for real,
  on a binary that actually contains it;
- one operator can turn it on and use it on a released build rather than a
  branch build, so what gets tested is what everyone else is running;
- nobody who did not go looking for it ever meets a half-built mode.

This note records what "switched off" does and — more importantly — what it
does **not** do, plus what is verified and what is not.

## The switch

`wspr_en`, NVS key `wspr_en`, dirty bit 105, **default false**.

Turn it on with either of:

```bash
curl -s -X POST http://qmx.local/api/cmd -d '{"action":"wspr_enable","on":true}'
```

or `wspr_enabled = yes` in an imported config file (`POST /api/config`).

There is deliberately **no drawer control and no web checkbox**. An unfinished
mode should be reachable by someone who went looking for it, not offered in a
list of settings next to things that are finished.

Turning it off while the page is up also leaves the page. Without that the
operator is stranded on a screen the swipe cycle can no longer reach, which is
a trap rather than a setting.

## What "off" actually gates

Every one of these asks the same function, `wspr_feature_enabled()`. Four gates
each reading the setting for themselves is how a feature ends up half
reachable.

| Surface | With `wspr_en` false |
|---|---|
| Tab5 swipe cycle | Panadapter ↔ FT8, exactly the existing behaviour |
| Tab5 boot | cannot land on WSPR — `ui_apply_saved_mode()` restores only FT8, everything else falls through to Panadapter |
| `POST /api/cmd` `set_screen: "wspr"` | refused, with a reason in the body |
| `GET /api/wspr` | `{"enabled":false, ...}` — not an empty spot list |
| Web UI WSPR card | hidden, and not polled |
| `wspr_rx_start()` | refuses; the 8.6 MB decode loop cannot be allocated |

The refusals are deliberately loud. `/api/cmd` answers an action it does not
recognise with `unknown action` and **HTTP 200**, so a gate that declines
quietly is indistinguishable from a typo — a distinction that has already cost
this project an evening.

## ⛔ What "off" does NOT buy

**`.bss` is allocated whether the code runs or not.** A gate that stops the
page opening, the task spawning and the decode buffers being allocated does
nothing whatsoever about static memory.

Measured before doing anything about it:

| | internal `.bss` |
|---|---|
| WSPR's share, as the branch stood | **15,914 B** |
| after moving the cold buffers to PSRAM | **6,230 B** |

For scale: v1.9.3 recovered 14,084 B of internal RAM and that was the whole
difference between an OTA download completing and taking four hardware
watchdog resets. Shipping WSPR as it stood would have handed that margin
straight back — to every user, including the ones who never enable it.

What moved (`EXT_RAM_BSS_ATTR`): `wspr_rx.c`'s `samp[2048]` (8,192 B, touched
once per 120 s cycle), `med[]` (820 B, first cycle only), and
`wspr_screen_view.c`'s `snap[]` (672 B, one list rebuild per second).

What deliberately did **not** move, and the reasons are not interchangeable:
`wspr_decode.c`'s LPF ring and coefficients (3,076 B) are read ~11.5 M times
per candidate — hardware measured 40 s versus 2.8 s per candidate when that
path was got wrong — and `colmap` (1,888 B) is read once per *pixel* of a
waterfall repaint.

**Generalise it:** before shipping any feature dark, measure its static
footprint. "The flag is off" is exactly the reasoning that skips the
measurement.

## Other costs of carrying the code

| | figure |
|---|---|
| binary size | 0x342730 (3.42 MB) of a 4 MB app partition, 19 % free |
| DIRAM total | 292,501 B of 445,392 (65.7 %), 152,891 free |

The app partition has room. The OTA path needs the *internal heap* margin more
than it needs flash, which is why the `.bss` work above is the load-bearing
part of this plan rather than the binary size.

## Merging

`origin/main` (`14d9093`) is an **ancestor** of `feat/wspr-page`, verified
against the live remote. So putting this on main is a fast-forward — there is
no merge to resolve and no conflict to get wrong.

## What is verified, and what is not

Verified on the host or by measurement:

- the 24-station reference decode set, the ~2 dB sensitivity deficit and zero
  fabrications (`tools/wspr_noise_ladder.py`, both arms rebuilt in one session)
- the internal `.bss` figures (`idf.py size` → `size-components` → `nm`)
- dirty-bit uniqueness (`tools/check_dirty_bits.py`, in the build,
  mutation-tested 5/5)
- the firmware builds, and the web UI's JavaScript parses

**Not verified — nothing has been flashed.** The QMX needs a hand on its power
switch after every flash, so the following are reasoned, not observed:

- the gate behaving on a real device in every one of the six rows above
- the per-phase millisecond savings, which are projections from the *old*
  build's `[mix + coarse + curve + dec ms]` line
- an OTA download and install of a binary containing WSPR

That last one is the entire point of the exercise, so it is the first thing to
do once the feature is on a released build.

## First checks after the first flash

1. `wspr_en` defaults false on a device that has never seen it — confirm the
   swipe cycle is Panadapter ↔ FT8 and the web card is absent.
2. `GET /api/wspr` returns `enabled:false`.
3. Enable it, confirm all six surfaces come alive.
4. Disable it *while the WSPR page is up* and confirm the device leaves the
   page rather than stranding on it.
5. Read the per-candidate phase timings and compare against a figure **from
   the same boot** — per-candidate timing varies about 35 % between boots, so a
   comparison against numbers recorded on another boot measures the boot.
