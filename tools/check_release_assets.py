#!/usr/bin/env python3
"""Does the published release carry the asset OTA actually asks for?

⛔ WHY THIS EXISTS. v1.20.4 shipped with no `qmx_panadapter.bin`. The release
had two assets - `QMX-Panadapter-v1.20.4-flasher.zip` and
`qmx_panadapter_merged_v1.20.4.bin` - and neither is the OTA app image. Every
Tab5 in the field that tried to update got

    could not reach the download (0xffffffff)

because `main/net/update_check.c` BUILDS the URL by hand:

    https://github.com/SteffenLav/qmx-panadapter/releases/download/<tag>/qmx_panadapter.bin

It does not read the release's asset list, so a differently-named upload is a
404, and a 404 makes esp_https_ota_begin() return ESP_FAIL, which prints as
0xffffffff. Reported by Dennis WN4FLA, 2026-10-22, from the field - which is
the part that must not happen again.

⚠ THE NAMING DRIFTED ON EVERY ASSET THAT RELEASE, which is the real tell:
v1.20.3 had `QMX-Panadapter-flasher-v1.20.3.zip` + `qmx_panadapter.bin`, and
v1.20.4 had the version moved inside the zip name and a `_merged_<ver>` suffix
on the bin. Nothing checked, because nothing could: the firmware's expectation
lived in a C format string and the upload was done by hand.

So this reads the NAME OUT OF THE SOURCE rather than repeating it here. A
checker that hardcodes the name would drift exactly the way the uploads did.

Usage:
    python tools/check_release_assets.py            # newest published release
    python tools/check_release_assets.py v1.20.4    # a specific tag

Exits non-zero if the asset is missing, empty, or does not match the local
build/qmx_panadapter.bin when that file is present.
"""

import json
import os
import re
import subprocess
import sys
import hashlib

REPO = "SteffenLav/qmx-panadapter"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URL_FMT_FILE = os.path.join(ROOT, "main", "net", "update_check.c")


def ota_asset_name():
    """The filename the FIRMWARE will ask for, read from its own format string."""
    try:
        with open(URL_FMT_FILE, encoding="utf-8", errors="replace") as f:
            src = f.read()
    except OSError as e:
        sys.exit("cannot read %s: %s" % (URL_FMT_FILE, e))

    m = re.search(r'RELEASE_ASSET_URL_FMT\s+"([^"]+)"', src)
    if not m:
        sys.exit("RELEASE_ASSET_URL_FMT not found in %s - has it been renamed?\n"
                 "This checker reads the asset name out of the source on purpose;\n"
                 "fix the pattern here rather than hardcoding a filename." % URL_FMT_FILE)
    url = m.group(1)
    name = url.rstrip("/").split("/")[-1]
    if "%" in name:
        sys.exit("the OTA URL's last path element is templated (%r).\n"
                 "This checker cannot predict the filename; update it." % name)
    return name


def gh_json(args):
    try:
        out = subprocess.run(["gh"] + args, capture_output=True, text=True, check=True)
    except FileNotFoundError:
        sys.exit("gh not found - the GitHub CLI is needed to read the release")
    except subprocess.CalledProcessError as e:
        sys.exit("gh failed: %s" % (e.stderr or e.stdout or e))
    return json.loads(out.stdout)


def main():
    # ⚠ ASCII ONLY IN PRINTED OUTPUT. The first version printed ⛔ and
    # died with UnicodeEncodeError on a cp1252 console - it still exited
    # non-zero, so the build was refused, but the operator got a traceback
    # instead of the sentence telling them what to upload. Found by running
    # the failure path against v1.11.1, which genuinely lacks the asset.
    tag = sys.argv[1] if len(sys.argv) > 1 else None
    want = ota_asset_name()

    args = ["release", "view"]
    if tag:
        args.append(tag)
    args += ["--repo", REPO, "--json", "tagName,isDraft,assets"]
    rel = gh_json(args)
    tag = rel["tagName"]

    assets = {a["name"]: a for a in rel.get("assets", [])}
    print("check_release_assets: %s has %d asset(s)" % (tag, len(assets)))
    for name in sorted(assets):
        print("    %-40s %10d bytes" % (name, assets[name]["size"]))

    if want not in assets:
        print()
        print("check_release_assets: MISSING -- %s" % want)
        print("    The firmware asks for exactly this name:")
        print("    https://github.com/%s/releases/download/%s/%s" % (REPO, tag, want))
        print("    A 404 there is the '(0xffffffff)' every user sees - v1.20.4's fault.")
        print("    Fix:  gh release upload %s build/%s --repo %s" % (tag, want, REPO))
        return 1

    # ⛔ NO MERGED IMAGE MAY BE PUBLISHED. v0.18.6 shipped one and it bricked
    # a unit on every use of flash.bat; v1.20.4 shipped another, measured
    # 2026-10-22 as spanning 0x0..0x922000 with user_nvs (0x810000) and storage
    # (0x820000) ALL 0xFF - so flashing it erases WiFi credentials, callsign,
    # memory channels, the ADIF log and the LoTW private key. Nothing ships it:
    # flash.bat writes the four component files at their own offsets and the
    # flasher zip contains no merged file.
    #
    # ⚠ REMOVING `qmx merge` WAS NOT ENOUGH, which is why this is here. The
    # v1.20.4 asset was never built by that verb - it was a hand-typed 4-way
    # esptool command, while the verb was 3-way and named its output without a
    # version. The build step was never the control; the PUBLISH step is.
    merged = sorted(n for n in assets if "merged" in n.lower())
    if merged:
        print()
        for n in merged:
            print("check_release_assets: FORBIDDEN -- %s (%d bytes)"
                  % (n, assets[n]["size"]))
        print("    A merged image is a whole-chip file written at 0x0. Its")
        print("    user_nvs and storage regions are blank padding, so flashing it")
        print("    erases WiFi, callsign, memory channels, the log and the LoTW key.")
        print("    Nothing ships it - flash.bat writes the four files separately.")
        print("    Fix:  gh release delete-asset %s %s --repo %s" % (tag, merged[0], REPO))
        return 1

    got = assets[want]
    if got["size"] <= 0 or got["state"] != "uploaded":
        print("check_release_assets: BAD -- %s is %d bytes, state=%s"
              % (want, got["size"], got["state"]))
        return 1

    local = os.path.join(ROOT, "build", want)
    if os.path.isfile(local):
        local_size = os.path.getsize(local)
        if local_size != got["size"]:
            print("check_release_assets: MISMATCH -- %s is %d bytes on the release but %d locally"
                  % (want, got["size"], local_size))
            print("    The published image is not the one in build/. Re-upload or re-check.")
            return 1
        # digest is "sha256:<hex>" when GitHub has computed one
        digest = (got.get("digest") or "")
        if digest.startswith("sha256:"):
            h = hashlib.sha256()
            with open(local, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            if h.hexdigest() != digest.split(":", 1)[1]:
                print("check_release_assets: MISMATCH -- %s sha256 differs from build/" % want)
                return 1
            print("check_release_assets: %s matches build/ byte for byte (sha256)" % want)
        else:
            print("check_release_assets: %s matches build/ by size "
                  "(no digest published yet)" % want)
    else:
        print("check_release_assets: build/%s not present - size/state checked only" % want)

    print("check_release_assets: OTA can reach %s on %s" % (want, tag))
    return 0


if __name__ == "__main__":
    sys.exit(main())
