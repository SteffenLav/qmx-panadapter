#!/usr/bin/env python3
"""Syntax-check the web UI that ships inside the firmware.

⛔ WHY THIS EXISTS. main/net/www/index.html is a single self-contained page
compiled into the binary, so a syntax error in it is not caught by the C
compiler, not caught by the linker, and not visible until somebody loads the
page on a device that has already been flashed.

It has gone wrong twice in ways that reached hardware:

  - a backslash escape sent through a heredoc produced a dead page (CLAUDE.md
    records it under the heredoc rule);
  - 2026-09-02: a new CSS rule was inserted ahead of an existing one and the
    line it replaced was re-emitted, leaving two opening selectors stacked with
    no closing brace between them:

        #wspr-actions button, #wspr-actions select {
        #ft8-actions button, #ft8-actions select {

    CSS parsing stops at that point, so EVERY rule after it was discarded and
    the operator got the page as unstyled HTML - default fonts, underlined
    links, no layout. It looked like the page was gone rather than like a
    styling slip.

The JavaScript was being checked by hand with `node --check` at the time. That
could never have caught it: the fault was in the <style> block.

⚠ AND THE FIRST VERSION OF THIS CHECK CRIED WOLF, which is worth keeping in
mind before adding rules to it. It flagged `@media (...) {` followed by a
selector - correct, legitimate nesting - as a stacked selector, and reported
BROKEN on a perfectly good file. A check that is wrong about a healthy file
gets ignored, and then it is worth less than no check at all. At-rules are
excluded for exactly that reason.

Usage:
    python tools/check_webui.py [path]     # default: main/net/www/index.html
Exit code 0 = clean, 1 = a problem worth stopping for.
"""
import io
import os
import re
import subprocess
import sys
import tempfile


def find_blocks(html, tag):
    return re.findall(r'<%s[^>]*>(.*?)</%s>' % (tag, tag), html, re.S)


def strip_comments(css):
    return re.sub(r'/\*.*?\*/', '', css, flags=re.S)


def check_js(html):
    """node --check each <script> block. Skipped (not failed) without node."""
    problems = []
    blocks = [b for b in find_blocks(html, 'script') if b.strip()]
    try:
        subprocess.run(['node', '--version'], capture_output=True, check=True)
    except Exception:
        print("  js : SKIPPED - node not on PATH")
        return problems
    for i, b in enumerate(blocks):
        fd, path = tempfile.mkstemp(suffix='.js')
        os.close(fd)
        io.open(path, 'w', encoding='utf-8', newline='\n').write(b)
        r = subprocess.run(['node', '--check', path], capture_output=True, text=True)
        os.remove(path)
        if r.returncode != 0:
            problems.append("script block %d does not parse:\n%s" % (i, r.stderr.strip()[:600]))
    print("  js : %d block(s), %s" % (len(blocks), "OK" if not problems else "BROKEN"))
    return problems


def check_css(html):
    problems = []
    blocks = find_blocks(html, 'style')
    for i, b in enumerate(blocks):
        n = strip_comments(b)

        depth = 0
        stray = 0
        for ch in n:
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth < 0:
                    stray += 1
                    depth = 0
        if depth:
            problems.append("style block %d: %d unclosed brace(s) - "
                            "rules after the fault are discarded" % (i, depth))
        if stray:
            problems.append("style block %d: %d stray closing brace(s)" % (i, stray))

        # The 2026-09-02 shape: a '{' whose block opens straight into ANOTHER
        # selector. Legitimate for an at-rule (@media/@supports nest blocks);
        # never legitimate for a plain selector.
        for m in re.finditer(r'([^\n{}]*)\{\s*\n\s*([#.\w][^;{}\n]*)\{', n):
            opener = m.group(1).strip()
            if opener.startswith('@'):
                continue                      # @media { .foo { - correct nesting
            problems.append("style block %d: selector %r opens straight into %r "
                            "- a missing closing brace, and every rule after it "
                            "is discarded" % (i, opener[:50], m.group(2).strip()[:50]))
        print("  css: block %d, %d bytes, %s"
              % (i, len(b), "OK" if not problems else "BROKEN"))
    return problems


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'main/net/www/index.html'
    html = io.open(path, encoding='utf-8', errors='replace').read()
    print("check_webui: %s (%d bytes)" % (path, len(html)))
    problems = check_js(html) + check_css(html)
    if problems:
        print("\ncheck_webui: %d PROBLEM(S)" % len(problems))
        for p in problems:
            print("  - " + p)
        return 1
    print("check_webui: clean")
    return 0


if __name__ == '__main__':
    sys.exit(main())
