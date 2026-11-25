#!/usr/bin/env bash
# Flash a Warthog release `.factory.bin` onto the matching XIAO or RadioCore.
#
# Usage:
#   ./flash.sh [--region CC] warthog-vX.Y.Z-VARIANT.factory.bin [PORT]
#
# --region CC also writes warthog-*-region-CC.bin, found beside the .factory.bin, to the
# region partition (0x420000). Without it the board keeps its stored region.
#
# Enter download mode before running this script. On XIAO, hold BOOT, tap
# RESET, then release BOOT. On RadioCore, hold USER, tap R, then release USER.

set -euo pipefail

# regnvs in partitions.csv: 12K at 0x420000.
REGNVS_OFFSET=0x420000
REGNVS_SIZE=12288

usage() {
    echo "usage: $0 [--region CC] <warthog-*.factory.bin> [PORT]" >&2
    exit 2
}

REGION=""
if [[ "${1:-}" == "--region" ]]; then
    [[ $# -ge 2 ]] || usage
    REGION=$(printf '%s' "$2" | tr '[:lower:]' '[:upper:]')
    shift 2
    [[ "$REGION" =~ ^[A-Z]{2}$ ]] || { echo "error: --region takes a two-letter region code" >&2; exit 2; }
fi

BIN="${1:-}"
[[ -n "$BIN" ]] || usage
if [[ ! -f "$BIN" ]]; then
    echo "error: $BIN not found" >&2
    exit 2
fi

REGION_BIN=""
if [[ -n "$REGION" ]]; then
    supported_regions=""
    case "${BIN##*/}" in
        warthog-*-heltec-rc32-68-868.factory.bin) supported_regions="EU" ;;
        warthog-*-heltec-rc32-68.factory.bin) supported_regions="AU CA JP KR NZ US" ;;
        warthog-*-station.factory.bin|warthog-*-mesh-smoke.factory.bin|\
        warthog-*-mesh-sae-meshvif.factory.bin|warthog-*-mesh-sae-swccmp-meshvif.factory.bin)
            supported_regions="AU JP KR NZ US" ;;
    esac
    if [[ -n "$supported_regions" && " $supported_regions " != *" $REGION "* ]]; then
        echo "error: $REGION is unavailable for ${BIN##*/}. Supported regions: $supported_regions" >&2
        exit 2
    fi
    shopt -s nullglob
    found=("$(dirname "$BIN")"/warthog-*-region-"$REGION".bin)
    shopt -u nullglob
    if [[ ${#found[@]} -ne 1 ]]; then
        echo "error: want exactly one warthog-*-region-$REGION.bin beside $BIN, found ${#found[@]}" >&2
        echo "       (the release bundle contains the region images; AT+REGION? lists the board's regions)" >&2
        exit 2
    fi
    REGION_BIN="${found[0]}"
    [[ "$(wc -c < "$REGION_BIN" | tr -d ' ')" = "$REGNVS_SIZE" ]] || { echo "error: $REGION_BIN is not $REGNVS_SIZE bytes" >&2; exit 2; }
    [[ "$(wc -c < "$BIN" | tr -d ' ')" -le $((REGNVS_OFFSET)) ]] || { echo "error: $BIN reaches the region partition" >&2; exit 2; }
fi

PORT="${2:-}"
if [[ -z "$PORT" ]]; then
    for candidate in /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB*; do
        if [[ -e "$candidate" ]]; then
            PORT="$candidate"
            break
        fi
    done
fi
if [[ -z "$PORT" ]]; then
    echo "error: no serial port found. Enter download mode and retry." >&2
    echo "       XIAO: hold BOOT, tap RESET, release BOOT. RadioCore: hold USER, tap R, release USER." >&2
    echo "       Or pass an explicit port: $0 $BIN /dev/cu.usbmodemXXXX" >&2
    exit 1
fi

if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL=esptool.py
elif command -v esptool >/dev/null 2>&1; then
    ESPTOOL=esptool
else
    echo "error: esptool not found. Install with: pip install esptool" >&2
    exit 1
fi

if [[ -n "$REGION_BIN" ]]; then
    echo "warthog: flashing $BIN and region $REGION ($REGION_BIN) -> $PORT"
    "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 460800 write_flash 0x0 "$BIN" "$REGNVS_OFFSET" "$REGION_BIN"
else
    echo "warthog: flashing $BIN -> $PORT"
    "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 460800 write_flash 0x0 "$BIN"
    echo "warthog: the board keeps its stored region and uses the firmware default if none is stored."
fi
echo "warthog: done. Tap RESET on the XIAO or R on the RadioCore to boot."
