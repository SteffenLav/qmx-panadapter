#!/usr/bin/env bash
# Fail if any tracked text file carries a NUL byte.
#
# Scripted edits through a Python heredoc keep turning a C '\0' literal into a
# REAL NUL byte in the source. GCC accepts it, so it reaches a commit unnoticed
# (it has, twice). Run this before committing; it exits non-zero on a hit.
fail=0
for f in $(git diff --cached --name-only --diff-filter=ACM; git diff --name-only); do
  case "$f" in *.bin|*.png|*.bmp|*.pdf|*.zip|*.ico) continue;; esac
  [ -f "$f" ] || continue
  n=$(LC_ALL=C tr -dc '\000' < "$f" | wc -c)
  if [ "$n" != "0" ]; then echo "NUL byte(s) in $f: $n"; fail=1; fi
done
[ "$fail" = "0" ] && echo "no NUL bytes"
exit $fail
