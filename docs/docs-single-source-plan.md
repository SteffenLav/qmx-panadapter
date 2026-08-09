# One source of truth for the documentation

Written 2026-08-09, the night v1.7.0 shipped, after an audit that found eleven
factually wrong statements and fourteen expired version markers live on the
website, in the PDF, and compiled into the on-device manual.

This is a proposal. Nothing here is implemented beyond `tools/check_docs.py`.

---

## What we have now: three outputs, two sources

| Output | Built from | Who reads it |
|---|---|---|
| **README.md** on GitHub | itself | anyone landing on the repo |
| **User Guide PDF** | `README.md` (between the `USERGUIDE` markers) **+ 4 guide files** | people who want one printable file |
| **tab5.lav.dk** | `docs/mkdocs/**` (19 pages) | anyone searching for an answer |
| **On-device manual** | `docs/mkdocs/**`, packed into `main/manual.bin` | the operator in the field, offline |

Only four files are shared by two outputs: `guide/panadapter.md`, `ft8-rx.md`,
`ft8-tx.md` and `time-sync.md`, which the PDF builder injects into the matching
README chapter. **Everything else exists twice**, and the two copies drift.

`tools/check_docs.py` measures the overlap. As of this writing: **19 headings
describe the same topic in both trees**, and **2 pages** on the website reach the
PDF by no route at all.

## Why this produced wrong documentation rather than merely duplicated

Every single error found in the audit was in `docs/mkdocs/**`. In every conflict,
**README was right and the guide file was wrong** — the tune-snap grid, the x1
span, the S-meter, the zoom presets, the frequency keypad, the decode
architecture, the drawer contents.

That is not a coincidence and it is not about who wrote what. README is the file
that gets read: it is the repo's front page, it is where release notes get
drafted from, and its author reads it. The mkdocs pages were written early,
render on a website nobody visits to re-read what they already know, and are
compiled into a manual whose whole selling point is that you do not have to go
looking for it. **Nothing was ever going to catch a stale sentence in there.**

So the objective is not "less typing". It is: make it impossible for a fact to
exist in one place and be contradicted in another.

## The two directions, and which one actually works

### Direction A — README is the source; the mkdocs pages are generated from it

This is the intuitive one, and it is the wrong way round. It fails on three
concrete requirements:

1. **The on-device manual needs per-page files with stable headings.** Context
   help jumps to a *heading* (`ui/help_topics.c`), and `pack_manual.py` fails the
   build when one rots. The A-Z index has 265 anchors. A single flat README
   cannot express the page boundaries those depend on without inventing markers
   that are themselves a second source of structure.
2. **The site uses things GitHub cannot render.** Admonitions (`!!! note`),
   content tabs (`=== "Windows"`) and — most importantly — the
   ` ```qmxdiagram ` fences, which the Tab5 renders with LVGL and
   `tools/diagram_svg.py` renders to SVG for the web. Putting those in README
   makes README worse for the people it is for.
3. **The nav order is a fact about the manual**, not about README's narrative
   order, and `mkdocs.yml` already owns it.

### Direction B — the mkdocs pages are the source; README.md is generated  ✅

`README.md` becomes a build artifact — still committed, still full length, still
the repo's front page — assembled by a new `tools/build_readme.py` from the
mkdocs pages plus a small set of README-only fragments.

- One place to edit: `docs/mkdocs/**`.
- The generator translates on the way out: `!!! note` → blockquote,
  ` ```qmxdiagram ` → the SVG that `diagram_svg.py` already produces, content
  tabs → sequential subsections.
- The PDF keeps building from README, so it inherits everything automatically —
  and both PDF gaps close as a side effect.

**The cost, stated honestly:** you would stop editing the file you currently
write in, and start editing pages. That is a real change to how you work, and it
is the reason this is a proposal rather than a commit.

### Direction C — the cheap half of B, if B is too much

Keep both trees, but let **only one of them describe any given thing**. Where
README and a page overlap, pick the better text (in practice: README's), move it
into the page, and leave a one-line pointer behind. `check_docs.py` already
prints the exact list — 19 items — and would count down to zero as they are
resolved. No new tooling, no workflow change, and it can be done a few at a time.

## Recommendation

**Direction C now, Direction B when there is an appetite for it.** C removes the
drift surface with no new machinery and no change to how you write; B is the
permanent answer but should be a deliberate decision made rested, not a change
made overnight to a project's public face.

Either way `check_docs.py` stays: it is what turns "the docs drifted again" from
something a user finds into something the build says.

## What is already done

- `tools/check_docs.py`, run from the mkdocs hook on every docs build.
  Forward-looking phrasing (`"in the next release"`) is an **error** and fails
  the build — verified by planting one. Duplicate coverage and PDF gaps are
  warnings, and the counts are the backlog above.
- The eleven wrong facts are corrected, and the five v1.7.0 features that existed
  only on the website now have README sections, so the PDF carries them.
