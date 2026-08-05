#!/usr/bin/env python3
"""Pack the user manual into a single blob that gets embedded in the firmware.

WHY
    The manual used to be downloaded over WiFi and optionally mirrored to the SD
    card. Both are unreliable on this board: SD writes only work before WiFi
    starts (see main/storage/sd_archive.c), and the docs host rate-limits bulk
    downloads. Embedding the manual in the app binary makes it ALWAYS available -
    WiFi on or off, card or no card, first boot out of the box - and it can never
    be out of step with the firmware, because it ships with it.

    Cost is ~140 KB in an 8 MB app partition with ~5.5 MB free, and it uses no
    SPIFFS at all, leaving that entirely to the QSO log and diagnostics.

OWNERSHIP
    The chapter order comes from mkdocs.yml's `nav:` via mkdocs_reader_export.py,
    which is the single source of truth. That hook calls this script during
    `mkdocs build`, so main/manual.bin is refreshed whenever the docs are
    published, and the firmware build just embeds the committed blob with no
    Python/mkdocs dependency of its own.

    --nav-from-mkdocs exists ONLY to bootstrap the blob without mkdocs installed.
    It understands just enough of this project's nav layout; the hook path is
    authoritative.

USAGE
    python tools/pack_manual.py --docs docs/mkdocs --toc site/md/toc.json \
                                --out main/manual.bin
    python tools/pack_manual.py --docs docs/mkdocs --nav-from-mkdocs mkdocs.yml \
                                --out main/manual.bin

FORMAT (little-endian, matches the ESP32; parsed by main/net/manual_embed.c)
    0   char magic[8]   "QMXMAN\0\0"
    8   u32  version    1
    12  u32  count
    16  count * 72-byte records:  char path[64] (NUL-padded), u32 off, u32 len
    ... payload (each entry's bytes at its own absolute offset)
"""

import argparse
import json
import os
import re
import struct
import sys

MAGIC = b"QMXMAN\0\0"
VERSION = 1
PATH_MAX = 64
REC_SIZE = PATH_MAX + 8


def nav_from_mkdocs(path):
    """Bootstrap-only minimal nav reader. See the module docstring: the mkdocs
    hook owns this for real. Handles `- Title: page.md` and one nested level."""
    pages, in_nav, level = [], False, 0
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if re.match(r"^nav:\s*$", line):
                in_nav = True
                continue
            if in_nav and re.match(r"^\S", line):
                break                                    # next top-level key
            if not in_nav or not line.strip():
                continue
            indent = len(line) - len(line.lstrip())
            m = re.match(r"^\s*-\s*(.+?):\s*(\S+\.md)\s*$", line)
            if m:
                pages.append({"title": m.group(1).strip(),
                              "path": m.group(2).strip(),
                              "level": 0 if indent <= 2 else 1})
                continue
            m = re.match(r"^\s*-\s*(.+?):\s*$", line)     # section header
            if m:
                pages.append({"title": m.group(1).strip(), "path": None, "level": 0})
    return pages


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--docs", required=True, help="mkdocs docs_dir (markdown root)")
    ap.add_argument("--toc", help="authoritative toc.json (from the mkdocs hook)")
    ap.add_argument("--nav-from-mkdocs", help="bootstrap only: read nav from mkdocs.yml")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    if a.toc:
        with open(a.toc, encoding="utf-8") as f:
            pages = json.load(f).get("pages", [])
        toc_bytes = json.dumps({"pages": pages}, indent=2).encode("utf-8")
    elif a.nav_from_mkdocs:
        pages = nav_from_mkdocs(a.nav_from_mkdocs)
        toc_bytes = json.dumps({"pages": pages}, indent=2).encode("utf-8")
    else:
        sys.exit("need --toc or --nav-from-mkdocs")

    # toc.json first so the reader can always find it, then every real page.
    entries = [("toc.json", toc_bytes)]
    missing = []
    for p in pages:
        rel = p.get("path")
        if not rel:
            continue                                     # section header
        src = os.path.join(a.docs, rel)
        if not os.path.isfile(src):
            missing.append(rel)
            continue
        with open(src, "rb") as f:
            entries.append((rel, f.read()))

    if missing:
        sys.exit("missing pages listed in the contents: %s" % ", ".join(missing))
    for rel, _ in entries:
        if len(rel.encode("utf-8")) >= PATH_MAX:
            sys.exit("path too long for the %d-byte field: %s" % (PATH_MAX, rel))

    header = len(MAGIC) + 4 + 4
    index_size = REC_SIZE * len(entries)
    off = header + index_size

    index, payload = b"", b""
    for rel, data in entries:
        index += rel.encode("utf-8").ljust(PATH_MAX, b"\0")
        index += struct.pack("<II", off, len(data))
        payload += data
        off += len(data)

    verify_help_topics(entries)

    blob = MAGIC + struct.pack("<II", VERSION, len(entries)) + index + payload
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(blob)

    print("packed %d entries (%d pages + toc) -> %s (%d bytes, %.1f KB)"
          % (len(entries), len(entries) - 1, a.out, len(blob), len(blob) / 1024.0))


def verify_help_topics(entries):
    """Check every context-help deep link in main/ui/help_topics.c still resolves.

    Deep links rot silently: a heading gets reworded, and three releases later half
    the "help me with this" buttons quietly land on the top of a long page while
    nobody notices. Since the manual is packed here from the same markdown the site
    uses, this is the one place that can prove the links are still good - so a
    renamed heading breaks the BUILD instead of the feature.

    Deliberately lenient about finding the table: if help_topics.c is absent or
    unparseable the check is skipped with a warning rather than failing a build for
    an unrelated reason. It only fails on a link it has positively shown to be dead.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(here, "..", "main", "ui", "help_topics.c")
    if not os.path.isfile(src):
        print("pack_manual: no help_topics.c - skipping deep-link check")
        return

    with open(src, "r", encoding="utf-8") as f:
        text = f.read()

    # { HELP_X, "page.md", "anchor", "label" }  - anchor may be empty
    rows = re.findall(
        r'\{\s*(HELP_[A-Z0-9_]+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}',
        text)
    if not rows:
        print("pack_manual: no help topics parsed - skipping deep-link check")
        return

    pages = {rel: data.decode("utf-8", "replace") for rel, data in entries}
    problems = []
    for topic, page, anchor, label in rows:
        if page not in pages:
            problems.append("%s (%s): page '%s' is not in the manual" % (topic, label, page))
            continue
        if not anchor:
            continue
        # Same rule the firmware applies: case-insensitive substring of a heading.
        headings = [ln.lstrip("#").strip()
                    for ln in pages[page].splitlines() if ln.lstrip().startswith("#")]
        if not any(anchor.lower() in h.lower() for h in headings):
            problems.append("%s (%s): no heading in '%s' contains '%s'"
                            % (topic, label, page, anchor))

    if problems:
        sys.exit("pack_manual: context-help deep links are broken:\n  - "
                 + "\n  - ".join(problems))
    print("pack_manual: %d context-help deep link(s) verified" % len(rows))


if __name__ == "__main__":
    main()
