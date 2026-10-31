#!/bin/sh
# Prints the resolved dependencies.lock and fails unless it is what the manifest's == pins and the
# registry hashes below say. The lock stays untracked: it records morsemicro/halow's absolute path.
#
# Usage: scripts/ci/check-lock.sh
#   LOCK (default: dependencies.lock in the repo) is the lock read. Exit 0 ok, 1 failed, 2 no lock.

set -u
unset CDPATH

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
LOCK="${LOCK:-$ROOT/dependencies.lock}"
MANIFEST="$ROOT/main/idf_component.yml"
# name version component_hash, from the Espressif registry; a pin bump updates this too.
HASHES='espressif/esp_tinyusb 2.2.1 9a73a76a6bc17907f6e523342ff6b1e440023e8aac2c11f4691212106ec24e66
espressif/tinyusb 0.21.0~2 16c8d774dab1f484d3e0c0f61105624d2424bbae864efd19818baf7b451b41e7
morsemicro/firmware 2.10.4-esp32-1 a5758df1db4c7c39a0f6e29012355791568ef62146c796dd519b91b72eb9fd73'

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

want_path="$ROOT/components/halow"
if [ "$(field morsemicro/halow 4)" = local ] && [ "$(field morsemicro/halow 5)" = "$want_path" ]; then
  ok "morsemicro/halow is local at $want_path"
else
  bad "morsemicro/halow is $(field morsemicro/halow 4) at $(field morsemicro/halow 5), want local at $want_path"
fi

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
