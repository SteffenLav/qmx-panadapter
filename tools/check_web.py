#!/usr/bin/env python3
"""Fail the build when the web UI's JavaScript does not parse.

WHY THIS EXISTS (TODO #183, written 2026-08-18 the morning after it shipped):

v1.8.5 went out with an unterminated string literal in index.html. One broken
literal means the ENTIRE inline <script> fails to parse, so the page renders its
controls and then does nothing at all - no WebSocket, no spectrum, no buttons, just
"disconnected" in red. Randy N4OPI and Michael KZ4LY both reported it within hours.

It cost nothing to detect and everything to miss, which is the definition of a check
worth having. `node --check` finds it in about 50 ms.

The cause is worth recording because it was not a typo: the literal was written by a
script whose shell collapsed a backslash-n into a real newline before Python saw it.
The same collapsing produced a NUL byte in ft8_qso.c and a raw DEL byte in
qmx_term.c on the same evening - the C compiler caught both of those, and nothing
caught this one, because HTML has no compiler in this build.

Node is optional: if it is not installed the check SKIPS with a warning rather than
failing, so a machine without node can still build. It is present on the usual build
machine, which is where releases are cut.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HTML = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "main", "net", "www", "index.html")


def main():
    if not os.path.isfile(HTML):
        print("check_web: SKIP - %s not found" % HTML)
        return 0

    node = shutil.which("node")
    if not node:
        print("check_web: SKIP - node not installed, cannot syntax-check the web UI")
        return 0

    with open(HTML, "r", encoding="utf-8", errors="replace") as fh:
        html = fh.read()

    blocks = re.findall(r"<script[^>]*>(.*?)</script>", html, re.S)
    if not blocks:
        print("check_web: SKIP - no inline <script> found")
        return 0

    # Joined with a statement separator so one block cannot mask another's error.
    js = "\n;\n".join(blocks)

    tmp = tempfile.NamedTemporaryFile("w", suffix=".js", delete=False,
                                      encoding="utf-8")
    try:
        tmp.write(js)
        tmp.close()
        res = subprocess.run([node, "--check", tmp.name],
                             capture_output=True, text=True)
    finally:
        try:
            os.unlink(tmp.name)
        except OSError:
            pass

    if res.returncode != 0:
        err = (res.stderr or "").strip().splitlines()
        print("")
        print("=" * 78)
        print("check_web: THE WEB UI's JAVASCRIPT DOES NOT PARSE - refusing to build")
        print("=" * 78)
        for line in err[:12]:
            print("  " + line)
        print("")
        print("  A single broken literal stops the WHOLE page script running: the")
        print("  controls draw, nothing updates, no button works, and the corner")
        print("  reads 'disconnected'. That is exactly how v1.8.5 shipped.")
        print("")
        print("  Line numbers above are into the CONCATENATED <script> blocks, not")
        print("  into index.html - search for the surrounding text instead.")
        print("")
        return 1

    print("check_web: web UI JavaScript parses (%d chars)" % len(js))
    return 0


if __name__ == "__main__":
    sys.exit(main())
