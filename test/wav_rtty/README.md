# RTTY test recordings

Decode one: `rtty_harness --wav <f> --centre <Hz> [--lsb] --afc 150 --squelch 0.48`
(build line at the top of `test/rtty_harness.c`).

## In this folder

- `20261112_1633_7046510_usb.*` - bench2, 40 m, rxcap. See its `.txt`.

## Not in this folder: VE3NEA's GRITTY demo set

Five off-air 6 kHz / 60 s WAVs from https://dxatlas.com/Gritty/Demo/
("feel free to download them and do your own tests" - no redistribution
grant, so they are NOT committed). Each has a PNG of the MMTTY and GRITTY
decodes, which is the reference. All are MMTTY tones (2125/2295, mark low):
use `--centre 2210 --lsb`; the real centres are 2130-2210 Hz.

Results 2026-11-12 (R013 acquisition, AFC +/-150, squelch 0.48):

| File | Condition | Ours | vs MMTTY / GRITTY |
|---|---|---|---|
| SX150ITU | strong | 3 of 3 CQ lines, one with 3 errors | equal |
| II1SAP | strong, QSB nulls | 3 CQ lines + "SPECIAL STATION ON BOARD THE HISZRIC SUBMARINSANDREA PROVANA ... MORE INFO WWW.QRZ.CO" | equal to GRITTY (before R013: nothing - signal 80 Hz off) |
| R120RB | weak, deep QSB | UR5XMM 599, UR9XMM TU R120RB QRZ, CQ R120RB R120RB | better than MMTTY, near GRITTY |
| PA4PS | contest, QRM | PA4PS ~5x, much garbage | worse than GRITTY (~8x) - R015 |
| DL2AL | contest, clipped | "DL2AL", "DLALT PSE AGAIN", "R3BT" | partial; misses "599 247-1711 TNX/73" that both copy - R015 |
