#!/bin/sh
# Fails unless every CONFIG_ line of sdkconfig.defaults (then sdkconfig.ecm for an -ecm env) landed in the
# generated sdkconfig.<env>: PlatformIO applies the defaults only when it first writes one.
#
# Usage: scripts/ci/check-sdkconfig-landing.sh [ENV...]
#   No ENV: every sdkconfig.warthog-* of an [env:warthog-*] in platformio.ini. SDKCONFIG_DIR (default: the
#   repo root) is where the sdkconfig.<ENV> files are. Exit 0 ok, 1 failed, 2 nothing to check, an empty or
#   malformed ENV, or an unreadable input.

set -u
unset CDPATH

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
DIR="${SDKCONFIG_DIR:-$ROOT}"
DEF="$ROOT/sdkconfig.defaults"
ECM="$ROOT/sdkconfig.ecm"
INI="$ROOT/platformio.ini"
for f in "$DEF" "$ECM" "$INI"; do
  [ -f "$f" ] && [ -r "$f" ] || { echo "error: no readable $f" >&2; exit 2; }
done

envs=""
if [ $# -gt 0 ]; then
  for e in "$@"; do
    case "$e" in '' | *[!A-Za-z0-9_.-]*) echo "error: env name '$e' is empty or malformed" >&2; exit 2 ;; esac
    envs="$envs $e"
  done
else
  # configparser ignores what follows a section header's ']' (an inline comment, blanks).
  defined=$(sed -n 's/^\[env:\(warthog-[^]]*\)\].*$/\1/p' "$INI")
  for g in "$DIR"/sdkconfig.warthog-*; do
    [ -f "$g" ] || continue
    e=${g##*/sdkconfig.}
    if printf '%s\n' "$defined" | grep -qxF "$e"; then
      envs="$envs $e"
    else
      echo "skip ${g##*/}: no [env:$e] in platformio.ini"
    fi
  done
  [ -n "$envs" ] || { echo "error: no generated sdkconfig.<env> for a platformio.ini env in $DIR" >&2; exit 2; }
fi

# check_env ENV NSRC SRC... GENERATED: expected lines, later sources winning; =n means "is not set".
# Kconfig hides sdkconfig.defaults' TinyUSB NCM symbols in an ECM build; sdkconfig.ecm must not set any.
check_env() {
  ce=$1 cn=$2
  shift 2
  awk -v nsrc="$cn" -v env="$ce" '
    FNR == 1 { fi++ }
    fi <= nsrc {
      if (match($0, /^CONFIG_[A-Za-z0-9_]+=/)) {
        k = substr($0, 1, RLENGTH - 1); v = substr($0, RLENGTH + 1)
        line = (v == "n") ? "# " k " is not set" : $0
      } else if ($0 ~ /^# CONFIG_[A-Za-z0-9_]+ is not set$/) {
        k = $2; line = $0
      } else next
      if (!(k in want)) order[++n] = k
      want[k] = line; src[k] = fi
      next
    }
    match($0, /^CONFIG_[A-Za-z0-9_]+=/) { got[substr($0, 1, RLENGTH - 1)] = $0; next }
    /^# CONFIG_[A-Za-z0-9_]+ is not set$/ { got[$2] = $0 }
    END {
      for (i = 1; i <= n; i++) {
        k = order[i]
        has = (k in got)
        g = has ? got[k] : "absent"
        if (has) print "PRESENT " k
        if (has && g == want[k]) landed++
        else if (!has && env ~ /-ecm$/ && k ~ /^CONFIG_TINYUSB_NCM_/ && src[k] == 1) hidden++
        else print "MISS " k " " want[k] " did not land (got: " g ")"
      }
      print "COUNT " landed + 0 " " hidden + 0
    }' "$@"
}

fail=0
present=" "
missed=""
for e in $envs; do
  g="$DIR/sdkconfig.$e"
  if [ ! -f "$g" ]; then
    echo "FAIL sdkconfig.$e: missing"
    fail=1
    continue
  elif [ ! -r "$g" ]; then
    echo "FAIL sdkconfig.$e: unreadable"
    fail=1
    continue
  fi
  case "$e" in
    *-ecm) out=$(check_env "$e" 2 "$DEF" "$ECM" "$g") ;;
    *) out=$(check_env "$e" 1 "$DEF" "$g") ;;
  esac
  rc=$?
  present="$present$(printf '%s\n' "$out" | sed -n 's/^PRESENT //p' | tr '\n' ' ')"
  miss=$(printf '%s\n' "$out" | sed -n 's/^MISS //p')
  cnt=$(printf '%s\n' "$out" | sed -n 's/^COUNT //p')
  landed=${cnt% *} hidden=${cnt#* }
  case "$rc $cnt" in
    "0 "[0-9]*" "[0-9]*) ;;
    *) echo "FAIL sdkconfig.$e: the comparison exited $rc with '${cnt:-no COUNT line}'"; fail=1; continue ;;
  esac
  if [ -n "$miss" ]; then
    printf '%s\n' "$miss" | sed "s/^[^ ]* /FAIL sdkconfig.$e: /"
    missed="$missed $(printf '%s\n' "$miss" | cut -d' ' -f1 | tr '\n' ' ')"
    [ "$hidden" = 0 ] || echo "info sdkconfig.$e: $landed lines landed, $hidden hidden by Kconfig dependencies"
    fail=1
  elif [ "$hidden" = 0 ]; then
    echo "ok   sdkconfig.$e: $landed lines landed"
  else
    echo "ok   sdkconfig.$e: $landed lines landed, $hidden hidden by Kconfig dependencies"
  fi
done

# A symbol in no generated file is one Kconfig does not have (removed, renamed or misspelt).
for k in $(printf '%s\n' "$missed" | tr ' ' '\n' | sort -u); do
  case "$present" in *" $k "*) ;; *) echo "FAIL unknown symbol $k (in no generated sdkconfig)" ;; esac
done

if [ "$fail" = 0 ]; then echo "SDKCONFIG LANDING OK"
else
  [ -z "$missed" ] || echo "info for a stale sdkconfig.<env>: rm -f sdkconfig.warthog-* and rebuild (PlatformIO applies sdkconfig.defaults and sdkconfig.ecm only to a new one)"
  echo "SDKCONFIG LANDING FAILED"
fi
exit "$fail"
