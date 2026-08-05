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
