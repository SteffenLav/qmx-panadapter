# mkdocs post-build hook for the on-device docs Reader + update check.
#
# Registered from mkdocs.yml (`hooks:`), so it runs automatically as part of
# `mkdocs build` — no new manual step beyond the existing FTP of site/.
#
# It produces three things the Tab5 firmware consumes (see
# docs/reader-page-and-update-check-plan.md):
#
#   site/md/**.md    verbatim copy of the SOURCE markdown tree (docs_dir), so
#                    the device renders the SAME files that build the site — no
#                    second copy to maintain. Served as https://tab5.lav.dk/md/…
#   site/md/toc.json table of contents derived from mkdocs.yml `nav:` (title +
#                    path + level), the single source of truth for page order.
#   site/latest.json { "version", "url", "notes_url" } — the zero-dependency
#                    fallback for the firmware's GitHub update check. Version is
#                    the latest git tag on the build machine.
#
# All best-effort: any failure logs a warning and does NOT fail the docs build.

import json
import logging
import os
import shutil
import subprocess
import sys

log = logging.getLogger("mkdocs.reader_export")

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools"))
try:
    import diagram_svg
except Exception as _e:            # pragma: no cover - the site still builds
    diagram_svg = None
    log.warning("reader_export: diagram_svg unavailable (%s); qmxdiagram fences "
                "will render as raw spec text on the site", _e)


def on_page_markdown(markdown, page=None, config=None, files=None, **kwargs):
    """Turn ```qmxdiagram fences into inline SVG for the website.

    Only the RENDERED site is affected. The .md mirrored to site/md/ and the
    copy packed into manual.bin keep the spec, because that is what the Tab5's
    own renderer reads - one source, two renderers.

    Without this the site would show the spec as a code block, which is worse
    than the character drawings it replaced."""
    if diagram_svg is None or "```qmxdiagram" not in markdown:
        return markdown
    return diagram_svg.substitute(markdown)

REPO = "SteffenLav/qmx-panadapter"


def _copy_md_tree(docs_dir, out_dir):
    count = 0
    for root, _dirs, files in os.walk(docs_dir):
        for name in files:
            if not name.endswith(".md"):
                continue
            src = os.path.join(root, name)
            rel = os.path.relpath(src, docs_dir)
            dst = os.path.join(out_dir, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
            count += 1
    return count


def _flatten_nav(nav, level=0, out=None):
    """Walk the mkdocs nav structure into a flat [{title, path, level}] list.

    nav items are one of:
      "index.md"                        -> page, no explicit title
      {"Home": "index.md"}              -> page with title
      {"User Guide": [ ...children ]}   -> section (title, no path)
    """
    if out is None:
        out = []
    if isinstance(nav, str):
        out.append({"title": "", "path": nav, "level": level})
    elif isinstance(nav, list):
        for item in nav:
            _flatten_nav(item, level, out)
    elif isinstance(nav, dict):
        for title, value in nav.items():
            if isinstance(value, str):
                out.append({"title": title, "path": value, "level": level})
            else:
                # a section header: emit the section, recurse one level deeper
                out.append({"title": title, "path": None, "level": level})
                _flatten_nav(value, level + 1, out)
    return out


def _latest_git_tag():
    try:
        tag = subprocess.check_output(
            ["git", "describe", "--tags", "--abbrev=0"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
        return tag or None
    except Exception as e:  # noqa: BLE001 - best-effort
        log.warning("reader_export: could not read git tag: %s", e)
        return None


def on_post_build(config, **kwargs):
    docs_dir = config["docs_dir"]
    site_dir = config["site_dir"]
    md_out = os.path.join(site_dir, "md")

    # 1) verbatim source markdown -> site/md/
    try:
        os.makedirs(md_out, exist_ok=True)
        n = _copy_md_tree(docs_dir, md_out)
        log.info("reader_export: copied %d markdown files to %s", n, md_out)
    except Exception as e:  # noqa: BLE001
        log.warning("reader_export: md copy failed: %s", e)

    # 2) TOC from nav -> site/md/toc.json
    try:
        toc = _flatten_nav(config.get("nav") or [])
        with open(os.path.join(md_out, "toc.json"), "w", encoding="utf-8") as f:
            json.dump({"pages": toc}, f, indent=2)
        log.info("reader_export: wrote toc.json (%d entries)", len(toc))
    except Exception as e:  # noqa: BLE001
        log.warning("reader_export: toc.json failed: %s", e)

    # 2b) Repack main/manual.bin, the manual that gets built INTO the firmware
    #     (see main/net/manual_embed.h). Done here because this is the one place
    #     that knows the authoritative chapter order from mkdocs' own nav, so the
    #     firmware build needs no mkdocs/PyYAML of its own - it just embeds the
    #     committed blob. Commit main/manual.bin after a docs change, or the
    #     on-device manual stays one edit behind.
    try:
        repo = os.path.dirname(os.path.abspath(__file__))
        packer = os.path.join(repo, "tools", "pack_manual.py")
        out = os.path.join(repo, "main", "manual.bin")
        r = subprocess.run(
            [sys.executable, packer, "--docs", docs_dir,
             "--toc", os.path.join(md_out, "toc.json"), "--out", out],
            capture_output=True, text=True)
        if r.returncode == 0:
            log.info("reader_export: %s", (r.stdout or "").strip())
        else:
            # HARD FAIL, not a warning. Two reasons this must stop the build:
            #
            # 1. main/manual.bin is a FIRMWARE INPUT. A failed pack leaves the
            #    previous blob in place, so the next build silently ships the old
            #    manual - invisible until an operator notices a stale chapter list.
            # 2. The packer verifies the context-help deep links. Letting that
            #    through as a warning defeats the point of checking them at all:
            #    the whole reason the check lives at build time is that rotted
            #    deep links are otherwise found by users, not by us.
            msg = (r.stderr or r.stdout or "").strip()
            log.error("reader_export: pack_manual.py FAILED: %s", msg)
            raise RuntimeError("pack_manual.py failed - manual.bin not regenerated:\n" + msg)
    except FileNotFoundError as e:
        # Genuinely absent tooling: warn, do not break an unrelated docs build.
        log.warning("reader_export: pack_manual.py could not run: %s", e)

    # 3) latest.json (update-check fallback) -> site/latest.json
    try:
        tag = _latest_git_tag()
        if tag:
            data = {
                "version": tag,
                "url": "https://github.com/%s/releases/tag/%s" % (REPO, tag),
                "notes_url": "md/releases.md",
            }
            with open(os.path.join(site_dir, "latest.json"), "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
            log.info("reader_export: wrote latest.json (%s)", tag)
        else:
            log.warning("reader_export: no git tag; left latest.json untouched")
    except Exception as e:  # noqa: BLE001
        log.warning("reader_export: latest.json failed: %s", e)

    # 4) documentation consistency (tools/check_docs.py)
    #
    # Errors RAISE, for the same reason the packer's failure does: a docs build
    # that only warns will be ignored, and the thing being checked here is text
    # that shipped wrong to the website, the PDF and the on-device manual and
    # stayed wrong for releases at a time. Warnings are printed, not fatal -
    # they measure the README/mkdocs overlap, which is a known backlog rather
    # than a regression.
    try:
        # Load by path rather than by name. mkdocs runs hooks with a sys.path
        # that does not reliably keep the entry added at import time, and the
        # broad except below would then turn "the check never ran" into a
        # single warning line - which is exactly how a check quietly stops
        # protecting anything. Verified failing that way before this change.
        import importlib.util  # noqa: PLC0415
        _cd_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "tools", "check_docs.py")
        _spec = importlib.util.spec_from_file_location("check_docs", _cd_path)
        check_docs = importlib.util.module_from_spec(_spec)
        _spec.loader.exec_module(check_docs)
        errors, warnings = check_docs.run(verbose=False)
        for w in warnings:
            log.warning("check_docs: %s", w)
        if errors:
            for e in errors:
                log.error("check_docs: %s", e)
            raise RuntimeError(
                "check_docs found %d documentation error(s) - see above" % len(errors))
        log.info("check_docs: no errors, %d warning(s)", len(warnings))
    except RuntimeError:
        raise
    except Exception as e:  # noqa: BLE001
        log.warning("reader_export: check_docs could not run: %s", e)
