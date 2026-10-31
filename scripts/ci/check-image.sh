#!/bin/sh
# Checks each env's images: pioarduino only prints 'esptool merge-bin failed' and the build goes on.
#
# Usage: scripts/ci/check-image.sh ENV...
#   BUILD_DIR (default: .pio/build in the repo), PLATFORMIO_CORE_DIR (default: ~/.platformio),
#   ESPTOOL (default: the core's penv esptool). partitions.bin must hash to scripts/ci/pins.env's PARTITIONS_SHA256.
#   Exit 0 ok, 1 failed, 2 usage (no ENV, or an empty or malformed one) or setup error.

set -u
export LC_ALL=C
unset CDPATH

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
BUILD="${BUILD_DIR:-$ROOT/.pio/build}"
# PlatformIO's core dir as pio reads it: a leading ~ expanded, a relative one made absolute.
CORE=${PLATFORMIO_CORE_DIR:-$HOME/.platformio}
case $CORE in "~"/*) CORE=$HOME/${CORE#"~/"} ;; /*) ;; *) CORE=$PWD/$CORE ;; esac
[ $# -gt 0 ] || { echo "usage: $0 ENV..." >&2; exit 2; }
for e in "$@"; do
  case "$e" in '' | *[!A-Za-z0-9_.-]*) echo "error: env name '$e' is empty or malformed" >&2; exit 2 ;; esac
done
if [ -z "${ESPTOOL:-}" ]; then
  for t in "$CORE/penv/bin/esptool" "$CORE/penv/bin/esptool.py"; do
    [ -x "$t" ] && { ESPTOOL=$t; break; }
  done
fi
[ -n "${ESPTOOL:-}" ] && [ -x "$ESPTOOL" ] || { echo "error: no esptool (set ESPTOOL or PLATFORMIO_CORE_DIR)" >&2; exit 2; }
PSHA=$(sed -n 's/^PARTITIONS_SHA256=\([0-9a-f]\{64\}\)$/\1/p' "$ROOT/scripts/ci/pins.env" 2>/dev/null | head -n 1)
[ -n "$PSHA" ] || { echo "error: no PARTITIONS_SHA256=<sha256> in $ROOT/scripts/ci/pins.env" >&2; exit 2; }

APP=$((0x10000))
PT=$((0x8000))
# The factory row's size in partitions.csv (K, M, hex or decimal).
fsz=$(awk -F, '/^[[:space:]]*#/ {next} {for (i = 1; i <= NF; i++) gsub(/^[[:space:]]+|[[:space:]]+$/, "", $i)}
               $2 == "app" && $3 == "factory" {print $5; exit}' "$ROOT/partitions.csv")
case "$fsz" in
  *[Mm]) FACTORY_MAX=$((${fsz%?} * 1048576)) ;;
  *[Kk]) FACTORY_MAX=$((${fsz%?} * 1024)) ;;
  0x* | 0X* | [0-9]*) FACTORY_MAX=$((fsz)) ;;
  *) echo "error: no factory app row in $ROOT/partitions.csv" >&2; exit 2 ;;
esac

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
size() { wc -c < "$1" | tr -d ' '; }
# not_ff FILE OFF LEN: how many bytes of FILE[OFF, OFF+LEN) are not 0xFF, the fill merge-bin pads with.
not_ff() { dd if="$1" bs=1 skip="$2" count="$3" 2>/dev/null | tr -d '\377' | wc -c | tr -d ' '; }
bad() { echo "FAIL $e: $1"; efail=1; }

fail=0
for e in "$@"; do
  d="$BUILD/$e"
  efail=0
  for f in firmware.bin firmware.factory.bin bootloader.bin partitions.bin; do
    [ -f "$d/$f" ] || bad "no $d/$f"
  done
  if [ "$efail" = 1 ]; then fail=1; continue; fi
  fw="$d/firmware.bin" fac="$d/firmware.factory.bin" bl="$d/bootloader.bin" pt="$d/partitions.bin"
  nfw=$(size "$fw") nfac=$(size "$fac") nbl=$(size "$bl") npt=$(size "$pt")

  [ -z "$(find "$fw" -newer "$fac")" ] || bad "firmware.bin is newer than firmware.factory.bin (a stale factory image)"
  [ "$nfac" = $((APP + nfw)) ] || bad "firmware.factory.bin is $nfac B, want 0x10000 + $nfw = $((APP + nfw)) B"
  [ "$nbl" -lt "$PT" ] || bad "bootloader.bin is $nbl B, past the partition table at 0x8000"
  [ "$nfw" -le "$FACTORY_MAX" ] || bad "firmware.bin is $nfw B, larger than the $fsz factory partition ($FACTORY_MAX B)"
  if [ "$nbl" -lt "$PT" ] && [ $((PT + npt)) -le "$APP" ]; then
    dd if="$fac" bs="$nbl" count=1 2>/dev/null | cmp -s - "$bl" || bad "factory [0x0, $nbl) is not bootloader.bin"
    dd if="$fac" bs=1 skip="$PT" count="$npt" 2>/dev/null | cmp -s - "$pt" || bad "factory [0x8000, +$npt) is not partitions.bin"
    [ "$(not_ff "$fac" "$nbl" $((PT - nbl)))" = 0 ] || bad "factory [$nbl, 0x8000) is not all 0xFF"
    [ "$(not_ff "$fac" $((PT + npt)) $((APP - PT - npt)))" = 0 ] || bad "factory [0x8000 + $npt, 0x10000) is not all 0xFF"
  fi
  dd if="$fac" bs="$APP" skip=1 2>/dev/null | cmp -s - "$fw" || bad "factory [0x10000, end) is not firmware.bin"

  # Header bytes 0, 2, 3: magic 0xE9, DIO (0x02), 8MB in the high nibble and 80m in the low (0x3F).
  for f in "$bl" "$fw"; do
    h=$(od -An -tx1 -N4 "$f" | tr -d ' \n')
    case "$h" in e9??023f) ;; *) bad "${f##*/} header is ${h:-empty} (bytes 0-3), want e9 ?? 02 3f (DIO, 8MB, 80m)" ;; esac
    out=$("$ESPTOOL" --chip esp32s3 image-info "$f" 2>&1)
    rc=$?
    if [ "$rc" != 0 ]; then
      bad "esptool image-info ${f##*/} exited $rc: $(printf '%s\n' "$out" | grep -v '^ *$' | tail -n 1)"
    elif printf '%s\n' "$out" | grep -qi invalid; then
      bad "esptool image-info ${f##*/}: $(printf '%s\n' "$out" | grep -i invalid | tr '\n' ' ')"
    fi
  done

  sum=$(sha256 "$pt")
  [ "$sum" = "$PSHA" ] || bad "partitions.bin sha256 is $sum, want $PSHA (pins.env)"
  if [ "$efail" = 0 ]; then
    echo "ok   $e: factory = 0x10000 + $nfw B, bootloader $nbl B, DIO/80m/8MB, partitions $(echo "$sum" | cut -c1-12)"
  else
    fail=1
  fi
done

if [ "$fail" = 0 ]; then echo "IMAGE CHECK OK"; else echo "IMAGE CHECK FAILED"; fi
exit "$fail"
