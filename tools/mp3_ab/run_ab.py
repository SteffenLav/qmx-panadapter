#!/usr/bin/env python3
"""Guard the vendored minimp3 header: it must decode identically to upstream.

WHY THIS EXISTS. components/minimp3/minimp3.h carries ONE local modification -
mp3dec_decode_frame()'s scratch is static instead of a stack local, so the
16,256 B can go to PSRAM .bss instead of a 12,288 B task stack it would not
fit in. Only a storage class changed, so the decoded PCM must be
BIT-IDENTICAL to pristine upstream. Any difference at all is the
scratch-reuse fault the change could in principle introduce.

⛔ A WEAKER CHECK WOULD HAVE PASSED ON NOTHING. minimp3's own suite compares
against the reference .pcm by PSNR (">96db"), not bytes, because a float
decoder need not match ISO exactly - so reference comparison cannot settle
this. The build-to-build comparison can, and does.

It also checks the decoder RAN: 15 empty output files also compare equal.
Every vector must produce frames and samples, and the counts are printed.

The scfsi vectors l3-si, l3-si_block and l3-si_huff are in the set on purpose
- they exercise the ist_pos reuse path the per-call memset guards, which is
the actual hazard.

Run it after ANY re-vendor of minimp3.h, and after touching the hoist.

    python tools/mp3_ab/run_ab.py

Needs gcc on PATH and network access on the first run (it fetches pristine
upstream and the conformance vectors into a cache). Nothing is written into
the repo.
"""

import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VENDORED = REPO / "components" / "minimp3" / "minimp3.h"
HARNESS = Path(__file__).resolve().parent / "minimp3_ab.c"

RAW = "https://raw.githubusercontent.com/lieff/minimp3/master"

# The Layer III set. M2L3_* are MPEG-2 LSF; l3-si* are the scfsi vectors that
# exercise the ist_pos reuse path, which is why they are not optional.
VECTORS = [
    "M2L3_bitrate_24_all", "M2L3_compl24", "M2L3_noise",
    "l3-compl", "l3-he_32khz", "l3-he_44khz", "l3-he_48khz", "l3-he_mode",
    "l3-hecommon", "l3-si", "l3-si_block", "l3-si_huff", "l3-sin1k0db",
    "l3-test45", "l3-test46",
]


def cache_dir() -> Path:
    base = os.environ.get("TEMP") or os.environ.get("TMPDIR") or "/tmp"
    d = Path(base) / "qmx-minimp3-ab"
    d.mkdir(parents=True, exist_ok=True)
    return d


def fetch(url: str, dest: Path) -> None:
    if dest.exists() and dest.stat().st_size > 0:
        return
    print(f"  fetch {dest.name}")
    with urllib.request.urlopen(url, timeout=60) as r, open(dest, "wb") as f:
        shutil.copyfileobj(r, f)


def build(header_dir: Path, out: Path, workdir: Path) -> None:
    """Compile the harness against one header.

    ⛔ Distinct object/exe names per build. gcc names a -fstack-usage .su
    after the SOURCE file, so two builds of the same .c fight over one name
    and a stale .su reads as a result - that method error cost a wrong
    measurement on 2026-11-18.
    """
    cmd = ["gcc", "-O2", "-I", str(header_dir), "-o", str(out), str(HARNESS), "-lm"]
    subprocess.run(cmd, check=True, cwd=workdir)


def main() -> int:
    if not VENDORED.exists():
        print(f"FAIL: {VENDORED} not found")
        return 1
    if shutil.which("gcc") is None:
        print("FAIL: gcc not on PATH")
        return 1

    cache = cache_dir()
    up_dir = cache / "upstream"
    up_dir.mkdir(exist_ok=True)
    vec_dir = cache / "vectors"
    vec_dir.mkdir(exist_ok=True)

    print(f"cache: {cache}")
    fetch(f"{RAW}/minimp3.h", up_dir / "minimp3.h")
    for name in VECTORS:
        fetch(f"{RAW}/vectors/{name}.bit", vec_dir / f"{name}.bit")

    # The vendored header must differ from upstream in the hoist and nothing
    # else. Report the line count so a silent third change is visible.
    diff = subprocess.run(
        ["git", "diff", "--no-index", "--stat",
         str(up_dir / "minimp3.h"), str(VENDORED)],
        capture_output=True, text=True, cwd=REPO)
    print("diff vs upstream:", (diff.stdout.strip().splitlines() or ["(none)"])[-1])

    ven_dir = cache / "vendored"
    ven_dir.mkdir(exist_ok=True)
    shutil.copy2(VENDORED, ven_dir / "minimp3.h")

    print("building harness against both headers")
    up_exe = cache / "ab_upstream.exe"
    ven_exe = cache / "ab_vendored.exe"
    build(up_dir, up_exe, cache)
    build(ven_dir, ven_exe, cache)

    out_up = cache / "out_upstream"
    out_ven = cache / "out_vendored"
    out_up.mkdir(exist_ok=True)
    out_ven.mkdir(exist_ok=True)

    bad = []
    empty = []
    for name in VECTORS:
        bit = vec_dir / f"{name}.bit"
        a = out_up / f"{name}.pcm"
        b = out_ven / f"{name}.pcm"
        ra = subprocess.run([str(up_exe), str(bit), str(a)],
                            capture_output=True, text=True)
        rb = subprocess.run([str(ven_exe), str(bit), str(b)],
                            capture_output=True, text=True)
        if ra.returncode or rb.returncode:
            bad.append((name, "harness exited non-zero"))
            continue
        size = a.stat().st_size
        if size == 0:
            empty.append(name)
        same = (size == b.stat().st_size and a.read_bytes() == b.read_bytes())
        print(f"  {name:28s} {size:9d} B  {'identical' if same else 'DIFFER'}"
              f"   {rb.stdout.strip().split(' ', 1)[-1] if rb.stdout else ''}")
        if not same:
            bad.append((name, "PCM differs"))

    print()
    if empty:
        print(f"FAIL: decoded nothing for {', '.join(empty)} - "
              f"empty files compare equal, so this is not a pass")
        return 1
    if bad:
        for name, why in bad:
            print(f"FAIL: {name}: {why}")
        return 1
    print(f"PASS: {len(VECTORS)}/{len(VECTORS)} vectors bit-identical to "
          f"pristine upstream minimp3")
    return 0


if __name__ == "__main__":
    sys.exit(main())
