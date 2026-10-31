#!/bin/sh
# Fails if the build left the work tree it runs in dirty, or an ELF carries git describe's '-dirty' stamp
# (WARTHOG_VERSION from inject_version.py, esp_app_desc.version from IDF's PROJECT_VER).
#
# Usage: scripts/ci/check-clean-tree.sh [ELF...]
#   Run from the checkout (CI: the repo root). Exit 0 ok, 1 failed, 2 outside a git work tree, git status
#   failing, an empty ELF argument, or a named ELF missing, unreadable or of zero bytes.

set -u
export LC_ALL=C

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "error: $(pwd) is not in a git work tree" >&2; exit 2; }
for elf in "$@"; do
  [ -n "$elf" ] || { echo "error: an empty ELF argument" >&2; exit 2; }
  [ -f "$elf" ] && [ -r "$elf" ] && [ -s "$elf" ] || { echo "error: no readable, non-empty $elf" >&2; exit 2; }
done

fail=0
st=$(git status --porcelain=v1 --untracked-files=all) || { echo "error: git status failed" >&2; exit 2; }
if [ -n "$st" ]; then
  printf '%s\n' "$st" | sed 's/^/FAIL git status: /'
  fail=1
else
  echo "ok   git status --porcelain is empty"
fi

for elf in "$@"; do
  # vX.Y.Z[-rcN][-N-gSHA]-dirty, or SHA-dirty from a clone without tags. grep: 0 found, 1 none, 2 error.
  stamps=$(grep -a -o -E -e '(v[0-9][[:alnum:]._+~-]*|[0-9a-f]{7,40})-dirty' "$elf")
  rc=$?
  [ "$rc" -le 1 ] || { echo "error: grep could not read $elf (rc=$rc)" >&2; exit 2; }
  stamp=$(printf '%s\n' "$stamps" | head -n 1)
  if [ -n "$stamp" ]; then
    echo "FAIL $elf carries a dirty-tree stamp ($stamp)"
    fail=1
  else
    echo "ok   $elf has no -dirty stamp"
  fi
done

if [ "$fail" = 0 ]; then echo "CLEAN TREE OK"; else echo "CLEAN TREE FAILED"; fi
exit "$fail"
