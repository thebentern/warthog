#!/usr/bin/env bash
# Region images for the regnvs partition: warthog-TAG-region-CC.bin, one per country the build offers.
#
# Usage: scripts/region-images.sh TAG OUTDIR [SDKCONFIG]
#   SDKCONFIG defaults to sdkconfig.warthog-station, which a station build writes.
# Needs esp-idf-nvs-partition-gen on $PYTHON (release.yml and ci.yml pin it) and IDF's nvs_tool.py.

set -euo pipefail

TAG="${1:?usage: $0 TAG OUTDIR [SDKCONFIG]}"
OUT="${2:?usage: $0 TAG OUTDIR [SDKCONFIG]}"
CFG="${3:-sdkconfig.warthog-station}"
PY="${PYTHON:-python3}"
NVS_TOOL="${NVS_TOOL:-$HOME/.platformio/packages/framework-espidf/components/nvs_flash/nvs_partition_tool/nvs_tool.py}"
# regnvs's size in partitions.csv (12K).
SIZE=0x3000

HW=$(sed -n 's/^CONFIG_WARTHOG_HW_REGIONS="\([A-Z,]*\)"$/\1/p' "$CFG")
[[ "$HW" =~ ^[A-Z]{2}(,[A-Z]{2})*$ ]] || { echo "error: no CONFIG_WARTHOG_HW_REGIONS in $CFG" >&2; exit 1; }
[[ -f "$NVS_TOOL" ]] || { echo "error: $NVS_TOOL not found (a pio build installs it)" >&2; exit 1; }

mkdir -p "$OUT"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
for CC in ${HW//,/ }; do
    BIN="$OUT/warthog-${TAG}-region-${CC}.bin"
    printf 'key,type,encoding,value\nwarthog,namespace,,\nregion,data,string,%s\n' "$CC" > "$WORK/region.csv"
    "$PY" -m esp_idf_nvs_partition_gen generate "$WORK/region.csv" "$BIN" "$SIZE" > "$WORK/gen.log" ||
        { cat "$WORK/gen.log" >&2; exit 1; }
    [[ "$(wc -c < "$BIN" | tr -d ' ')" = $((SIZE)) ]] || { echo "error: $BIN is not $SIZE bytes" >&2; exit 1; }
    "$PY" "$NVS_TOOL" --color never -i -d none "$BIN" > "$WORK/check.txt"
    if ! awk '/^Page no\. [0-9]+[ \t]+CRC32: OK$/ {ok++; next} /^Page Empty$/ || NF == 0 {next} {bad=1}
              END {exit (ok && !bad) ? 0 : 1}' "$WORK/check.txt"; then
        cat "$WORK/check.txt" >&2
        echo "error: $BIN fails nvs_tool's integrity check" >&2
        exit 1
    fi
    "$PY" "$NVS_TOOL" --color never -d minimal -f json "$BIN" | "$PY" -c '
import json, sys
want = [{"namespace": "warthog", "key": "region", "encoding": "string", "data": sys.argv[1]}]
got = [{k: e[k] for k in ("namespace", "key", "encoding", "data")} for e in json.load(sys.stdin)]
sys.exit(0 if got == want else "holds %r, not warthog/region=%s" % (got, sys.argv[1]))' "$CC"
    echo "warthog: $BIN (warthog/region=$CC)"
done
