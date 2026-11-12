#!/bin/sh
# Prints the resolved dependencies.lock and fails unless it is what the manifest says: every override_path
# dependency local at that path with the manifest's version, any other one at its == pin with the registry
# hash below. The lock stays untracked: it records the vendored components' absolute paths.
#
# Usage: scripts/ci/check-lock.sh
#   LOCK (default: dependencies.lock in the repo) is the lock read. Exit 0 ok, 1 failed, 2 no lock.

set -u
unset CDPATH

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
LOCK="${LOCK:-$ROOT/dependencies.lock}"
MANIFEST="$ROOT/main/idf_component.yml"
# name version component_hash per line, from the Espressif registry; a pin bump updates this too. Empty: both
# dependencies are vendored (espressif/esp_tinyusb since 2026-11-11, docs/test-sweep-2026-11-10.md H6; the
# registry's 3.0.0 was 0ff29f6113c4922691b3d7eee2f29af5fb98b799ef8b198a2956328f7c2296a9), so a component
# with a registry hash in the lock is one the build fetched instead of the copy in components/.
HASHES=''

[ -f "$LOCK" ] || { echo "error: no lock: run a pio build first ($LOCK)" >&2; exit 2; }
[ -f "$MANIFEST" ] || { echo "error: no $MANIFEST" >&2; exit 2; }
echo "--- dependencies.lock ---"
cat "$LOCK"
echo "--- end ---"

fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

if ! git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  bad "$ROOT is not a git work tree"
elif git -C "$ROOT" ls-files --error-unmatch dependencies.lock >/dev/null 2>&1; then
  bad "dependencies.lock is tracked; it records this checkout's absolute path"
elif ! git -C "$ROOT" check-ignore -q dependencies.lock; then
  bad "dependencies.lock is not ignored by git"
else
  ok "dependencies.lock is untracked and ignored"
fi

# Lock components as "name version hash type path" ("-" for none); the YAML has a fixed 2-space indent.
comps=$(awk '
  function flush() { if (name != "") print name, ver, hash, type, path; name = ""; ver = hash = type = path = "-"; src = 0 }
  function val(s) { sub(/^[^:]*:[ ]*/, "", s); gsub(/^'\''|'\''$/, "", s); gsub(/^"|"$/, "", s); return s }
  /^[^ ]/ { flush(); top = $0; next }
  top == "dependencies:" && /^  [^ ].*:$/ { flush(); name = substr($0, 3, length($0) - 3); next }
  name == "" { next }
  /^    version: / { ver = val($0) }
  /^    component_hash: / { hash = val($0) }
  /^      type: / && src { type = val($0) }
  /^      path: / && src { path = val($0) }
  /^    [^ ]/ { src = ($0 == "    source:") }
  END { flush() }' "$LOCK")
direct=$(awk '/^[^ -]/ {on = ($0 == "direct_dependencies:"); next} on && /^- / {print substr($0, 3)}' "$LOCK" | sort | tr '\n' ' ')
field() { printf '%s\n' "$comps" | awk -v n="$1" -v f="$2" '$1 == n {print $f; exit}'; }

# Every manifest dependency's name: the lock's direct_dependencies.
declared=$(awk '/^[^ #]/ {on = ($0 == "dependencies:"); next} on && /^  [^ #]/ {n = $0; sub(/^  /, "", n); sub(/:.*$/, "", n); print n}' \
  "$MANIFEST" | sort | tr '\n' ' ')
# Manifest dependencies as "name spec", minus idf and the override_path one.
pins=$(awk '
  function flush() { if (name != "" && name != "idf" && !over) print name, (spec == "" ? "-" : spec); name = ""; spec = ""; over = 0 }
  function val(s) { sub(/^[^:]*:[ ]*/, "", s); sub(/[ ]+#.*$/, "", s); gsub(/^'\''|'\''$/, "", s); gsub(/^"|"$/, "", s); return s }
  /^[^ #]/ { flush(); on = ($0 == "dependencies:"); next }
  !on || /^ *#/ || /^ *$/ { next }
  /^  [^ ]/ { flush(); name = $0; sub(/^  /, "", name); sub(/:.*$/, "", name); spec = val($0); next }
  /^    version:/ { spec = val($0) }
  /^    override_path:/ { over = 1 }
  END { flush() }' "$MANIFEST")
# The override_path dependencies as "name spec path", the path as written (relative to main/).
locals=$(awk '
  function flush() { if (name != "" && over != "") print name, (spec == "" ? "-" : spec), over; name = ""; spec = ""; over = "" }
  function val(s) { sub(/^[^:]*:[ ]*/, "", s); sub(/[ ]+#.*$/, "", s); gsub(/^'\''|'\''$/, "", s); gsub(/^"|"$/, "", s); return s }
  /^[^ #]/ { flush(); on = ($0 == "dependencies:"); next }
  !on || /^ *#/ || /^ *$/ { next }
  /^  [^ ]/ { flush(); name = $0; sub(/^  /, "", name); sub(/:.*$/, "", name); spec = val($0); next }
  /^    version:/ { spec = val($0) }
  /^    override_path:/ { over = val($0) }
  END { flush() }' "$MANIFEST")

while read -r n spec; do
  [ -n "$n" ] || continue
  got=$(field "$n" 2)
  case "$spec" in
    ==*) if [ "${spec#==}" = "$got" ]; then ok "$n $got (manifest $spec)"
         else bad "$n resolves to ${got:-nothing}, the manifest pins $spec"; fi ;;
    *) bad "$n has no == pin (manifest: $spec)" ;;
  esac
done <<EOF
$pins
EOF

while read -r n v h; do
  [ -n "$n" ] || continue
  spec=$(printf '%s\n' "$pins" | awk -v n="$n" '$1 == n {print $2}')
  case "$spec" in "==$v") ;; ==*) bad "the hash table has $n $v, the manifest pins $spec: update both" ;;
    '') bad "the hash table has $n $v, the manifest does not depend on it" ;; esac
  got=$(field "$n" 2) gh=$(field "$n" 3)
  if [ "$got" = "$v" ] && [ "$gh" = "$h" ]; then ok "$n $v component_hash $(echo "$h" | cut -c1-12)"
  else bad "$n is ${got:-absent} with component_hash ${gh:-none}, the table has $v $h"; fi
done <<EOF
$HASHES
EOF
for n in $(printf '%s\n' "$comps" | awk '$3 != "-" {print $1}'); do
  printf '%s\n' "$HASHES" | grep -q "^$n " || bad "$n $(field "$n" 2) has component_hash $(field "$n" 3), which the table lacks"
done

# Both vendored components, by name: dropping an override_path fetches the registry's build (for
# espressif/esp_tinyusb, the Buffer DMA driver of H6).
for n in morsemicro/halow espressif/esp_tinyusb; do
  printf '%s\n' "$locals" | grep -q "^$n " || bad "$n has no override_path in main/idf_component.yml"
done
while read -r n spec rel; do
  [ -n "$n" ] || continue
  want_path=$(cd "$ROOT/main" 2>/dev/null && cd "$rel" 2>/dev/null && pwd -P)
  if [ -n "$want_path" ] && [ "$(field "$n" 4)" = local ] && [ "$(field "$n" 5)" = "$want_path" ] && [ "$(field "$n" 3)" = - ]; then
    ok "$n is local at $want_path"
  else
    bad "$n is $(field "$n" 4) at $(field "$n" 5) (component_hash $(field "$n" 3)), want local at ${want_path:-main/$rel, which is missing}"
  fi
  if [ "${spec#==}" = "$(field "$n" 2)" ]; then ok "$n $(field "$n" 2) (manifest version $spec)"
  else bad "$n is $(field "$n" 2) in the lock, the manifest says $spec"; fi
done <<EOF
$locals
EOF

if [ -n "$declared" ] && [ "$direct" = "$declared" ]; then
  ok "direct_dependencies: ${direct% } (main/idf_component.yml's)"
else
  bad "direct_dependencies are '${direct% }', main/idf_component.yml declares '${declared% }'"
fi

idf=$(field idf 2)
if printf '%s\n' "$idf" | awk -F. '$1 == 5 && ($2 + 0 > 5 || ($2 == 5 && $3 + 0 >= 4)) {y = 1} END {exit !y}'; then
  ok "idf $idf"
else
  bad "idf is ${idf:-absent}, want 5.x at or above 5.5.4"
fi

if [ "$fail" = 0 ]; then echo "LOCK CHECK OK"; else echo "LOCK CHECK FAILED"; fi
exit "$fail"
