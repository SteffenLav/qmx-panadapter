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


def clean_heading(text):
    """Reduce a markdown heading to the plain words the reader will display.

    The reader finds an anchor by case-insensitive SUBSTRING match against the
    heading as it renders it, so whatever we emit here has to survive that same
    cleaning - inline code, emphasis and links all stripped, links reduced to
    their visible text."""
    t = text.strip()
    t = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", t)      # [text](url) -> text
    t = re.sub(r"[`*_]+", "", t)                        # code / emphasis markers
    # Deliberately NOT stripping <...>: reader_view.c's md_inline_clean does not,
    # and headings like "POST /api/adif/delete?idx=<n>" carry angle brackets as
    # literal text. Removing them here produced an anchor that could never match
    # the heading it came from.
    return re.sub(r"\s+", " ", t).strip()


def anchor_for(term):
    """The part of a term safe to match against the RENDERED heading.

    The reader folds UTF-8 punctuation to ASCII before matching (an em dash
    becomes something else entirely), so any anchor containing non-ASCII would
    silently never match. Anchors are substring matches, so a leading ASCII-only
    prefix is enough - and honest. Returns "" when too little survives to be
    worth aiming at, in which case the index still opens the right page."""
    cut = term
    for i, ch in enumerate(term):
        if ord(ch) > 126:
            cut = term[:i]
            break
    cut = cut.strip().rstrip("-,:;(")
    return cut.strip() if len(cut.strip()) >= 4 else ""


def sort_key(term):
    """Sort on the first real word: '1. Tap to Tune' files under T, not 1.

    Operators look things up by the word they remember, and the numbering in
    these headings is an artefact of the chapter, not part of the term."""
    t = re.sub(r"^[\d.\s]+", "", term).strip()
    return ((t or term).lower(), term.lower())


def build_index(docs_dir, pages):
    """A-Z index of every heading in the manual: [{t: term, p: path, a: anchor}].

    Layer 4 of the context help. Deliberately data, not a generated markdown
    page: the Tab5's reader renders links as plain text (only the Contents
    panel navigates), so an index written as markdown links would look right
    and do nothing."""
    seen, terms = set(), []
    for p in pages:
        rel = p.get("path")
        if not rel:
            continue
        src = os.path.join(docs_dir, rel)
        if not os.path.isfile(src):
            continue
        with open(src, encoding="utf-8") as f:
            body = f.read()
        for _hashes, raw in re.findall(r"^(#{2,3})\s+(.+?)\s*$", body, re.M):
            term = clean_heading(raw)
            # 79 = the reader's anchor buffer minus its NUL. A longer anchor
            # would be truncated there and then fail to match its own heading.
            if not term or len(term.encode("utf-8")) > 79:
                continue
            key = (term.lower(), rel)
            if key in seen:
                continue
            seen.add(key)
            terms.append({"t": term, "p": rel, "a": anchor_for(term)})
    terms.sort(key=lambda e: sort_key(e["t"]))
    return terms


def verify_index(docs_dir, terms):
    """Every index anchor must be findable in the page it points at.

    Same reasoning as verify_help_topics: a rotted index entry is invisible
    until an operator taps it and lands on the wrong place, so it is checked
    where it is cheap to fix - at build time."""
    bad = []
    for e in terms:
        if not e["a"]:
            continue                                   # page-level entry, nothing to aim at
        src = os.path.join(docs_dir, e["p"])
        if not os.path.isfile(src):
            bad.append("%s -> missing page %s" % (e["t"], e["p"]))
            continue
        with open(src, encoding="utf-8") as f:
            headings = [clean_heading(h) for _x, h in
                        re.findall(r"^(#{1,6})\s+(.+?)\s*$", f.read(), re.M)]
        if not any(e["a"].lower() in h.lower() for h in headings):
            bad.append("%s -> no heading in %s contains %r" % (e["t"], e["p"], e["a"]))
    if bad:
        sys.exit("A-Z index anchors do not resolve:\n  " + "\n  ".join(bad))
    print("pack_manual: %d index anchor(s) verified" % sum(1 for e in terms if e["a"]))


DIAGRAM_KEYS = {
    "flow":     {"title", "node", "branch", "note"},
    "stack":    {"title", "row", "note"},
    "timeline": {"title", "span", "seg", "mark", "tick", "note"},
    "panel":    {"title", "row", "buttons", "note"},
}


def verify_diagrams(docs_dir, pages):
    """Check every ```qmxdiagram spec parses before it reaches the device.

    The C renderer degrades to raw text on a bad spec rather than crashing, but
    a diagram that silently renders as a wall of `key: value` lines is exactly
    the sort of rot that went unnoticed in the character drawings for years.
    Fail the build instead."""
    bad, count = [], 0
    for p in pages:
        rel = p.get("path")
        if not rel:
            continue
        src = os.path.join(docs_dir, rel)
        if not os.path.isfile(src):
            continue
        with open(src, encoding="utf-8") as f:
            body = f.read()
        for m in re.finditer(r"```qmxdiagram\n(.*?)```", body, re.S):
            count += 1
            spec = m.group(1)
            where = "%s (diagram %d)" % (rel, count)
            fields = {}
            for raw in spec.splitlines():
                line = raw.strip()
                if not line:
                    continue
                if ":" not in line:
                    bad.append("%s: line is not 'key: value': %r" % (where, line[:50]))
                    continue
                k = line.split(":", 1)[0].strip().lower()
                fields.setdefault(k, []).append(line.split(":", 1)[1].strip())
            kind = (fields.get("type") or [""])[0].lower()
            if kind not in DIAGRAM_KEYS:
                bad.append("%s: unknown or missing type %r" % (where, kind))
                continue
            allowed = DIAGRAM_KEYS[kind] | {"type"}
            for k in fields:
                if k not in allowed:
                    bad.append("%s: %r is not valid in a %s diagram" % (where, k, kind))
            if kind == "timeline":
                if "span" not in fields:
                    bad.append("%s: timeline has no span" % where)
                if "seg" not in fields:
                    bad.append("%s: timeline has no segments" % where)
                else:
                    span = float(fields["span"][0]) if "span" in fields else 0
                    for seg in fields["seg"] + fields.get("mark", []):
                        rng = seg.split()[0]
                        try:
                            a, b = (float(x) for x in rng.split("-", 1))
                        except ValueError:
                            bad.append("%s: segment %r is not <a>-<b>" % (where, rng))
                            continue
                        if b <= a:
                            bad.append("%s: segment %r ends before it starts" % (where, rng))
                        if span and b > span + 1e-6:
                            bad.append("%s: segment %r runs past the %g span" % (where, rng, span))
            if kind == "flow" and "node" not in fields:
                bad.append("%s: flow has no nodes" % where)
            if kind == "panel" and "row" not in fields:
                bad.append("%s: panel has no rows" % where)
            if kind == "stack" and "row" not in fields:
                bad.append("%s: stack has no rows" % where)
    if bad:
        sys.exit("diagram specs did not check out:\n  " + "\n  ".join(bad))
    if count:
        print("pack_manual: %d diagram spec(s) verified" % count)


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

    # toc.json first so the reader can always find it, then the A-Z index, then
    # every real page.
    verify_diagrams(a.docs, pages)
    index_terms = build_index(a.docs, pages)
    verify_index(a.docs, index_terms)
    index_bytes = json.dumps({"terms": index_terms}, indent=1).encode("utf-8")
    entries = [("toc.json", toc_bytes), ("index.json", index_bytes)]
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

    print("packed %d entries (%d pages + toc + index) -> %s (%d bytes, %.1f KB)"
          % (len(entries), len(entries) - 2, a.out, len(blob), len(blob) / 1024.0))
    print("pack_manual: A-Z index has %d terms (%.1f KB)"
          % (len(index_terms), len(index_bytes) / 1024.0))


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
