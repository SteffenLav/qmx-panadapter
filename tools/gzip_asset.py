#!/usr/bin/env python3
"""Gzip a build asset so the web server can ship it with Content-Encoding: gzip.

Run from CMake at build time, NOT committed as a derived file. That is
deliberate: `main/manual.bin` is a committed derived artifact and it shipped
STALE in v1.8.8 because the regeneration step ran after the tag. A generated
file cannot fall out of step with its source, so anything that can be produced
during the build should be.

mtime is forced to 0 so the output is byte-identical for identical input -
otherwise every build would produce a different binary and obscure real diffs.
"""
import gzip
import os
import shutil
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gzip_asset.py <infile> <outfile>", file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]

    with open(src, "rb") as f:
        raw = f.read()

    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    tmp = dst + ".tmp"
    # mtime=0 for reproducibility; level 9 because this runs once per build and
    # every byte saved is ~0.4 s off a page load on this board's WiFi link.
    with open(tmp, "wb") as f:
        with gzip.GzipFile(fileobj=f, mode="wb", compresslevel=9, mtime=0) as gz:
            gz.write(raw)
    shutil.move(tmp, dst)

    out = os.path.getsize(dst)
    print(f"gzip_asset: {os.path.basename(src)} {len(raw):,} -> {out:,} bytes "
          f"({len(raw)/out:.1f}x)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
