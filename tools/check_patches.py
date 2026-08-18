#!/usr/bin/env python3
"""Fail the build when a standing patch is missing.

WHY THIS EXISTS (TODO #181, written 2026-08-17 the same evening it was needed):

Three patches had silently vanished from `managed_components/` - a git-ignored
directory, so nothing in the repo could show it - and the resulting binary wedged
WiFi within about four minutes of every boot. The symptom is indistinguishable
from a hardware fault: the device stays completely healthy (no reboot, no panic,
audio still streaming) while the C6 co-processor stops answering. It cost an
evening and TWO wrong hypotheses (BLE scanning, then DMA starvation) before anyone
thought to check whether the patches were still applied.

The only thing that had ever said "re-apply them" was the release process. A plain
`idf.py build` would happily produce that broken binary, which is the gap this
closes.

TWO RULES FOR THIS FILE:

  1. The markers below are the SAME strings each apply_*.ps1 script tests for its
     own idempotency. Do not invent a different marker here - if the two ever
     disagree, this check starts lying, and a check that lies about patches is
     worse than no check at all. When a patch script's marker changes, change it
     here in the same commit.

  2. Missing patches are an ERROR, never a warning. A warning scrolls past in a
     long IDF build, which is exactly how it would go unnoticed again.

A missing FILE is only a warning: the managed_components tree may legitimately not
be fetched yet on the very first configure, and the IDF-tree patches live outside
this repo entirely (they are per-build-machine, wiped by an IDF reinstall). It is a
present-but-unpatched file that means trouble.
"""

import os
import sys

# (script to run, path relative to what, path, marker, why it matters)
#
# "repo" = this checkout; "idf" = the pinned ESP-IDF install tree.
PATCHES = [
    ("apply_esp_hosted_psram.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/port/include/os_wrapper.h",
     "extra_heap_caps = MALLOC_CAP_SPIRAM",
     "WiFi transport buffers stay in scarce internal DRAM; the device reboots "
     "under QMX+FT8 load when WiFi TX bursts"),

    ("apply_esp_hosted_sdio_recovery.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c",
     "SDIO RX oversize",
     "an oversized SDIO pending-byte delta livelocks the link: WiFi dies within "
     "minutes and every RPC times out forever (reboot is the only way out)"),

    ("apply_cdc_acm_close_tolerant.ps1", "repo",
     "managed_components/espressif__usb_host_cdc_acm/cdc_acm_host.c",
     "PATCHED (qmx-panadapter, 2026-08-16)",
     "closing a CDC interface while a URB is in flight abort()s the device, i.e. "
     "a reboot on a busy port"),

    ("apply_hcd_bulk_error_recovery.ps1", "idf",
     "components/usb/hcd_dwc.c",
     "PATCHED (qmx-panadapter, 2026-07-16)",
     "a transient USB bulk error abort()s the device instead of being retried"),

    ("apply_hub_recover_tolerant.ps1", "idf",
     "components/usb/hub.c",
     "PATCHED (qmx-panadapter, 2026-08-03)",
     "a root-port recover that races the hub FSM abort()s the device"),

    ("apply_usb_dwc_hal_chan_error_tolerant.ps1", "idf",
     "components/hal/usb_dwc_hal.c",
     "PATCHED (qmx-panadapter, 2026-08-18)",
     "a USB channel error arriving without the halt bit abort()s the device - and "
     "that warm reset then leaves the QMX unable to re-enumerate for hours"),

    ("apply_fatfs_exfat.ps1", "idf",
     "components/fatfs/src/ffconf.h",
     "#define FF_FS_EXFAT\t1",
     "microSD cards larger than 32 GB (exFAT) will not mount"),
]


def marker_present(path, marker):
    """Substring test, whitespace-insensitive so a tab/space difference in a
    #define cannot produce a false alarm."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None                      # unreadable == treat as absent-file
    squash = " ".join(text.split())
    return " ".join(marker.split()) in squash


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    idf = os.environ.get("IDF_PATH", "")

    missing, absent = [], []
    for script, root, rel, marker, why in PATCHES:
        base = repo if root == "repo" else idf
        if not base:
            absent.append((script, rel, "IDF_PATH is not set"))
            continue
        path = os.path.join(base, rel.replace("/", os.sep))
        if not os.path.isfile(path):
            absent.append((script, rel, "file not present"))
            continue
        if not marker_present(path, marker):
            missing.append((script, rel, why))

    for script, rel, note in absent:
        print("check_patches: SKIP %s (%s) - %s" % (rel, script, note))

    if missing:
        print("")
        print("=" * 78)
        print("check_patches: %d STANDING PATCH(ES) MISSING - refusing to build" % len(missing))
        print("=" * 78)
        for script, rel, why in missing:
            print("")
            print("  %s" % rel)
            print("     consequence : %s" % why)
            print("     fix         : powershell -File tools/patches/%s" % script)
        print("")
        print("  These live outside version control (managed_components/ is git-ignored;")
        print("  the IDF-tree ones are per-build-machine), so a clean fetch, an")
        print("  `idf.py fullclean`, a dependency refresh or an IDF reinstall silently")
        print("  removes them. The resulting binary looks fine and then fails on")
        print("  hardware in a way that reads as a hardware fault - see TODO #180.")
        print("")
        return 1

    print("check_patches: all %d standing patches present" % len(PATCHES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
