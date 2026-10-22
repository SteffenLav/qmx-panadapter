#!/usr/bin/env python3
"""Host harness for tools/check_release_assets.py.

    python test/check_release_assets_harness.py

⛔ WHY IT EXISTS. The checker's whole job is to refuse a release, and the
refusal paths are the ones that never run in normal use - so they are the ones
that rot. The checker's FIRST version printed an emoji and died with
UnicodeEncodeError on the cp1252 console: it still exited non-zero, so a build
would have been refused, but the operator got a traceback instead of the line
telling them what to upload. That was found by running the failure path, not by
reading it.

A live release cannot be used to test the "forbidden merged asset" path, because
no release carries one any more (v1.20.4's was deleted 2026-10-22 and a scan of
all ~100 releases found no other). So the gh call is stubbed and the verdicts
are asserted here.

Exits non-zero on any disagreement.
"""

import contextlib
import importlib.util
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(ROOT, "tools", "check_release_assets.py")

OTA = "qmx_panadapter.bin"
OTA_SIZE = 5319072          # v1.20.4; only needs to match build/ when that exists


def asset(name, size, state="uploaded", digest=""):
    return {"name": name, "size": size, "state": state, "digest": digest}


CASES = [
    # (name, assets, expected exit code, text that must appear)
    ("clean release",
     [asset(OTA, OTA_SIZE)], 0, "OTA can reach"),

    ("OTA asset missing - v1.20.4's fault",
     [asset("QMX-Panadapter-v1.20.4-flasher.zip", 2959558)], 1, "MISSING"),

    ("merged image published - v0.18.6's and v1.20.4's fault",
     [asset(OTA, OTA_SIZE), asset("qmx_panadapter_merged_v1.20.4.bin", 9576448)],
     1, "FORBIDDEN"),

    ("merged match is case-insensitive",
     [asset(OTA, OTA_SIZE), asset("QMX_MERGED_IMAGE.BIN", 9576448)], 1, "FORBIDDEN"),

    ("asset present but zero bytes",
     [asset(OTA, 0)], 1, "BAD"),

    ("asset still uploading",
     [asset(OTA, OTA_SIZE, state="starter")], 1, "BAD"),

    ("published size differs from build/",
     [asset(OTA, OTA_SIZE + 1)], 1, "MISMATCH"),

    ("empty release",
     [], 1, "MISSING"),
]


def main():
    spec = importlib.util.spec_from_file_location("cra", TARGET)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    have_build = os.path.isfile(os.path.join(ROOT, "build", OTA))
    if not have_build:
        print("note: build/%s absent - the size/sha comparison cases are skipped" % OTA)

    sys.argv = ["check_release_assets.py"]
    fails = 0
    print("%-52s %-6s %s" % ("case", "exit", "verdict"))
    for name, assets, want_rc, want_text in CASES:
        if not have_build and want_text == "MISMATCH":
            print("%-52s %-6s skipped (needs build/)" % (name, "-"))
            continue
        mod.gh_json = lambda args, _a=assets: {
            "tagName": "vTEST", "isDraft": False, "assets": _a}
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                rc = mod.main()
        except SystemExit as e:              # sys.exit() inside the checker
            rc = e.code if isinstance(e.code, int) else 1
        except Exception as e:               # a traceback IS the failure here
            print("%-52s %-6s CRASHED: %r" % (name, "!", e))
            fails += 1
            continue
        out = buf.getvalue()
        ok = (rc == want_rc) and (want_text in out)
        if not ok:
            fails += 1
            print("%-52s %-6d <<< WRONG (wanted exit %d containing %r)"
                  % (name, rc, want_rc, want_text))
            print(out)
        else:
            print("%-52s %-6d ok" % (name, rc))

    print()
    print("%d case(s), %d wrong" % (len(CASES), fails))
    if fails:
        print("A false PASS here publishes a release the updater cannot use, or")
        print("leaves a whole-chip image on the page that erases the LoTW key.")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
