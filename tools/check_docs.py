#!/usr/bin/env python3
"""Documentation consistency checks, run from the mkdocs hook on every docs build.

WHY THIS EXISTS
---------------
On 2026-08-09, the evening v1.7.0 shipped, a read-through of the PDF turned up a
version marker that looked stale. Chasing it found considerably more:

  * Three flatly WRONG instructions, one of them dangerous - the build guide told
    a reader whose device would not boot to flash the app to 0x0, which is where
    the bootloader lives and is exactly how units were bricked in v0.18.5.
  * The panadapter guide described a snap grid that varied with ZOOM (10 kHz at
    x1); it is and was mode-aware (10 Hz CW ... 1 kHz AM). It also called the x1
    span "4 MHz" - it is 48 kHz - and documented an S-meter peak-hold mode that
    has never existed.
  * FOURTEEN occurrences of "new in the next release", written during v1.6.0
    development and never updated when v1.6.0 shipped. They were live on the
    website, in the PDF, and compiled into the on-device manual.
  * Whole features - Bluetooth mouse, activation logging, SWR protection - were
    documented on the website but absent from the printable guide.

None of it was anyone's decision. It is what happens when the same thing is
described in two trees and only one of them gets read. These checks are the
cheap mechanical part of not repeating it; judgement is still human.

The checks
----------
1. FORWARD-LOOKING TEXT (error). "new in the next release" is true exactly until
   the release, and nothing makes it false again. Ban the phrasing outright.
2. PDF COVERAGE (warning). Every page in the mkdocs nav should reach the
   printable guide somehow - either the PDF builder injects it, or README has a
   section of its own on the subject. A page in neither is invisible to anyone
   reading the PDF.
3. DUPLICATE COVERAGE (warning). Headings that exist in BOTH README and the
   mkdocs tree are the drift surface: two texts, one topic, and no mechanism
   keeping them honest with each other. Fewer is better.

Run standalone for the full report:  python tools/check_docs.py
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
README = os.path.join(REPO, "README.md")
MKDOCS_DIR = os.path.join(REPO, "docs", "mkdocs")
PDF_SCRIPT = os.path.join(REPO, "tools", "build_userguide_pdf.ps1")

# version-history.md and releases.md are CHRONOLOGY: they are supposed to talk
# about past releases, and a "new in" there is a fact about a moment, not a
# claim about the present.
EXEMPT = {"version-history.md", "releases.md"}

# Phrases that are true only until the next release and never become false again.
FORWARD_LOOKING = [
    r"new in the next release",
    r"in the next release",
    r"coming soon",
    r"in a future release",
    r"not yet released",
    r"will be added in",
]


def _md_files():
    """README plus every page under docs/mkdocs, minus the chronologies."""
    out = []
    if os.path.isfile(README):
        out.append(README)
    for root, _dirs, files in os.walk(MKDOCS_DIR):
        for fn in files:
            if fn.endswith(".md") and fn not in EXEMPT:
                out.append(os.path.join(root, fn))
    return out


def _rel(path):
    return os.path.relpath(path, REPO).replace("\\", "/")


def _read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def check_forward_looking():
    """Text that quietly expires the moment a release ships."""
    errors = []
    pat = re.compile("|".join(FORWARD_LOOKING), re.IGNORECASE)
    for path in _md_files():
        for n, line in enumerate(_read(path).splitlines(), 1):
            m = pat.search(line)
            if m:
                errors.append(
                    "%s:%d: %r - name the version, or say nothing. "
                    "This survives the release that makes it false."
                    % (_rel(path), n, m.group(0))
                )
    return errors


def _normalise_heading(text):
    """Reduce a heading to something comparable across two writing styles.

    'Touch Controls', '2. Touch controls' and 'Touch-to-tune' will not all
    collapse together - and should not. The aim is to catch the same topic
    written twice, not to be clever about synonyms.
    """
    t = text.strip().lower()
    t = re.sub(r"^\d+(\.\d+)*\.?\s*", "", t)          # leading section numbers
    t = re.sub(r"\(.*?\)", "", t)                       # parentheticals
    t = re.sub(r"[^a-z0-9]+", " ", t)                   # punctuation, emoji, dashes
    t = re.sub(r"\b(the|a|an|and|to|of|on|in|for|your|with)\b", " ", t)
    return " ".join(t.split())


def _headings(text, levels=(2, 3)):
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m and len(m.group(1)) in levels:
            key = _normalise_heading(m.group(2))
            if key and key not in out:
                out[key] = (n, m.group(2).strip())
    return out


def _pdf_injected_guides():
    """Guide files the PDF builder pulls in, read from the builder itself."""
    if not os.path.isfile(PDF_SCRIPT):
        return set()
    src = _read(PDF_SCRIPT)
    return set(re.findall(r'Join-Path \$guideDir "([a-z0-9-]+\.md)"', src))


def _nav_pages():
    """Pages listed in mkdocs.yml nav - what the website and the device carry."""
    cfg = os.path.join(REPO, "mkdocs.yml")
    if not os.path.isfile(cfg):
        return []
    pages, in_nav = [], False
    for line in _read(cfg).splitlines():
        if re.match(r"^nav:", line):
            in_nav = True
            continue
        if in_nav and re.match(r"^[a-zA-Z_]", line):
            break
        if in_nav:
            for m in re.finditer(r"([A-Za-z0-9_/-]+\.md)", line):
                pages.append(m.group(1))
    return pages


def check_pdf_coverage():
    """Site pages that never reach the printable guide."""
    warnings = []
    injected = _pdf_injected_guides()
    readme_headings = _headings(_read(README), levels=(2, 3)) if os.path.isfile(README) else {}

    for page in _nav_pages():
        base = os.path.basename(page)
        if base in EXEMPT or base == "index.md":
            continue
        if base in injected:
            continue
        path = os.path.join(MKDOCS_DIR, page.replace("/", os.sep))
        if not os.path.isfile(path):
            continue
        # Does README cover this subject anywhere under its own heading?
        #
        # Matched on shared SUBJECT WORDS rather than on the whole title: the
        # two trees name the same thing differently on purpose ("Quick Start"
        # vs "Quick Guide", "Gestures & Controls" vs "Gestures"), and a check
        # that flags those is noise - and a noisy check is one nobody reads,
        # which is how the docs got into this state to begin with.
        title = _headings(_read(path), levels=(1,))
        title_key = list(title.keys())[0] if title else _normalise_heading(base[:-3])
        subject = set(title_key.split()) - GENERIC_WORDS
        if not subject:
            continue
        if any(subject & (set(k.split()) - GENERIC_WORDS) for k in readme_headings):
            continue
        warnings.append(
            "%s is on the website and in the on-device manual, but nothing in "
            "the PDF covers it - it is neither injected by the PDF builder nor "
            "given a README section." % page
        )
    return warnings


def check_duplicate_coverage():
    """Topics described in both trees - where drift happens."""
    if not os.path.isfile(README):
        return []
    readme_h = _headings(_read(README), levels=(3,))
    dupes = []
    for root, _dirs, files in os.walk(MKDOCS_DIR):
        for fn in sorted(files):
            if not fn.endswith(".md") or fn in EXEMPT:
                continue
            path = os.path.join(root, fn)
            for key, (n, raw) in _headings(_read(path), levels=(2, 3)).items():
                if key in readme_h:
                    dupes.append(
                        "%s:%d %r also exists in README (line %d, %r)"
                        % (_rel(path), n, raw, readme_h[key][0], readme_h[key][1])
                    )
    return dupes


def run(verbose=True):
    """Returns (errors, warnings). Errors should fail a docs build."""
    errors = check_forward_looking()
    missing = check_pdf_coverage()
    dupes = check_duplicate_coverage()

    if verbose:
        if errors:
            print("check_docs: FORWARD-LOOKING TEXT (%d):" % len(errors))
            for e in errors:
                print("  " + e)
        if missing:
            print("check_docs: NOT IN THE PDF (%d):" % len(missing))
            for w in missing:
                print("  " + w)
        if dupes:
            print("check_docs: described in BOTH trees (%d) - drift surface:" % len(dupes))
            for d in dupes:
                print("  " + d)
        if not (errors or missing or dupes):
            print("check_docs: clean")

    return errors, missing + dupes


if __name__ == "__main__":
    errs, warns = run()
    print("\ncheck_docs: %d error(s), %d warning(s)" % (len(errs), len(warns)))
    sys.exit(1 if errs else 0)
