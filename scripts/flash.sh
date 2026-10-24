#!/usr/bin/env bash
# Flash a Warthog release `.factory.bin` onto a XIAO ESP32-S3.
#
# Usage:
#   ./flash.sh [--region CC] warthog-vX.Y.Z-VARIANT.factory.bin [PORT]
#
# --region CC also writes warthog-*-region-CC.bin, found beside the .factory.bin, to the
# region partition (0x420000). Without it the board keeps its stored region.
#
# Before running: hold the XIAO's BOOT button, tap RESET, release BOOT. USB-OTG
# de-enumerates and the bootloader presents a serial port for esptool.

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
    [[ "$REGION" =~ ^[A-Z]{2}$ ]] || { echo "error: --region takes a two-letter country code" >&2; exit 2; }
fi

BIN="${1:-}"
[[ -n "$BIN" ]] || usage
if [[ ! -f "$BIN" ]]; then
    echo "error: $BIN not found" >&2
    exit 2
fi

REGION_BIN=""
if [[ -n "$REGION" ]]; then
    shopt -s nullglob
    found=("$(dirname "$BIN")"/warthog-*-region-"$REGION".bin)
    shopt -u nullglob
    if [[ ${#found[@]} -ne 1 ]]; then
        echo "error: want exactly one warthog-*-region-$REGION.bin beside $BIN, found ${#found[@]}" >&2
        echo "       (the release bundle has one per offered country; AT+REGION? lists them)" >&2
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
    echo "error: no serial port found. Hold BOOT + tap RESET on the XIAO, then retry." >&2
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
    echo "warthog: region not written; the board keeps its stored region (none: US until AT+REGION=<CC>)"
fi
echo "warthog: done. Tap RESET on the XIAO to boot the new firmware."
