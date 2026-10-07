#!/bin/sh
# Flash the mesh-smoke image the moment the XIAO is reachable in ROM download mode.
#
# WHY THIS EXISTS
#   For a board AT+DLMODE (main/dlmode.c) cannot reach: no console, or an image older
#   than 2026-10-07, whose download mode never enumerated after the ROM itself had
#   started the app (an esptool session's end). Recover with the physical dance:
#
#       hold BOOT -> tap RESET -> release BOOT
#
#   then run this script (or run it first and do the dance while it polls).
#
# NOTE: do NOT probe with `esptool chip-id` first. That consumes the transient ROM
#   window -- observed directly: chip-id connected, and the write-flash that followed
#   then failed with "No serial data received". Go straight to write-flash.
cd "$(dirname "$0")/.."
PY="${PIO_PYTHON:-$HOME/.platformio/penv/bin/python3}"
IMG=.pio/build/warthog-mesh-smoke/firmware.factory.bin

[ -f "$IMG" ] || { echo "[autoflash] missing $IMG - build first:"; \
                   echo "  ~/.platformio/penv/bin/pio run -e warthog-mesh-smoke"; exit 1; }

echo "[autoflash] waiting for ROM download mode (hold BOOT, tap RESET, release BOOT)..."
while :; do
  for p in /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB*; do
    [ -e "$p" ] || continue
    if $PY -m esptool --chip esp32s3 --port "$p" --connect-attempts 1 \
         write-flash 0x0 "$IMG" 2>&1 | tee /tmp/autoflash.log | grep -q "Hash of data verified"; then
      echo "[autoflash] FLASHED OK on $p"
      echo "[autoflash] now: uhubctl -l 0-1 -p 2 -a cycle   # cold boot the board"
      exit 0
    fi
  done
  sleep 1
done
