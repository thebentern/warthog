#!/usr/bin/env bash
# Flash a Warthog release `.factory.bin` onto a XIAO ESP32-S3.
#
# Usage:
#   ./flash.sh warthog-vX.Y.Z-REGION.factory.bin [PORT]
#
# Before running: hold the XIAO's BOOT button, tap RESET, release BOOT. USB-OTG
# de-enumerates and the bootloader presents a serial port for esptool.

set -euo pipefail

BIN="${1:-}"
if [[ -z "$BIN" ]]; then
    echo "usage: $0 <warthog-*.factory.bin> [PORT]" >&2
    exit 2
fi
if [[ ! -f "$BIN" ]]; then
    echo "error: $BIN not found" >&2
    exit 2
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

echo "warthog: flashing $BIN -> $PORT"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 460800 write_flash 0x0 "$BIN"
echo "warthog: done. Tap RESET on the XIAO to boot the new firmware."
